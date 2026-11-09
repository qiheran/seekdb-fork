# AMD hipVS bridge (lib=hipvs)

Optional FP32/L2 HNSW construction for seekdb. AMD hipVS (the ROCm port of
cuVS) builds the CAGRA base graph on the GPU; the CPU completes the upper HNSW
hierarchy and exports hnswlib's native stream. seekdb validates that stream,
remaps its source-row labels to seekdb VIDs, and imports it into a normal VSAG
index, so queries, filters, incremental writes, and persistence stay on the
existing VSAG path.

Only `type=hnsw,distance=l2` without stored extra information is supported. A
nonempty batch must contain more than `2*M` rows; unsupported requests fail
explicitly. There is no CPU fallback for an explicit `lib=hipvs` request.

## Build

The normal CMake build detects the SDK and builds the bridge next to the seekdb
executable (`build_release/src/observer/libseekdb_hipvs_build.so`). Packaging
installs it under `usr/lib/seekdb`, where the runtime loader also searches.

```sh
bash build.sh release -DSEEKDB_WITH_HIPVS=ON --make -j8
```

`SEEKDB_WITH_HIPVS=AUTO` (default) leaves CPU-only hosts unchanged when the SDK
is absent. `ON` fails during configuration if a dependency is missing: `hipcc`,
the hipVS `cuvs` headers, and `libcuvs_c.so`, all found under `/opt/hipvs` and
`/opt/rocm` by default. Override with `-DSEEKDB_HIPVS_PREFIX=...` and
`-DSEEKDB_ROCM_PATH=...`.

The bridge can also be built manually for diagnostics. hipVS installs its cuVS
headers but not the `dlpack` headers its C API includes, so either copy the
hipVS build tree's dlpack include directory into `${HIPVS_PREFIX}/include` or
point `HIPVS_DLPACK_INCLUDE` at it:

```sh
bash tools/hipvs_bridge_build/build.sh /tmp/hipvs-bridge
export SEEKDB_HIPVS_BUILD_LIBRARY=/tmp/hipvs-bridge/libseekdb_hipvs_build.so
export LD_LIBRARY_PATH=/opt/hipvs/lib:/opt/rocm/lib:${LD_LIBRARY_PATH:-}
```

The override must be inherited by the seekdb process before its first hipVS
build. `SEEKDB_HIPVS_BUILD_THREADS` (default 8) sets the CPU thread count used
for the upper hierarchy.

## Usage

```sql
CREATE VECTOR INDEX idx_gpu ON vectors(embedding)
WITH (distance=l2,type=hnsw,lib=hipvs,m=16,ef_construction=200,ef_search=200);
```

The GPU job runs on a dedicated 32 MiB pthread stack because ROCm
initialization exceeds a seekdb coroutine stack. Builds are serialized because
the hipVS/RAFT allocator is process-global. The optional bridge stays loaded for
the process lifetime; unloading live ROCm TLS/runtime state is unsafe.
