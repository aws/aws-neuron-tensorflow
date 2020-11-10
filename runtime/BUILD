package(default_visibility = ["//visibility:public"])

load("@com_google_protobuf//:protobuf.bzl", "cc_proto_library")
load("@org_tensorflow//tensorflow:tensorflow.bzl", "tf_gen_op_libs")
load("@org_tensorflow//tensorflow:tensorflow.bzl", "tf_kernel_library")


cc_library(
    name = "all_ops",
    deps = [
        ":neuron_op_op_lib",
        ":neuron_op_kernel",
    ],
)

tf_gen_op_libs(
    op_lib_names = ["neuron_op"],
)

tf_kernel_library(
    name = "neuron_op_kernel",
    srcs = ["kernels/neuron_op.cc"],
    hdrs = ["kernels/neuron_op.h"],
    deps = [
        ":device",
    ],
)

cc_library(
    name = "device",
    srcs = [
        "device.cc",
        "timestamps.h",
        "profiler.h",
        "profiler.cc",
        "tensor_util.h",
        "tensor_util.cc",
        "shared_memory_io.h",
        "shared_memory.h",
        "shared_memory.cc",
        "runtime_grpc.h",
        "runtime_grpc.cc",
        "runtime_io.h",
        "semaphore.h",
        "semaphore.cc",
    ],
    hdrs = ["device.h"],
    deps = [
        "@org_tensorflow//tensorflow/core:framework",
        "@org_tensorflow//tensorflow/core:lib",
        ":nmgr_service.grpc.o",
        ":macros",
        "@grpc//:grpc++_codegen_base",
    ],
)

cc_library(
    name = "macros",
    srcs = ["macros.h"],
    hdrs = ["macros.h"],
)

# nrt proto libraries
cc_proto_library(
    name = "nmgr_service.grpc.o",
    srcs = [
        "proto/nmgr_service.proto",
        "proto/nmgr_session_service.proto",
        "proto/nmgr.proto",
        "proto/nmetric.proto",
        "proto/common.proto",
        "proto/nerr.proto",
    ],
    protoc = "@com_google_protobuf//:protoc",
    include = "proto/",
    use_grpc_plugin = True,
    default_runtime = "@com_google_protobuf//:protobuf",
)