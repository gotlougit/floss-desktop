#!/usr/bin/env python3
"""Compile upstream MicrophoneTest with mocked discovery and real Qt/PulseAudio.
No package manager or network access is used. Supply cached development paths.
"""
import argparse
from pathlib import Path
import subprocess
p = argparse.ArgumentParser(description=__doc__)
for name in ('source', 'qt-base', 'pulse-dev', 'pulse-lib', 'output'):
    p.add_argument('--'+name, type=Path, required=True)
p.add_argument('--cxx', default='c++')
a = p.parse_args()
root = Path(__file__).resolve().parent
a.output.mkdir(parents=True, exist_ok=True)
flags = ['-std=c++20','-fPIC','-g','-I'+str(root/'stubs'),'-I'+str(a.source/'src'),
    '-I'+str(a.qt_base/'include'),'-I'+str(a.qt_base/'include/QtCore'),'-I'+str(a.pulse_dev/'include')]
moc = a.output/'moc_microphonetest.cpp'
subprocess.run([str(a.qt_base/'libexec/moc'), *[f for f in flags if f.startswith('-I')],
    str(a.source/'src/microphonetest.h'), '-o', str(moc)],check=True)
subprocess.run([a.cxx,*flags,str(a.source/'src/microphonetest.cpp'),str(moc),str(root/'microphone-lifecycle.cpp'),
    '-L'+str(a.qt_base/'lib'), '-Wl,-rpath,'+str(a.qt_base/'lib'), '-lQt6Core',
    '-L'+str(a.pulse_lib/'lib'), '-Wl,-rpath,'+str(a.pulse_lib/'lib'), '-lpulse',
    '-o',str(a.output/'microphone-lifecycle')],check=True)
