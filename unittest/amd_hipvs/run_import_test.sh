#!/usr/bin/env bash
# CPU-only test for the hipVS HNSW stream validator; no GPU or seekdb build needed.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "${here}/../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "${out}"' EXIT
cxx=${CXX:-g++}
"${cxx}" -std=c++17 -O2 -Wall -Wextra -Werror \
  -I"${root}/src/oblib" \
  "${here}/validate_stream_test.cpp" \
  -o "${out}/validate_stream_test"
"${out}/validate_stream_test"
