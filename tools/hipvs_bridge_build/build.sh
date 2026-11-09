#!/usr/bin/env bash
# Build the optional AMD hipVS HNSW bridge. CPU-only builds never call this.
set -euo pipefail
bridge_dir=$(cd -- "$(dirname -- "$0")" && pwd)
hipvs_prefix=${HIPVS_PREFIX:-/opt/hipvs}
rocm_prefix=${ROCM_PATH:-/opt/rocm}
hipvs_libdir=${HIPVS_LIBDIR:-"${hipvs_prefix}/lib"}
if [[ ! -e "${hipvs_libdir}/libcuvs_c.so" && -e "${hipvs_prefix}/lib64/libcuvs_c.so" ]]; then
  hipvs_libdir="${hipvs_prefix}/lib64"
fi
extra_includes=()
if [[ -n "${HIPVS_DLPACK_INCLUDE:-}" ]]; then
  extra_includes+=(-I"${HIPVS_DLPACK_INCLUDE}")
elif [[ ! -e "${hipvs_prefix}/include/dlpack/dlpack.h" ]]; then
  printf 'error: dlpack headers not found; copy hipVS dlpack headers into %s/include or set HIPVS_DLPACK_INCLUDE\n' \
    "${hipvs_prefix}" >&2
  exit 1
fi
out_dir=${1:-"${bridge_dir}/build"}
mkdir -p -- "${out_dir}"
"${rocm_prefix}/bin/hipcc" -std=c++17 -O3 -fPIC -shared -pthread \
  -I"${hipvs_prefix}/include" -I"${bridge_dir}/compat" -I"${bridge_dir}/../../src/oblib" \
  "${extra_includes[@]}" \
  "${bridge_dir}/seekdb_hipvs_build.cpp" \
  -L"${hipvs_libdir}" -lcuvs_c \
  -Wl,-z,defs -Wl,-rpath,"${hipvs_libdir}" \
  -Wl,-rpath,"${rocm_prefix}/lib" \
  -o "${out_dir}/libseekdb_hipvs_build.so"
printf 'Built %s/libseekdb_hipvs_build.so\n' "${out_dir}"
