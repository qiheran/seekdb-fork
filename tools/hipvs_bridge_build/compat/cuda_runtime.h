#pragma once
// hipVS ships the cuVS C API unchanged; its public headers still include
// <cuda_runtime.h> and name CUDA runtime types. hipVS solves this with its own
// cuvs/cuda_runtime.h shim, which is not part of the installed prefix, so the
// bridge repeats the two aliases it needs.
#include <hip/hip_runtime.h>

#ifndef cudaDataType_t
#define cudaDataType_t hipDataType
#endif
#ifndef cudaStream_t
#define cudaStream_t hipStream_t
#endif
