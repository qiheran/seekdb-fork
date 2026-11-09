/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "lib/vector/hipvs_build_api.h"
#if defined(__linux__)
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace oceanbase {
namespace common {
namespace obvsag {

// Optional AMD hipVS snapshot builder. The default vsag path must keep working
// on hosts without ROCm, so the bridge is loaded lazily with dlopen and only
// when an index explicitly requests lib=hipvs.
class HipvsBackend {
public:
  using BuildHnswFn = SeekdbHipvsBuildHnswV1;

  HipvsBackend() { load(); }
  HipvsBackend(const HipvsBackend &) = delete;
  HipvsBackend &operator=(const HipvsBackend &) = delete;

  BuildHnswFn build_hnsw() const { return build_hnsw_; }
  const char *error() const { return error_; }

private:
  void set_error(const char *message)
  {
    std::snprintf(error_, sizeof(error_), "%s", message == nullptr ? "unknown error" : message);
  }

  void load_from(const char *path)
  {
#if defined(__linux__)
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
      const char *detail = dlerror();
      std::snprintf(error_, sizeof(error_), "dlopen(%s): %s", path,
                    detail == nullptr ? "unknown error" : detail);
      return;
    }
    dlerror();
    void *symbol = dlsym(library, "seekdb_hipvs_build_hnsw_v1");
    const char *detail = dlerror();
    if (symbol == nullptr || detail != nullptr) {
      std::snprintf(error_, sizeof(error_), "dlsym(%s, seekdb_hipvs_build_hnsw_v1): %s", path,
                    detail == nullptr ? "null symbol" : detail);
      return;
    }
    build_hnsw_ = reinterpret_cast<BuildHnswFn>(symbol);
    // Keep the ROCm runtime loaded for the process lifetime: unloading a
    // library with live HIP worker threads or TLS is not safe.
#else
    (void)path;
    set_error("AMD hipVS construction requires Linux and ROCm");
#endif
  }

  void load()
  {
#if defined(__linux__)
    const char *configured = std::getenv("SEEKDB_HIPVS_BUILD_LIBRARY");
    if (configured != nullptr && configured[0] != '\0') {
      if (configured[0] != '/') {
        set_error("SEEKDB_HIPVS_BUILD_LIBRARY must be an absolute path");
      } else {
        load_from(configured);
      }
      return;
    }
    char executable[4096] = {};
    const ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length <= 0 || length >= static_cast<ssize_t>(sizeof(executable) - 1)) {
      set_error("cannot resolve /proc/self/exe; set SEEKDB_HIPVS_BUILD_LIBRARY");
      return;
    }
    char *slash = std::strrchr(executable, '/');
    if (slash == nullptr) {
      set_error("cannot locate the seekdb executable directory");
      return;
    }
    *slash = '\0';
    // Build-tree layout (bridge next to seekdb) and packaged layout.
    const char *suffixes[] = {
        "/libseekdb_hipvs_build.so",
        "/../lib/seekdb/libseekdb_hipvs_build.so",
    };
    for (const char *suffix : suffixes) {
      char path[8192] = {};
      std::snprintf(path, sizeof(path), "%s%s", executable, suffix);
      if (access(path, F_OK) == 0) {
        load_from(path);
        return;
      }
    }
    set_error("libseekdb_hipvs_build.so not found next to seekdb or in ../lib/seekdb; "
              "build with -DSEEKDB_WITH_HIPVS=ON on a ROCm/hipVS host or set "
              "SEEKDB_HIPVS_BUILD_LIBRARY");
#else
    set_error("AMD hipVS construction requires Linux and ROCm");
#endif
  }

  BuildHnswFn build_hnsw_ = nullptr;
  char error_[4096] = {};
};

} // namespace obvsag
} // namespace common
} // namespace oceanbase
