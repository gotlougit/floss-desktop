#!/usr/bin/env bash
# Exercise the production property store before main(), then concurrently.
# Requires a locally installed C++ compiler; never downloads a toolchain.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
compiler=${CXX:-c++}
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
"$compiler" -std=c++17 -pthread -I "$root/Bluetooth/system/gd" \
  "$root/tools/floss-properties-smoke.cc" \
  "$root/Bluetooth/system/gd/os/linux/system_properties.cc" -o "$work/properties-smoke"
"$work/properties-smoke"
printf 'Static initialization and concurrent property access: PASS\n'
