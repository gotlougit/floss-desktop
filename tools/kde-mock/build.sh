#!/usr/bin/env bash
# Compile the test double using an already installed compiler and Qt base output.
set -euo pipefail
if [[ $# != 3 ]]; then
  echo "usage: $0 /path/to/c++ /path/to/qtbase /path/to/output-binary" >&2
  exit 2
fi
compiler=$1
qtbase=$2
output=$3
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
"$compiler" -std=c++17 -fPIC "$source_dir/floss-mock.cpp" \
  -I"$qtbase/include" -I"$qtbase/include/QtCore" -I"$qtbase/include/QtDBus" \
  -L"$qtbase/lib" -Wl,-rpath,"$qtbase/lib" -lQt6Core -lQt6DBus -o "$output"
