#!/usr/bin/env python3
"""Test production AAC feeder control flow using an existing C++ compiler.

Extracts the two feeder functions unchanged from the production translation
unit. The harness substitutes only socket reads, codec RPC, logging and buffer
allocation; the real MMC encoder is tested separately by smoke-aac-isolated.py.
No network, hardware, or system services are accessed.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, default=root / 'Bluetooth/system/stack/a2dp/a2dp_aac_encoder_linux.cc')
parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
parser.add_argument('--output', type=Path, help='Keep the binary for the isolated real-codec test')
args = parser.parse_args()
source = args.source.read_text()
functions = []
for signature in ('static void a2dp_aac_encode_frames(uint8_t nb_frame) {',
                  'static bool a2dp_aac_read_feeding(uint8_t* read_buffer, uint32_t* bytes_read) {'):
    start = source.index(signature)
    end = source.index('\n}\n', start) + 3
    functions.append(source[start:end])
with tempfile.TemporaryDirectory(prefix='floss-aac-feeder-') as temporary:
    directory = Path(temporary)
    (directory / 'aac-feeder-production.inc').write_text('\n'.join(functions))
    binary = args.output.resolve() if args.output else directory / 'feeder-test'
    subprocess.run([args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I', str(directory), str(root / 'tools/aac-feeder-test.cc'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
