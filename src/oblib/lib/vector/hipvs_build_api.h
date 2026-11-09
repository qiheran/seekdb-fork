/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <stddef.h>
#include <stdint.h>

// Stable C ABI between seekdb and the optional ROCm/hipVS bridge. Keeping the
// C++17/ROCm toolchain in a separate shared object means a normal CPU build
// never needs a HIP compiler or SDK.
//
// The bridge must synchronize the GPU, complete the CPU upper hierarchy, export
// a validated native HNSW stream to output_path, and return 0. Any non-zero
// return is a hard failure: no silent CPU fallback is allowed for a request
// that explicitly selected lib=hipvs.
typedef int (*SeekdbHipvsBuildHnswV1)(const float *vectors,
                                      int64_t rows,
                                      int32_t dim,
                                      int32_t m,
                                      int32_t ef_construction,
                                      int32_t threads,
                                      const char *output_path,
                                      char *error,
                                      size_t error_capacity);

#ifdef __cplusplus
extern "C" {
#endif

int seekdb_hipvs_build_hnsw_v1(const float *vectors,
                               int64_t rows,
                               int32_t dim,
                               int32_t m,
                               int32_t ef_construction,
                               int32_t threads,
                               const char *output_path,
                               char *error,
                               size_t error_capacity);

#ifdef __cplusplus
}
#endif
