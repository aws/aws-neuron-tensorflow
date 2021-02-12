# Copyright Amazon Web Services and its Affiliates. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
from collections import abc
from tensorflow.core.protobuf import config_pb2
from tensorflow.core.protobuf import meta_graph_pb2
from tensorflow.python.eager import function
from tensorflow.python.eager import def_function
from tensorflow.python.eager import wrap_function
from tensorflow.python.framework import convert_to_constants
from tensorflow.python.framework import ops
from tensorflow.python.grappler import tf_optimizer
from tensorflow.python.keras.engine.training import Model
from tensorflow.python.platform import tf_logging as logging
from tensorflow.python.util import nest
from tensorflow.neuron.python import meta_graph_util as mgu
from tensorflow.neuron.python import graph_def_util as gdu
from tensorflow.neuron.python import utils
from tensorflow.neuron.python.neuron_cc import list_operators


def trace(func, example_inputs, subgraph_builder_function=None):
    """Convert a function to a Neuron-optimized `keras.Model`.

    Args:
        func: The function to be converted.
        example_inputs: A `tf.Tensor` or a tuple/list of `tf.Tensor`s for tracing the function.

    Returns:
        A Neuron-optimized `keras.Model`.
    """
    if isinstance(func, def_function.Function):
        original_func = func
        func = func.get_concrete_function(example_inputs)
    elif isinstance(func, function.ConcreteFunction):
        original_func = func
    else:
        original_func = def_function.function(func)
        func = original_func.get_concrete_function(example_inputs)

    # Note: input_names is also used for constructing the output ConcreteFunction,
    # where using a dictionary (such as `{ts.name: ts.name for ts in func_inputs}`) can cause
    # the resulted ConcreteFunction to get feed tensors going to the wrong inputs occationally
    captured_inputs = {ts.name for _, ts in func.graph.captures}
    input_names = [ts.name for ts in func.inputs if ts.name not in captured_inputs]

    # convert all variables to constants
    with utils.change_grappler_logging_level_according_to_cc_flags():
        cfunc = convert_to_constants.convert_variables_to_constants_v2(func)
    graph_def = cfunc.graph.as_graph_def(add_shapes=True)
    original_graph_def = graph_def

    # encode known shapes
    inputs_list = [example_inputs] if isinstance(example_inputs, ops.Tensor) else example_inputs
    if isinstance(example_inputs, abc.Mapping):
        func_args, func_kwargs = func.structured_input_signature
        if len(func_args) != 1:
            raise NotImplementedError('function with multiple dictionary inputs is not supported')
        input_dict, = func_args
        shape_feed_dict = {'{}:0'.format(spec.name): example_inputs[name].shape for name, spec in input_dict.items()}
    else:
        shape_feed_dict = {name: ts.shape for name, ts in zip(input_names, inputs_list)}
    graph_def = gdu.encode_inferred_shapes(graph_def, shape_feed_dict)

    # call main-graph grappler passes
    graph_def = _run_grappler_on_main_graph(graph_def, cfunc, subgraph_builder_function)

    # call graph_def_util/meta_graph_util passes
    graph_def = gdu.run_graph_def_pass_in_subgraphs(graph_def, gdu.convert_shape_to_constant)
    graph_def = mgu.run_grappler_on_subgraphs(graph_def)
    graph_def = gdu.run_compiler_on_subgraphs(graph_def)
    graph_def = gdu.restore_compiler_failures(graph_def, original_graph_def)
    graph_def = gdu.run_graph_def_pass_in_subgraphs(graph_def, gdu.erase_large_constants)
    graph_def = gdu.set_execution_plan(graph_def)

    # wrap GraphDef as a WrappedFunction
    output_names = _get_output_names(func.outputs, func.structured_outputs)
    cfunc = wrap_function.function_from_graph_def(graph_def, input_names, output_names)

    # some hacks to ensure the new ConcreteFunction will have the same calling signature
    cfunc.graph.structured_input_signature = func.graph.structured_input_signature
    try:
        cfunc._set_function_spec(func._function_spec)
    except AttributeError:
        unroll_args = isinstance(example_inputs, list)
    else:
        unroll_args = False

    # TODO: remove this hack once https://github.com/tensorflow/tensorflow/blob/v2.3.1/tensorflow/python/eager/wrap_function.py#L377 is fixed
    try:
        if cfunc._arg_keywords != func._arg_keywords:
            cfunc._arg_keywords = func._arg_keywords
    except AttributeError:
        pass

    # wrap ConcreteFunction as a keras model
    new_func = def_function.function(cfunc)
    struct_out = func.structured_outputs
    output_type = type(struct_out) if isinstance(struct_out, abc.Mapping) else None
    model = AwsNeuronModel(new_func, unroll_args, output_type)

    # hack to propagate metadata for saving
    if hasattr(model, '_set_save_spec'):
        set_save_spec = model._set_save_spec
    elif hasattr(model, '_set_input_attrs'):
        set_save_spec = model._set_input_attrs
    else:
        set_save_spec = None
        logging.warning('Not setting inputs for the traced model; it may not be savable before running inference')
    if set_save_spec is not None:
        set_save_spec(example_inputs)
    return model


class AwsNeuronModel(Model):

    def __init__(self, aws_neuron_function, unroll_args, output_type):
        super().__init__(trainable=False, autocast=False)
        self.aws_neuron_function = aws_neuron_function
        self._aws_neuron_unroll_args = unroll_args
        self._aws_neuron_output_type = output_type

    def call(self, inputs):
        if self._aws_neuron_unroll_args and not isinstance(inputs, ops.Tensor):
            output = self.aws_neuron_function(*inputs)
        else:
            output = self.aws_neuron_function(inputs)
        if self._aws_neuron_output_type is not None:
            output = self._aws_neuron_output_type(**output)
        return output


def _run_grappler_on_main_graph(graph_def, cfunc, subgraph_builder_function):
    # produce GraphDef by running grappler passes
    meta_graph_def = meta_graph_pb2.MetaGraphDef(graph_def=graph_def)
    sig_def = mgu.build_signature_def(cfunc.inputs, cfunc.outputs)
    meta_graph_def.signature_def['serving_default'].CopyFrom(sig_def)
    opt_config = config_pb2.ConfigProto()
    rewriter_config = opt_config.graph_options.rewrite_options
    rewriter_config.meta_optimizer_iterations = 1
    rewriter_config.min_graph_nodes = -1
    graph_passes = [
        'debug_stripper',
        'pruning',
        'dependency',
        'aws_neuron_static_shape_inference',
    ]
    rewriter_config.optimizers.extend(graph_passes)

    # configure operator fusion
    fuser_config = rewriter_config.custom_optimizers.add()
    fuser_config.name = 'aws_neuron_fuse_supported_operators'
    fuser_param_map = fuser_config.parameter_map
    fuser_param_map['supported_op_types'].list.s.extend(item.encode() for item in list_operators())
    if subgraph_builder_function is None:
        fuser_param_map['fuse_foldable_nodes'].b = True
        fuser_param_map['prune_small_subgraphs_ratio'].f = 0.9
        try:
            import hlo2neuron
        except ImportError:
            no_fuse_ops = []
        else:
            no_fuse_ops = _find_pad_ops_preceding_conv2d(cfunc.graph)
    else:
        force_fuse_ops = [node.name for node in graph_def.node if subgraph_builder_function(node)]
        fuser_param_map['force_fuse_ops'].list.s.extend(item.encode() for item in force_fuse_ops)
        no_fuse_ops = [node.name for node in graph_def.node]
    if no_fuse_ops:
        fuser_param_map['no_fuse_ops'].list.s.extend(item.encode() for item in no_fuse_ops)

    # call all grappler passes
    with utils.change_grappler_logging_level_according_to_cc_flags():
        graph_def = tf_optimizer.OptimizeGraph(opt_config, meta_graph_def)
    return graph_def


def _find_pad_ops_preceding_conv2d(graph):
    # exclude Pad that precedes Conv2D for HLO frontend
    no_fuse_ops = []
    supported_op_types = list_operators()
    for op in graph.get_operations():
        if op.type == 'Pad' and op.inputs[0].op.type not in supported_op_types:
            consumers = op.outputs[0].consumers()
            if len(consumers) == 1 and consumers[0].type == 'Conv2D':
                no_fuse_ops.append(op.name)
                no_fuse_ops.append(op.inputs[1].op.name)
    return no_fuse_ops


def _get_output_names(tensors, structured_signature):
    if isinstance(structured_signature, dict):
        # return a map from output argument name to symbolic tensor name
        # in order to let the WrappedFunction's return dictionary have the correct keys
        tensor_specs = nest.flatten(structured_signature, expand_composites=True)
        tensor_spec_name_map = {spec.name: name for name, spec in structured_signature.items()}
        tensor_spec_names = [tensor_spec_name_map[spec.name] for spec in tensor_specs]
        return {name: ts.name for ts, name in zip(tensors, tensor_spec_names)}
    elif len(tensors) == 1 and isinstance(structured_signature, ops.Tensor):
        tensor, = tensors
        return tensor.name
    else:
        return [ts.name for ts in tensors]
