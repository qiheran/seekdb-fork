/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
// Optional AMD hipVS construction backend. Loaded with dlopen by seekdb only
// when an index declares lib=hipvs, so CPU-only builds never touch ROCm.
//
// The GPU builds the CAGRA base graph; the CPU completes the upper HNSW
// hierarchy and exports hnswlib's native stream. seekdb validates that stream
// and remaps its source-row labels to seekdb VIDs before importing it.
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/hnsw.h>
#include <hip/hip_runtime.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <string>

#include "lib/vector/hipvs_build_api.h"

namespace {

// The hipVS/RAFT allocator is process-global, so construction and destruction
// must not interleave between two seekdb sessions.
std::mutex g_build_mutex;
constexpr size_t kWorkerStackSize = 32ULL * 1024 * 1024;

void check_cuvs(cuvsError_t status, const char *operation)
{
  if (status != CUVS_SUCCESS) {
    const char *detail = cuvsGetLastErrorText();
    throw std::runtime_error(std::string(operation) + ": " +
                             (detail == nullptr ? "unknown hipVS error" : detail));
  }
}

void check_hip(hipError_t status, const char *operation)
{
  if (status != hipSuccess) {
    throw std::runtime_error(std::string(operation) + ": " + hipGetErrorString(status));
  }
}

int32_t parse_threads()
{
  const char *configured = std::getenv("SEEKDB_HIPVS_BUILD_THREADS");
  if (configured == nullptr || configured[0] == '\0') {
    return 8;
  }
  char *end = nullptr;
  const long parsed = std::strtol(configured, &end, 10);
  if (end == configured || *end != '\0' || parsed < 1 || parsed > 256) {
    return 8;
  }
  return static_cast<int32_t>(parsed);
}

struct Job {
  const float *vectors = nullptr;
  int64_t rows = 0;
  int32_t dim = 0;
  int32_t m = 0;
  int32_t ef_construction = 0;
  int32_t threads = 0;
  const char *output_path = nullptr;
  char *error = nullptr;
  size_t error_capacity = 0;
  int status = 1;
};

void run_build(Job &job)
{
  // Keeping the critical section around construction and destruction prevents
  // another build from observing a half-torn-down hipVS runtime.
  std::lock_guard<std::mutex> lock(g_build_mutex);
  cuvsResources_t resources = 0;
  cuvsCagraIndexParams_t cagra_params = nullptr;
  cuvsCagraIndex_t cagra_index = nullptr;
  cuvsHnswIndexParams_t hnsw_params = nullptr;
  cuvsHnswIndex_t hnsw_index = nullptr;
  bool have_resources = false;
  try {
    int device_count = 0;
    check_hip(hipGetDeviceCount(&device_count), "hipGetDeviceCount");
    if (device_count < 1) {
      // An explicit lib=hipvs request must fail rather than silently retrain
      // the graph on the CPU.
      throw std::runtime_error("no AMD GPU available; CPU fallback is disabled");
    }
    check_hip(hipSetDevice(0), "hipSetDevice");
    hipDeviceProp_t properties{};
    check_hip(hipGetDeviceProperties(&properties, 0), "hipGetDeviceProperties");
    if (std::strncmp(properties.gcnArchName, "gfx", 3) != 0) {
      throw std::runtime_error("the selected device is not an AMD GPU");
    }

    check_cuvs(cuvsResourcesCreate(&resources), "cuvsResourcesCreate");
    have_resources = true;
    check_cuvs(cuvsCagraIndexParamsCreate(&cagra_params), "cuvsCagraIndexParamsCreate");
    check_cuvs(cuvsCagraIndexCreate(&cagra_index), "cuvsCagraIndexCreate");
    cagra_params->metric = L2Expanded;
    cagra_params->graph_degree = 2 * job.m;
    cagra_params->intermediate_graph_degree =
        static_cast<int64_t>(std::min<int64_t>(job.rows - 1, 4LL * job.m));
    cagra_params->build_algo = NN_DESCENT;
    cagra_params->nn_descent_niter = 20;
    cagra_params->compression = nullptr;

    int64_t shape[] = {job.rows, job.dim};
    DLManagedTensor dataset{};
    dataset.dl_tensor.data = const_cast<float *>(job.vectors);
    dataset.dl_tensor.device = {kDLCPU, 0};
    dataset.dl_tensor.ndim = 2;
    dataset.dl_tensor.dtype = {kDLFloat, 32, 1};
    dataset.dl_tensor.shape = shape;
    dataset.dl_tensor.strides = nullptr;
    dataset.dl_tensor.byte_offset = 0;
    dataset.manager_ctx = nullptr;
    dataset.deleter = nullptr;

    check_cuvs(cuvsCagraBuild(resources, cagra_params, &dataset, cagra_index), "cuvsCagraBuild");
    check_cuvs(cuvsStreamSync(resources), "cuvsStreamSync(CAGRA)");

    check_cuvs(cuvsHnswIndexParamsCreate(&hnsw_params), "cuvsHnswIndexParamsCreate");
    check_cuvs(cuvsHnswIndexCreate(&hnsw_index), "cuvsHnswIndexCreate");
    // GPU supplies the base layer; the CPU only adds the upper hierarchy.
    hnsw_params->hierarchy = CPU;
    hnsw_params->ef_construction = job.ef_construction;
    hnsw_params->num_threads = job.threads;
    check_cuvs(cuvsHnswFromCagraWithDataset(resources, hnsw_params, cagra_index, hnsw_index,
                                            &dataset),
               "cuvsHnswFromCagraWithDataset");
    check_cuvs(cuvsStreamSync(resources), "cuvsStreamSync(HNSW)");
    check_cuvs(cuvsHnswSerialize(resources, job.output_path, hnsw_index), "cuvsHnswSerialize");
    check_cuvs(cuvsStreamSync(resources), "cuvsStreamSync(serialize)");

    // Virtualized GPUs report an empty name; the architecture still identifies
    // the device, and an empty quoted field would break log correlation.
    const char *device_name = properties.name[0] != '\0' ? properties.name
                                                         : properties.gcnArchName;
    std::fprintf(stderr,
                 "[seekdb-hipvs] backend=hipvs vendor=AMD device=\"%s\" arch=%s rows=%lld "
                 "dim=%d m=%d ef_construction=%d gpu_compute=true cpu_fallback=false\n",
                 device_name, properties.gcnArchName, static_cast<long long>(job.rows),
                 job.dim, job.m, job.ef_construction);
    job.status = 0;
  } catch (const std::exception &error) {
    std::snprintf(job.error, job.error_capacity, "%s", error.what());
  } catch (...) {
    std::snprintf(job.error, job.error_capacity, "unhandled hipVS build exception");
  }
  if (hnsw_index != nullptr) {
    cuvsHnswIndexDestroy(hnsw_index);
  }
  if (hnsw_params != nullptr) {
    cuvsHnswIndexParamsDestroy(hnsw_params);
  }
  if (cagra_index != nullptr) {
    cuvsCagraIndexDestroy(cagra_index);
  }
  if (cagra_params != nullptr) {
    cuvsCagraIndexParamsDestroy(cagra_params);
  }
  if (have_resources) {
    cuvsResourcesDestroy(resources);
  }
}

void *build_worker(void *opaque)
{
  Job &job = *static_cast<Job *>(opaque);
  run_build(job);
  return nullptr;
}

} // namespace

extern "C" int seekdb_hipvs_build_hnsw_v1(const float *vectors, int64_t rows, int32_t dim,
                                          int32_t m, int32_t ef_construction, int32_t threads,
                                          const char *output_path, char *error,
                                          size_t error_capacity)
{
  if (error == nullptr || error_capacity == 0) {
    return 1;
  }
  error[0] = '\0';
  if (vectors == nullptr || output_path == nullptr || output_path[0] == '\0' ||
      rows <= 2LL * m || rows > INT32_MAX || dim <= 0 || m < 5 || m > 128 ||
      ef_construction <= m || threads < 0) {
    std::snprintf(error, error_capacity,
                  "hipVS requires finite FP32/L2 data with rows > 2*M, 5 <= M <= 128, "
                  "ef_construction > M, dim > 0 and threads >= 0");
    return 1;
  }
  if (threads == 0) {
    threads = parse_threads();
  }
  for (int64_t i = 0; i < rows * static_cast<int64_t>(dim); ++i) {
    if (!std::isfinite(vectors[i])) {
      std::snprintf(error, error_capacity, "non-finite vector component at offset %lld",
                    static_cast<long long>(i));
      return 1;
    }
  }

  Job job;
  job.vectors = vectors;
  job.rows = rows;
  job.dim = dim;
  job.m = m;
  job.ef_construction = ef_construction;
  job.threads = threads;
  job.output_path = output_path;
  job.error = error;
  job.error_capacity = error_capacity;

  // ROCm/hipVS initialization needs an OS thread stack that is larger than a
  // seekdb worker coroutine stack.
  pthread_attr_t attr;
  int rc = pthread_attr_init(&attr);
  if (rc == 0) {
    rc = pthread_attr_setstacksize(&attr, kWorkerStackSize);
    pthread_t worker;
    if (rc == 0) {
      rc = pthread_create(&worker, &attr, build_worker, &job);
    }
    pthread_attr_destroy(&attr);
    if (rc == 0) {
      rc = pthread_join(worker, nullptr);
    }
  }
  if (rc != 0) {
    std::snprintf(error, error_capacity, "cannot start hipVS worker thread: %s", std::strerror(rc));
    return 1;
  }
  return job.status;
}
