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
import sys
import os
import json
import subprocess
import shlex
import tempfile
from distutils import spawn


def list_operators():
    neuron_cc = _find_neuron_cc()
    if neuron_cc is None:
        return set()
    command = [neuron_cc, 'list-operators', '--framework', 'TENSORFLOW']
    try:
        list_operators_output = subprocess.check_output(command)
    except subprocess.CalledProcessError:
        logging.warning('neuron-cc is not behaving correctly. Please check neuron-cc '
                        'installation, or reinstall by "pip install --force neuron-cc".')
        return set()
    op_whitelist = {op_type.strip() for op_type in list_operators_output.decode()[:-1].split('\n')}
    op_whitelist.discard('Placeholder')
    op_whitelist.discard('IdentityN')
    return op_whitelist


def compile_savetemps(graph_def, io_config, workdir=None, compiler_args=None):
    """Returns raw neff bytes (empty bytes if neuron-cc crashed)
    """
    neuron_cc = _find_neuron_cc()
    if neuron_cc is None:
        return b''
    neuron_cc_input_name = 'graph_def.pb'
    neuron_executable_name = 'graph_def.neff'
    if workdir is None:
        workdir_obj = tempfile.TemporaryDirectory()
        workdir = workdir_obj.name
    else:
        workdir = os.path.realpath(workdir)
        os.makedirs(workdir, exist_ok=True)
    input_path = os.path.join(workdir, neuron_cc_input_name)
    output_path = os.path.join(workdir, neuron_executable_name)
    with open(input_path, 'wb') as f:
        f.write(graph_def.SerializeToString())
    command = [neuron_cc, 'compile', input_path, '--framework', 'TENSORFLOW',
               '--pipeline', 'compile', 'SaveTemps', '--output', output_path]
    command.extend(['--io-config', json.dumps(io_config)])
    if compiler_args is not None:
        if isinstance(compiler_args, bytes):
            compiler_args = compiler_args.decode()
        if isinstance(compiler_args, str):
            compiler_args = shlex.split(compiler_args)
        command.extend(compiler_args)
    proc = subprocess.run(command, cwd=workdir)
    if proc.returncode != 0:
        return b''
    with open(output_path, 'rb') as f:
        executable = f.read()
    return executable


def _find_neuron_cc():
    path = '{}:{}'.format(os.path.dirname(sys.executable), os.environ.get('PATH', ''))
    return spawn.find_executable('neuron-cc', path)
