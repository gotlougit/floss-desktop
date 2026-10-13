#!/usr/bin/env python3
"""Test the actual patched uinput.rs with mocked ioctls/writes and real owned FDs.
No root, devices, dependencies or network needed. Pass a cached rustc and linker.
"""
import argparse
import pathlib
import re
import subprocess
import tempfile
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=pathlib.Path, required=True)
p.add_argument('--rustc', default='rustc')
p.add_argument('--linker', default='cc')
a = p.parse_args()
# Apply just the packaged uinput hunk to a temporary copy of the upstream file.
patch = (pathlib.Path(__file__).resolve().parents[2]/'patches/bluetooth-service-runtime.patch').read_text()
patch = patch[patch.index('diff --git a/system/gd/rust/linux/utils/src/uinput.rs'):]
with tempfile.TemporaryDirectory(prefix='floss-uinput-source-') as tmp:
    patched = pathlib.Path(tmp)/'uinput.rs'
    patched.write_text(a.source.read_text())
    subprocess.run(['patch', '--batch', '--fuzz=0', str(patched)], input=patch, text=True, check=True)
    source = patched.read_text()
source = source.replace('//!', '//').replace('use libc;', 'use crate::mock_libc as libc;').replace('use nix;', '')
source = re.sub(r'nix::request_code_none!\(UINPUT_IOCTL_BASE, (\d+)\)', r'((85u64) << 8 | \1)', source)
source = re.sub(r'nix::request_code_write!\(UINPUT_IOCTL_BASE, (\d+), (mem::size_of::<.*?>\(\))\)', r'((1u64 << 30) | ((\2 as u64) << 16) | ((85u64) << 8) | \1)', source)
with tempfile.TemporaryDirectory(prefix='floss-uinput-test-') as tmp:
    tmp = pathlib.Path(tmp)
    src = tmp/'test.rs'
    src.write_text('#![allow(dead_code, non_camel_case_types, unused_parens)]\n'+source+'\n'+pathlib.Path(__file__).with_name('mock.rs').read_text())
    subprocess.run([a.rustc, '--edition=2021', '--test', str(src), '-C', 'linker='+a.linker, '-o', str(tmp/'test')], check=True)
    subprocess.run([str(tmp/'test'), '--test-threads=1'], check=True)
