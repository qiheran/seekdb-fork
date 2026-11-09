# Copyright (c) 2025 OceanBase.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Optional AMD hipVS bridge (lib=hipvs). The bridge is a separate shared object
# loaded with dlopen, so the default CPU build neither links ROCm nor needs the
# SDK. SEEKDB_WITH_HIPVS=AUTO skips it when the SDK is absent; ON fails the
# configure step when it is missing; OFF never builds it.
function(seekdb_configure_hipvs target)
  set(SEEKDB_WITH_HIPVS "AUTO" CACHE STRING "Build the AMD hipVS bridge: AUTO, ON, or OFF")
  set_property(CACHE SEEKDB_WITH_HIPVS PROPERTY STRINGS AUTO ON OFF)
  if(NOT SEEKDB_WITH_HIPVS MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "SEEKDB_WITH_HIPVS must be AUTO, ON, or OFF")
  endif()
  if(SEEKDB_WITH_HIPVS STREQUAL "OFF")
    return()
  endif()
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
    if(SEEKDB_WITH_HIPVS STREQUAL "ON")
      message(FATAL_ERROR "HIPVS requires a native Linux build and AMD ROCm")
    endif()
    return()
  endif()

  set(hipvs_prefix_default "/opt/hipvs")
  set(rocm_path_default "/opt/rocm")
  if(DEFINED ENV{HIPVS_PREFIX} AND NOT "$ENV{HIPVS_PREFIX}" STREQUAL "")
    set(hipvs_prefix_default "$ENV{HIPVS_PREFIX}")
  endif()
  if(DEFINED ENV{ROCM_PATH} AND NOT "$ENV{ROCM_PATH}" STREQUAL "")
    set(rocm_path_default "$ENV{ROCM_PATH}")
  endif()
  set(SEEKDB_HIPVS_PREFIX "${hipvs_prefix_default}" CACHE PATH "hipVS C SDK root")
  set(SEEKDB_ROCM_PATH "${rocm_path_default}" CACHE PATH "AMD ROCm root")

  set(hipvs_libdir "${SEEKDB_HIPVS_PREFIX}/lib")
  if(NOT EXISTS "${hipvs_libdir}/libcuvs_c.so" AND
     EXISTS "${SEEKDB_HIPVS_PREFIX}/lib64/libcuvs_c.so")
    set(hipvs_libdir "${SEEKDB_HIPVS_PREFIX}/lib64")
  endif()

  set(missing "")
  foreach(required IN ITEMS
      "${SEEKDB_ROCM_PATH}/bin/hipcc"
      "${SEEKDB_ROCM_PATH}/include/hip/hip_runtime.h"
      "${SEEKDB_HIPVS_PREFIX}/include/cuvs/core/c_api.h"
      "${SEEKDB_HIPVS_PREFIX}/include/cuvs/neighbors/cagra.h"
      "${SEEKDB_HIPVS_PREFIX}/include/cuvs/neighbors/hnsw.h"
      "${hipvs_libdir}/libcuvs_c.so")
    if(NOT EXISTS "${required}")
      list(APPEND missing "${required}")
    endif()
  endforeach()
  if(missing)
    if(SEEKDB_WITH_HIPVS STREQUAL "ON")
      message(FATAL_ERROR "HIPVS requested but dependencies are missing: ${missing}")
    endif()
    message(STATUS "HIPVS bridge skipped (SDK not installed): ${missing}")
    message(STATUS "CPU build unchanged; pass -DSEEKDB_WITH_HIPVS=ON on an AMD host to require the bridge.")
    return()
  endif()

  set(bridge_dir "${CMAKE_SOURCE_DIR}/tools/hipvs_bridge_build")
  # Keep the library next to the seekdb executable so the runtime loader finds
  # it without an environment override.
  set(bridge "${CMAKE_CURRENT_BINARY_DIR}/libseekdb_hipvs_build.so")
  add_custom_command(OUTPUT "${bridge}"
    COMMAND "${CMAKE_COMMAND}" -E env
      "HIPVS_PREFIX=${SEEKDB_HIPVS_PREFIX}"
      "ROCM_PATH=${SEEKDB_ROCM_PATH}"
      "HIPVS_LIBDIR=${hipvs_libdir}"
      bash "${bridge_dir}/build.sh" "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS "${bridge_dir}/build.sh" "${bridge_dir}/seekdb_hipvs_build.cpp"
            "${CMAKE_SOURCE_DIR}/src/oblib/lib/vector/hipvs_build_api.h"
    COMMENT "Building AMD hipVS HNSW bridge"
    VERBATIM)
  add_custom_target(seekdb_hipvs_bridge DEPENDS "${bridge}")
  add_dependencies(${target} seekdb_hipvs_bridge)
  install(FILES "${bridge}" DESTINATION usr/lib/seekdb COMPONENT server)
  message(STATUS "HIPVS bridge enabled (hipVS=${SEEKDB_HIPVS_PREFIX}, ROCm=${SEEKDB_ROCM_PATH})")
endfunction()
