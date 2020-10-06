/* Copyright 2019, Amazon.com, Inc. or its affiliates. All Rights Reserved. */

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef NEURONTFSERV
#include <csignal>
#endif  // NEURONTFSERV
#include "tensorflow/neuron/runtime/device.h"


namespace tensorflow {
namespace neuron {


#define TF_LOG_IF_ERROR(status) {   \
    if (!(status).ok()) {           \
        LOG(ERROR) << (status);     \
    }                               \
}


NeuronDeviceManager global_neuron_device_manager;


#ifdef NEURONTFSERV
void sigint_handler(int sig) {
    global_neuron_device_manager.clear_from_global_state();
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
    std::raise(sig);
}
#endif // NEURONTFSERV


class ShmFile {
public:
    ShmFile(const std::string &name) {
        name_ = name;
        mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
        shm_open_fd_ = ::shm_open(name.c_str(), O_CREAT | O_RDWR, mode);
        if (shm_open_fd_ >= 0) {
            if (::fchmod(shm_open_fd_, mode) < 0) {
                shm_open_fd_ = -1;
            } else {
                shm_fd_ = shm_open_fd_;
            }
        }
    }
    ~ShmFile() {
        if (shm_open_fd_ >= 0) {
            ::close(shm_open_fd_);
            SYS_FAIL_LOG(shm_unlink(name_.c_str()) < 0, "shm_unlink");
        }
    }
    ShmFile(const ShmFile &shm_file) = delete;
    ShmFile &operator=(const ShmFile &shm_file) = delete;
    ShmFile(ShmFile &&) = delete;
    ShmFile &operator=(ShmFile &&) = delete;
    int shm_fd_ = -1;
private:
    int shm_open_fd_ = -1;
    std::string name_;
};


static std::string gen_shm_path() {
    std::string filename = "/neuron_clib_";
    for (size_t i = 0; i < 64; ++i) {
        if (Env::Default()->CreateUniqueFileName(&filename, "")) {
            return filename;
        }
        Env::Default()->SleepForMicroseconds(1);
    }
    return "";
}


SharedMemoryBuffer::SharedMemoryBuffer(const size_t id, const size_t size, const uint64_t session_id,
                                       std::shared_ptr<RuntimeGRPC> runtime) : id_(id) {
    VLOG(1) << "entering SharedMemoryBuffer constructor";
    if (nullptr == runtime) {
        LOG(ERROR) << "runtime is not initialized";
        return;
    }
    runtime_ = runtime;
    std::string path = gen_shm_path();
    if (path.empty()) {
        LOG(ERROR) << "cannot generate unique file name for shared memory";
        return;
    }
    ShmFile shm_file(path);
    SYS_FAIL_LOG_RETURN(shm_file.shm_fd_ < 0, "shm_open");
    SYS_FAIL_LOG_RETURN(::ftruncate(shm_file.shm_fd_, size) < 0, "ftruncate");
    ptr_ = static_cast<char*>(::mmap(0, size, PROT_WRITE, MAP_SHARED, shm_file.shm_fd_, 0));
    size_ = size;
    SYS_FAIL_LOG_RETURN(nullptr == ptr_, "mmap");
    if (!runtime_->shm_map(path, PROT_READ | PROT_WRITE, session_id).ok()) {
        VLOG(1) << "neuron-rtd shm_map failed";
        unsupported_by_runtime_ = true;
        return;
    }
    VLOG(1) << "allocated shared memory buffer " << path;
    path_ = path;
}

SharedMemoryBuffer::~SharedMemoryBuffer() {
    VLOG(1) << "entering destructor of SharedMemoryBuffer " << path_;
    if (!path_.empty()) {
        TF_LOG_IF_ERROR(runtime_->shm_unmap(path_, PROT_READ | PROT_WRITE));
    }
    if (nullptr != ptr_) {
        SYS_FAIL_LOG(munmap(ptr_, size_) < 0, "munmap");
    }
}

SharedMemoryBuffer::SharedMemoryBuffer(const SharedMemoryBuffer &buffer) : id_(buffer.id_) {
    runtime_ = buffer.runtime_;
    ptr_ = buffer.ptr_;
    size_ = buffer.size_;
    path_ = buffer.path_;
}

SharedMemoryBuffer::SharedMemoryBuffer(SharedMemoryBuffer &&buffer) : id_(buffer.id_) {
    runtime_ = buffer.runtime_;
    ptr_ = buffer.ptr_;
    size_ = buffer.size_;
    path_ = buffer.path_;
    buffer.runtime_ = nullptr;
    buffer.ptr_ = nullptr;
    buffer.size_ = 0;
    buffer.path_.clear();
}


SharedMemoryBufferManager::SharedMemoryBufferManager(const uint64_t session_id,
                                                     const std::string &nrtd_address)
                                                     : session_id_(session_id) {
    runtime_ = std::make_shared<RuntimeGRPC>();
    TF_LOG_RETURN_IF_ERROR(runtime_->initialize(nrtd_address));
    is_valid_ = true;
}


SharedMemoryPtr SharedMemoryBufferManager::allocate_shm(const size_t size) {
    if (!is_valid_) {
        VLOG(1) << "SharedMemoryBufferManager is invalid";
        return nullptr;
    }
    tensorflow::mutex_lock lock(mutex_);
    if (!is_valid_) {
        // check once more to kick out threads that have already accessed is_valid_ before the lock
        return nullptr;
    }
    if (size_to_free_buffer_id_.count(size) && size_to_free_buffer_id_[size].size()) {
        // get one from the free buffer set
        VLOG(1) << "getting an already allocated shm buffer";
        std::unordered_set<size_t> *free_buffer_id_set = &size_to_free_buffer_id_[size];
        auto iter = free_buffer_id_set->begin();
        size_t free_buffer_id = *iter;
        free_buffer_id_set->erase(iter);
        return buffer_vec_[free_buffer_id];
    }
    VLOG(1) << "allocating a new shm buffer";
    size_t id = buffer_vec_.size();
    buffer_vec_.push_back(std::make_shared<SharedMemoryBuffer>(id, size, session_id_, runtime_));
    if (!buffer_vec_.back()->is_valid()) {
        if (buffer_vec_.back()->unsupported_by_runtime()) {
            LOG(INFO) << "The current Neuron runtime configuration does not support "
                         "shared memory data transfer. Please refer to "
                         "https://github.com/aws/aws-neuron-sdk/blob/master/docs/neuron-runtime/nrt-theory-of-operation.md#shared-memory-for-inference-ifmaps-and-ofmaps "
                         "if you encounter performance problem caused by high CPU usage on inf1 instances.";
            is_valid_ = false;
        }
        buffer_vec_.pop_back();
        VLOG(1) << "SharedMemoryBufferManager created an invalid buffer";
        return nullptr;
    }
    return buffer_vec_.back();
}

void SharedMemoryBufferManager::free_shm(SharedMemoryPtr shm) {
    tensorflow::mutex_lock lock(mutex_);
    if (!shm->is_valid()) {
        LOG(ERROR) << "SharedMemoryBufferManager cannot free an invalid shared memory buffer";
        return;
    }
    VLOG(1) << "freeing shm buf " << *shm->get_path();
    size_t size = shm->get_size();
    if (!size_to_free_buffer_id_.count(size)) {
        size_to_free_buffer_id_.emplace(
            std::piecewise_construct, std::forward_as_tuple(size), std::forward_as_tuple());
    }
    size_to_free_buffer_id_[size].insert(shm->get_id());
}

void SharedMemoryBufferManager::clear() {
    tensorflow::mutex_lock lock(mutex_);
    size_to_free_buffer_id_.clear();
    buffer_vec_.clear();
    is_valid_ = false;
}


static std::string remove_pattern(std::string data, const std::string &pattern) {
    size_t string_length = data.size();
    size_t pos = 0;
    for (size_t idx = 0; idx < string_length; ++idx) {
        pos = data.find(pattern, pos);
        if (std::string::npos == pos) {
            break;
        }
        data.replace(pos, pattern.size(), "");
    }
    return data;
}

NeuronDeviceManager::~NeuronDeviceManager() {
    tensorflow::mutex_lock lock(global_mutex_);
    clear_from_global_state();
}

Status NeuronDeviceManager::initialize(const int64_t opt_device_size,
                                       const int64_t max_num_duplicates) {
    if (!path_set_) {
        // append /opt/aws/neuron/bin to PATH
        std::string env_path = env_get("PATH", "");
        setenv("PATH", (env_path + ":/opt/aws/neuron/bin").c_str(), 1);
        path_set_ = true;
    }

    // neuron-rtd address
    nrtd_address_ = env_get("NEURON_RTD_ADDRESS", "unix:/run/neuron.sock");

    TF_RETURN_IF_ERROR(session_.initialize(nrtd_address_));

    // get number of neuron cores from comma-separated list of integers
    std::string neuron_device_sizes_raw = env_get("NEURONCORE_GROUP_SIZES", "");
    if (neuron_device_sizes_raw.empty()) {
        TF_RETURN_IF_ERROR(init_default_device(opt_device_size, max_num_duplicates));
    } else {
        // remove [ and ]
        std::string neuron_device_sizes = remove_pattern(neuron_device_sizes_raw, "[");
        neuron_device_sizes = remove_pattern(neuron_device_sizes, "]");

        std::vector<int> num_cores_req_vector;
        std::vector<int> num_dup_vector;
        std::stringstream neuron_device_sizes_stream(neuron_device_sizes);
        for (size_t idx = 0; idx < MAX_NUM_CORES; ++idx) {
            if (!neuron_device_sizes_stream.good()) {
                break;
            }
            std::string device_spec;
            std::getline(neuron_device_sizes_stream, device_spec, ',');
            if (device_spec.empty()) {
                continue;
            }
            int num_dup = 1;
            if (device_spec.find("x") != std::string::npos) {
                size_t delim_pos = device_spec.find("x");
                num_dup = stoi_no_throw(device_spec.substr(0, delim_pos));
                device_spec = device_spec.substr(delim_pos + 1, std::string::npos);
            }
            int num_cores_req = stoi_no_throw(device_spec);
            if (num_cores_req < 0 || num_cores_req > MAX_NUM_CORES || num_dup <= 0 || num_dup > MAX_NUM_CORES) {
                LOG(WARNING) << "NEURONCORE_GROUP_SIZES=" << neuron_device_sizes_raw
                             << " looks ill-formatted. Falling back to initializing"
                             << " a default NeuronCore Group.";
                num_cores_req_vector.clear();
                num_dup_vector.clear();
                break;
            }
            num_cores_req_vector.push_back(num_cores_req);
            num_dup_vector.push_back(num_dup);
        }
        if (num_cores_req_vector.empty()) {
            TF_RETURN_IF_ERROR(init_default_device(opt_device_size, max_num_duplicates));
        } else {
            TF_RETURN_IF_ERROR(init_devices(num_cores_req_vector, num_dup_vector));
        }
    }
    ready_ = true;
    return Status::OK();
}

Status NeuronDeviceManager::init_devices(const std::vector<int> &num_cores_req_vector,
                                         const std::vector<int> &num_dup_vector) {
    Status status = errors::ResourceExhausted("No NeuronCore Group can be initialized.");
    for (size_t idx = 0; idx < num_cores_req_vector.size(); ++idx) {
        int num_cores_req = num_cores_req_vector[idx];
        int num_dup;
        if (num_dup_vector.size() == num_cores_req_vector.size()) {
            num_dup = num_dup_vector[idx];
        } else {
            num_dup = 1;
        }
        status = device_array_[idx].initialize(
            nrtd_address_, num_cores_req, num_dup, session_.get_id());
        if (!status.ok()) {
            if (status.code() != tensorflow::error::Code::ABORTED) {
                LOG(WARNING) << "Cannot initialize NeuronCore Group with " << num_cores_req
                             << " cores; stopping initialization.";
            }
            break;
        }
        ++num_devices_;
        VLOG(1) << "successfully initialized NeuronCore Group of size " << num_cores_req;
    }
    if (0 == num_devices_) {
        return status;
    }
    return Status::OK();
}

Status NeuronDeviceManager::init_default_device(const int64_t opt_device_size,
                                                const int64_t max_num_duplicates) {
    std::vector<int> num_cores_req_vector = {DEFAULT_NUM_CORES};
    std::vector<int> num_dup_vector({1});
    if (0 <= opt_device_size && opt_device_size <= 64) {
        // get one full Inferentia by default
        if (opt_device_size == 1) {
            if (4 == max_num_duplicates) {
                num_cores_req_vector = {1};
                num_dup_vector = {4};
            } else if (3 == max_num_duplicates) {
                num_cores_req_vector = {1};
                num_dup_vector = {3};
            } else if (2 == max_num_duplicates) {
                num_cores_req_vector = {1, 1};
                num_dup_vector = {2, 2};
            } else {
                num_cores_req_vector = {1, 1, 1, 1};
                num_dup_vector = {};
            }
        } else if (opt_device_size == 2) {
            if (2 == max_num_duplicates) {
                num_cores_req_vector = {2};
                num_dup_vector = {2};
            } else {
                num_cores_req_vector = {2, 2};
                num_dup_vector = {};
            }
        }
    }
    return init_devices(num_cores_req_vector, num_dup_vector);
}

void NeuronDeviceManager::clear_if_empty() {
    tensorflow::mutex_lock lock(global_mutex_);
    bool empty = true;
    for (size_t idx = 0; idx < num_devices_; ++idx) {
        if (0 != device_array_[idx].num_executable()) {
            empty = false;
        }
    }
    if (empty) {
        clear();
    }
}

void NeuronDeviceManager::clear() {
    for (size_t idx = 0; idx < num_devices_; ++idx) {
        device_array_[idx].clear();
    }
    num_devices_ = 0;
    device_index_ = 0;
    ready_ = false;
    VLOG(1) << "NeuronDeviceManager is cleared";
}

void NeuronDeviceManager::clear_from_global_state() {
    for (size_t idx = 0; idx < num_devices_; ++idx) {
        device_array_[idx].clear(true);
    }
    num_devices_ = 0;
    device_index_ = 0;
    ready_ = false;
    VLOG(1) << "NeuronDeviceManager is cleared from global state";
}


Status NeuronDeviceManager::apply_for_device(NeuronDevice **device,
                                             const int64_t opt_device_size,
                                             const int64_t max_num_duplicates,
                                             const int64_t device_index) {
    tensorflow::mutex_lock lock(global_mutex_);
    if (!ready_) {
        TF_RETURN_IF_ERROR(initialize(opt_device_size, max_num_duplicates));
#ifdef NEURONTFSERV
        std::signal(SIGINT, sigint_handler);
        std::signal(SIGTERM, sigint_handler);
#endif // NEURONTFSERV
    }

    if (0 <= device_index && device_index < (int64_t)num_devices_) {
        *device = &device_array_[device_index];
        return Status::OK();
    }
    *device = &device_array_[device_index_];
    ++device_index_;
    if (device_index_ >= num_devices_) {
        device_index_ = 0;
    }
    return Status::OK();
}

Status NeuronDevice::initialize(const std::string &nrtd_address,
                                const int num_cores_req,
                                const int num_dup,
                                const uint64_t session_id) {
    tensorflow::mutex_lock lock(mutex_eg_);
    if (closed_) {
        return errors::Aborted("neuron_device is closed");
    }
    nrtd_address_ = nrtd_address;
    TF_RETURN_IF_ERROR(runtime_.initialize(nrtd_address_));
    if (RuntimeSession::INVALID_ID != session_id) {
        session_id_ = session_id;
    }
    if (num_dup == 1) {
        uint32_t eg_id = NRT_INVALID_EG_ID;
        TF_RETURN_IF_ERROR(runtime_.create_eg(&eg_id, &num_cores_, num_cores_req, session_id));
        vec_eg_id_.push_back(eg_id);
    } else {
        // setup device to duplicate models automatically
        for (int idx = 0; idx < num_dup; ++idx) {
            uint32_t eg_id = NRT_INVALID_EG_ID;
            uint32_t num_cores = 0;
            TF_RETURN_IF_ERROR(runtime_.create_eg(&eg_id, &num_cores, num_cores_req, session_id));
            if (num_cores != 1) {
                return errors::InvalidArgument(
                    "NeuronCore group size ", num_cores, " is not allowed in model duplication mode");
            }
            vec_eg_id_.push_back(eg_id);
            num_cores_ = num_cores_req;
        }
    }
    running_nn_id_ = NRT_INVALID_NN_ID;
    std::string nrt_shm_map = env_get("NEURON_RTD_SHM_MAP", "");
    if ("no" != nrt_shm_map) {
        shm_buf_mgr_ = std::make_shared<SharedMemoryBufferManager>(session_id, nrtd_address);
        if (!shm_buf_mgr_->is_valid()) {
            shm_buf_mgr_ = nullptr;
        }
    }
    return Status::OK();
}

Status NeuronDevice::load(
        uint32_t *nn_id, const StringPiece &executable,
        const uint32_t timeout, const uint32_t ninfer, const bool profile_enabled) {
    tensorflow::mutex_lock lock(mutex_eg_);
    if (closed_) {
        return errors::Aborted("neuron_device is closed");
    }
    uint32_t first_nn_id = NRT_INVALID_NN_ID;
    std::vector<uint32_t> all_nn_ids;
    if (vec_eg_id_.size() == 1) {
        TF_RETURN_IF_ERROR(runtime_.load(
            &first_nn_id, vec_eg_id_[0], executable, timeout, ninfer,
            profile_enabled, session_id_));
        all_nn_ids.push_back(first_nn_id);
    } else if (vec_eg_id_.size() > 1) {
        Status status;
        for (const uint32_t eg_id : vec_eg_id_) {
            uint32_t this_nn_id = NRT_INVALID_NN_ID;
            status = runtime_.load(&this_nn_id, eg_id, executable, timeout, ninfer,
                                   profile_enabled, session_id_);
            if (!status.ok()) {
                LOG(WARNING) << "stop duplicating nn " << first_nn_id
                             << " due to error " << status.error_message();
                break;
            }
            if (all_nn_ids.size() == 0) {
                TF_RETURN_IF_ERROR(status);
                first_nn_id = this_nn_id;
            } else {
                VLOG(1) << "duplicated " << first_nn_id << " as " << this_nn_id;
            }
            all_nn_ids.push_back(this_nn_id);
        }
        if (all_nn_ids.size() == 0) {
            return status;
        }
    } else {
        return errors::Unavailable("NeuronDevice is uninitialized");
    }
    if (nn_id_to_all_nn_ids_.count(first_nn_id)) {
        for (const uint32_t nid : all_nn_ids) {
            TF_LOG_IF_ERROR(runtime_.unload(nid));
        }
        return errors::AlreadyExists("nn ", first_nn_id, " is already mapped");
    }
    nn_id_to_all_nn_ids_[first_nn_id] = all_nn_ids;
    nn_id_to_active_idx_[first_nn_id] = 0;
    *nn_id = first_nn_id;
    VLOG(1) << "successfully loaded " << first_nn_id;
    return Status::OK();
}

void NeuronDevice::unload(const uint32_t nn_id) {
    tensorflow::mutex_lock lock(mutex_eg_);
    if (closed_) {
        return;
    }
    if (!nn_id_to_all_nn_ids_.count(nn_id)) {
        VLOG(1) << "model " << nn_id << " is not loaded";
        return;
    }
    // stop
    if (running(nn_id)) {
        // stop all models
        for (const uint32_t nid : nn_id_to_all_nn_ids_[nn_id]) {
            TF_LOG_IF_ERROR(runtime_.stop(nid));
        }
        set_running(NRT_INVALID_NN_ID);
    }

    // unload all models
    for (const uint32_t nid : nn_id_to_all_nn_ids_[nn_id]) {
        TF_LOG_IF_ERROR(runtime_.unload(nid));
    }
    nn_id_to_all_nn_ids_.erase(nn_id);
    VLOG(1) << "unload: number of NEFFs: " << num_executable();
}

Status NeuronDevice::setup_infer_post(RuntimeIO *runtime_io, int64_t post_tag) {
    uint32_t active_nn_id = NRT_INVALID_NN_ID;
    TF_RETURN_IF_ERROR(get_active(&active_nn_id, runtime_io->get_nn_id()));
    runtime_io->set_nn_id(active_nn_id);
    return runtime_.setup_infer_post(runtime_io, post_tag);
}

Status NeuronDevice::post_infer_post(RuntimeIO *runtime_io) {
    return runtime_.post_infer_post(runtime_io);
}

Status NeuronDevice::wait_infer_post(RuntimeIO *runtime_io) {
    return runtime_.wait_infer_post(runtime_io);
}

Status NeuronDevice::setup_infer(RuntimeIO *runtime_io, int64_t post_tag) {
    uint32_t active_nn_id = NRT_INVALID_NN_ID;
    TF_RETURN_IF_ERROR(get_active(&active_nn_id, runtime_io->get_nn_id()));
    runtime_io->set_nn_id(active_nn_id);
    return runtime_.setup_infer(runtime_io, post_tag);
}

Status NeuronDevice::post_infer(RuntimeIO *runtime_io) {
    return runtime_.post_infer(runtime_io);
}

Status NeuronDevice::wait_infer(RuntimeIO *runtime_io) {
    return runtime_.wait_infer(runtime_io);
}

Status NeuronDevice::infer(RuntimeIO *runtime_io, Timestamps *timestamps,
                           ProfilerInterface *profile, const uint32_t nn_id) {
    tensorflow::mutex_lock lock(mutex_eg_);
    TF_RETURN_IF_ERROR(start_model_unsafe(nn_id));
    if (profile->enabled_) profile->start_session(nrtd_address_, nn_id);
    Status status_post = runtime_.infer_post(runtime_io);
    Status status_wait = runtime_.infer_wait(runtime_io);
    if (profile->enabled_) profile->stop_session();
    TF_RETURN_IF_ERROR(status_post);
    return status_wait;
}

Status NeuronDevice::infer_post(RuntimeIO *runtime_io, SemResQueue *sem_res_queue,
                                xla::Semaphore *infer_sem, Timestamps *timestamps,
                                const uint32_t nn_id) {
    tensorflow::mutex_lock lock(mutex_eg_);
    sem_res_queue->push(infer_sem->ScopedAcquire(1));
    return infer_post_unsafe(runtime_io, timestamps, nn_id);
}

void NeuronDevice::acquire_mutex(std::queue<tensorflow::mutex_lock> *mutex_lock_queue) {
    mutex_lock_queue->emplace(mutex_eg_);
}

Status NeuronDevice::infer_post_unsafe(RuntimeIO *runtime_io, Timestamps *timestamps,
                                       const uint32_t nn_id) {
    TF_RETURN_IF_ERROR(start_model_unsafe(nn_id));
    if (nullptr != timestamps) timestamps->mark_above_nrtd_infer();
    uint32_t active_nn_id = NRT_INVALID_NN_ID;
    TF_RETURN_IF_ERROR(get_active(&active_nn_id, runtime_io->get_nn_id()));
    runtime_io->set_nn_id(active_nn_id);
    return runtime_.infer_post(runtime_io);
}

Status NeuronDevice::infer_wait(RuntimeIO *runtime_io, Timestamps *timestamps) {
    TF_RETURN_IF_ERROR(runtime_.infer_wait(runtime_io));
    if (nullptr != timestamps) timestamps->mark_below_nrtd_infer();
    return Status::OK();
}

void NeuronDevice::clear(bool from_global_state) {
    tensorflow::mutex_lock lock(mutex_eg_);
    if (closed_) {
        return;
    }
    if (from_global_state) {
        closed_ = true;
    }
    for (const auto &nn_id_pair : nn_id_to_all_nn_ids_) {
        const uint32_t nn_id = nn_id_pair.first;
        const std::vector<uint32_t> &all_nn_ids = nn_id_pair.second;
        if (running(nn_id)) {
            // stop all models
            for (const uint32_t nid : all_nn_ids) {
                TF_LOG_IF_ERROR(runtime_.stop(nid));
            }
        }
        // unload all models
        for (const uint32_t nid : all_nn_ids) {
            TF_LOG_IF_ERROR(runtime_.unload(nid, from_global_state));
        }
        VLOG(1) << "unload from NeuronDevice::clear";
    }
    for (const uint32_t eg_id : vec_eg_id_) {
        TF_LOG_IF_ERROR(runtime_.destroy_eg(eg_id, from_global_state));
    }
    VLOG(1) << "destroy_eg from NeuronDevice::clear";
    if (!from_global_state) {
        set_running(NRT_INVALID_NN_ID);
        nn_id_to_all_nn_ids_.clear();
        shm_buf_mgr_->clear();
        vec_eg_id_.clear();
    }
}

Status NeuronDevice::start_ping(const uint32_t nn_id) {
    if (closed_) {
        return errors::Aborted("neuron_device is closed");
    }
    return runtime_.start_ping(nn_id);
}

Status NeuronDevice::start_model_unsafe(const uint32_t nn_id) {
    if (closed_) {
        return errors::Aborted("neuron_device is closed");
    }
    if (!running(nn_id) && is_busy()) {
        // if nn_id is not running, stop the current running model
        std::queue<RuntimeStopper> stopper_queue;
        for (const uint32_t nid : nn_id_to_all_nn_ids_[nn_get_current_running()]) {
            stopper_queue.emplace();
            TF_RETURN_IF_ERROR(runtime_.post_stop(&stopper_queue.back(), nid));
        }
        for (const uint32_t nid : nn_id_to_all_nn_ids_[nn_get_current_running()]) {
            TF_RETURN_IF_ERROR(runtime_.wait_stop(&stopper_queue.front()));
            stopper_queue.pop();
            VLOG(1) << "stopped model " << nid;
        }
        set_running(NRT_INVALID_NN_ID);
    }
    if (!is_busy()) {
        // if no model is running, start nn_id
        std::queue<RuntimeStarter> starter_queue;
        for (const uint32_t nid : nn_id_to_all_nn_ids_[nn_id]) {
            starter_queue.emplace();
            TF_RETURN_IF_ERROR(runtime_.post_start(&starter_queue.back(), nid));
        }
        for (const uint32_t nid : nn_id_to_all_nn_ids_[nn_id]) {
            TF_RETURN_IF_ERROR(runtime_.wait_start(&starter_queue.front()));
            starter_queue.pop();
            VLOG(1) << "started model " << nid;
        }
        set_running(nn_id);
    }
    return Status::OK();
}

bool NeuronDevice::is_busy() {
    return running_nn_id_ != NRT_INVALID_NN_ID;
}

bool NeuronDevice::running(uint32_t nn_id) {
    return running_nn_id_ == nn_id && NRT_INVALID_NN_ID != running_nn_id_;
}

uint32_t NeuronDevice::nn_get_current_running() {
    return running_nn_id_;
}

void NeuronDevice::set_running(uint32_t nn_id) {
    running_nn_id_ = nn_id;
}

Status NeuronDevice::get_active(uint32_t *active_nn_id, const uint32_t nn_id) {
    if (!nn_id_to_all_nn_ids_.count(nn_id)) {
        return errors::InvalidArgument("no active id can be found from nn id ", nn_id);
    }
    size_t idx = nn_id_to_active_idx_[nn_id];
    nn_id_to_active_idx_[nn_id] = (idx + 1) % nn_id_to_all_nn_ids_[nn_id].size();
    *active_nn_id = nn_id_to_all_nn_ids_[nn_id][idx];
    return Status::OK();
}


std::string env_get(const char *env_var, const char *default_env_var) {
    char *str = std::getenv(env_var);
    return str ? str : default_env_var;
}

int stoi_no_throw(const std::string &str) {
    try {
        return std::stoi(str);
    } catch (std::invalid_argument e) {
        return STOI_INVALID_RESULT;
    } catch (std::out_of_range e) {
        return STOI_INVALID_RESULT;
    }
}


}  // namespace neuron
}  // namespace tensorflow
