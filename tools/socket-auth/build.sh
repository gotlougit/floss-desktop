#!/usr/bin/env bash
# Use only an already cached compiler and Qt base output.
set -euo pipefail
if [[ $# != 3 ]]; then
  echo "usage: $0 /path/to/c++ /path/to/qtbase /path/to/output-binary" >&2
  exit 2
fi
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
"$1" -std=c++17 -fPIC "$source_dir/client.cpp" \
  -I"$2/include" -I"$2/include/QtCore" -I"$2/include/QtDBus" \
  -L"$2/lib" -Wl,-rpath,"$2/lib" -lQt6Core -lQt6DBus -o "$3"
