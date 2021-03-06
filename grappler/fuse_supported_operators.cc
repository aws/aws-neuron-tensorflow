/* Copyright Amazon Web Services and its Affiliates. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/neuron/grappler/fuse_supported_operators.h"
#include "tensorflow/core/grappler/grappler_item.h"
#include "tensorflow/neuron/grappler/convert/convert_graph.h"
#include "tensorflow/neuron/grappler/graph_optimizer_registry.h"

namespace tensorflow {
namespace grappler {
namespace neuron {

constexpr char key_minimum_segment_size[] = "minimum_segment_size";
constexpr char key_fuse_foldable_nodes[] = "fuse_foldable_nodes";
constexpr char key_prune_small_subgraphs_ratio[] =
    "prune_small_subgraphs_ratio";
constexpr char key_supported_op_types[] = "supported_op_types";
constexpr char key_no_fuse_ops[] = "no_fuse_ops";
constexpr char key_force_fuse_ops[] = "force_fuse_ops";

template <class T>
static std::string container_debug_string(const T& container) {
  std::string debug_string;
  for (const auto& item : container) {
    debug_string += ",";
    debug_string += item;
  }
  return debug_string.substr(0 < debug_string.size() ? 1 : 0);
}

Status FuseSupportedOperators::Init(
    const tensorflow::RewriterConfig_CustomGraphOptimizer* config) {
  const auto& parameter_map = config->parameter_map();
  if (parameter_map.count(key_minimum_segment_size)) {
    minimum_segment_size_ = parameter_map.at(key_minimum_segment_size).i();
  }
  if (parameter_map.count(key_fuse_foldable_nodes)) {
    fuse_foldable_nodes_ = parameter_map.at(key_fuse_foldable_nodes).b();
  }
  if (parameter_map.count(key_prune_small_subgraphs_ratio)) {
    prune_small_subgraphs_ratio_ =
        parameter_map.at(key_prune_small_subgraphs_ratio).f();
  }
  if (!parameter_map.count(key_supported_op_types)) {
    return errors::InvalidArgument(
        name_optimizer,
        " requires providing a list of supported operator names");
  }
  const auto& param_supported_op_types =
      parameter_map.at(key_supported_op_types).list().s();
  supported_op_types_ = {param_supported_op_types.begin(),
                         param_supported_op_types.end()};
  VLOG(2) << "supported_op_types_ "
          << container_debug_string(supported_op_types_);
  if (parameter_map.count(key_no_fuse_ops)) {
    const auto& param_no_fuse_ops =
        parameter_map.at(key_no_fuse_ops).list().s();
    no_fuse_ops_ = {param_no_fuse_ops.begin(), param_no_fuse_ops.end()};
  }
  VLOG(2) << "no_fuse_ops_ " << container_debug_string(no_fuse_ops_);
  if (parameter_map.count(key_force_fuse_ops)) {
    const auto& param_force_fuse_ops =
        parameter_map.at(key_force_fuse_ops).list().s();
    force_fuse_ops_ = {param_force_fuse_ops.begin(),
                       param_force_fuse_ops.end()};
  }
  VLOG(2) << "force_fuse_ops_ " << container_debug_string(force_fuse_ops_);
  return Status::OK();
}

Status FuseSupportedOperators::Optimize(Cluster* cluster,
                                        const GrapplerItem& item,
                                        GraphDef* output) {
  if (cluster == nullptr) {
    return errors::InvalidArgument("cluster == nullptr");
  }
  std::vector<std::string> input_op_names;
  for (const auto& feed : item.feed) {
    input_op_names.push_back(feed.first);
  }
  VLOG(2) << "input_op_names " << container_debug_string(input_op_names);
  VLOG(2) << "output_op_names " << container_debug_string(item.fetch);
  TF_RETURN_IF_ERROR(tensorflow::neuron::convert::CreateNeuronGraphDef(
      output, item.graph, input_op_names, item.fetch, fuse_foldable_nodes_,
      minimum_segment_size_, prune_small_subgraphs_ratio_, supported_op_types_,
      no_fuse_ops_, force_fuse_ops_));
  return Status::OK();
}

void FuseSupportedOperators::Feedback(Cluster* cluster,
                                      const GrapplerItem& item,
                                      const GraphDef& optimize_output,
                                      double result) {
  // Nothing to do for FuseSupportedOperators.
}

REGISTER_NEURON_GRAPH_OPTIMIZER_AS(FuseSupportedOperators, name_optimizer);

}  // end namespace neuron
}  // end namespace grappler
}  // end namespace tensorflow
