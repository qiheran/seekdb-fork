#!/usr/bin/env python3
"""Real-GPU check of the hipVS bridge export. This is not the SQL build benchmark.

The script loads libseekdb_hipvs_build.so, builds several HNSW indexes on the
AMD GPU, and validates every byte of the exported hnswlib-layout stream the same
way the seekdb import path does: header, level-zero degree/link bounds, exact
FP32 vector correspondence, unique source labels, upper layers and trailing data.
It does not measure SQL end-to-end construction and does not substitute for it.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import math
import pathlib
import struct
import sys
import time

import numpy as np

CASES = ((128, 8, 16, 200), (1000, 32, 16, 200), (5000, 64, 32, 400))


def validate_stream(data: bytes, vectors: np.ndarray, rows: int, dim: int, m: int) -> dict:
    if len(data) < 96:
        raise ValueError("exported stream is shorter than the HNSW header")
    offset0, capacity, count, stride, label_offset, data_offset = struct.unpack_from("<6Q", data, 0)
    max_level, entry = struct.unpack_from("<2I", data, 48)
    max_m, max_m0, m_field = struct.unpack_from("<3Q", data, 56)
    multiplier = struct.unpack_from("<d", data, 80)[0]
    upper_param = struct.unpack_from("<Q", data, 88)[0]
    if offset0 != 0 or capacity < rows or count != rows:
        raise ValueError("HNSW capacity/count does not match the input rows")
    if data_offset != 4 + 8 * m or label_offset != data_offset + 4 * dim or stride != label_offset + 8:
        raise ValueError("HNSW element layout does not match FP32 data and M")
    if max_level > 64 or entry >= rows:
        raise ValueError("invalid HNSW max level or entry point")
    if (max_m, max_m0, m_field) != (m, 2 * m, m):
        raise ValueError("HNSW M fields do not match the requested graph degree")
    if not math.isfinite(multiplier) or multiplier <= 0 or upper_param < m:
        raise ValueError("invalid HNSW hierarchy parameters")
    if len(data) < 96 + rows * stride:
        raise ValueError("truncated HNSW level-zero graph")

    seen = bytearray(rows)
    for row in range(rows):
        pos = 96 + row * stride
        degree = struct.unpack_from("<I", data, pos)[0]
        if degree > 2 * m:
            raise ValueError("level-zero degree exceeds 2*M")
        for j in range(degree):
            if struct.unpack_from("<I", data, pos + 4 + 4 * j)[0] >= rows:
                raise ValueError("level-zero neighbor outside the input")
        source = struct.unpack_from("<Q", data, pos + label_offset)[0]
        if source >= rows or seen[source]:
            raise ValueError("duplicate or out-of-range source label")
        seen[source] = 1
        if data[pos + data_offset:pos + label_offset] != vectors[source].tobytes():
            raise ValueError("exported vector does not match its source label")

    pos = 96 + rows * stride
    upper_block = 4 + 4 * m
    entry_levels = 0
    for row in range(rows):
        if len(data) - pos < 4:
            raise ValueError("missing upper-layer length")
        length = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        if length % upper_block or length // upper_block > max_level or length > len(data) - pos:
            raise ValueError("invalid upper-layer extent")
        if row == entry:
            entry_levels = length // upper_block
        for level in range(0, length, upper_block):
            degree = struct.unpack_from("<I", data, pos + level)[0]
            if degree > m:
                raise ValueError("upper-layer degree exceeds M")
            for j in range(degree):
                if struct.unpack_from("<I", data, pos + level + 4 + 4 * j)[0] >= rows:
                    raise ValueError("upper-layer neighbor outside the input")
        pos += length
    if entry_levels != max_level or pos != len(data):
        raise ValueError("entry point levels or trailing data do not match the header")
    return {"max_level": max_level, "entry_point_levels": entry_levels,
            "level_zero_degree_max": 2 * m, "node_bytes": stride}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[2]
                        / "tools/hipvs_bridge_build/build/libseekdb_hipvs_build.so")
    parser.add_argument("--output-dir", type=pathlib.Path, default=pathlib.Path("local-results"))
    parser.add_argument("--threads", type=int, default=8)
    args = parser.parse_args()

    root = args.output_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    library_path = args.library.resolve()
    library = ctypes.CDLL(str(library_path))
    build = library.seekdb_hipvs_build_hnsw_v1
    build.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.c_int64, ctypes.c_int32,
                      ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, ctypes.c_char_p,
                      ctypes.c_char_p, ctypes.c_size_t]
    build.restype = ctypes.c_int

    digest = hashlib.sha256(library_path.read_bytes()).hexdigest()
    reports = []
    for rows, dim, m, ef_construction in CASES:
        vectors = np.random.default_rng(20261109).normal(size=(rows, dim)).astype("<f4")
        output = root / f"probe_{rows}x{dim}.hnsw"
        error = ctypes.create_string_buffer(4096)
        started = time.perf_counter()
        status = build(vectors.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), rows, dim,
                       m, ef_construction, args.threads, str(output).encode(),
                       error, len(error))
        elapsed = time.perf_counter() - started
        if status != 0:
            raise RuntimeError(error.value.decode("utf-8", errors="replace"))
        data = output.read_bytes()
        layout = validate_stream(data, vectors, rows, dim, m)
        reports.append({
            "rows": rows, "dim": dim, "m": m, "ef_construction": ef_construction,
            "threads": args.threads, "bridge_seconds": elapsed, "stream_bytes": len(data),
            **layout, "all_vectors_preserved": True, "all_source_labels_unique": True,
            "scope": "GPU bridge export only; SQL end-to-end construction is benchmark.py",
        })
        print(f"{rows}x{dim} m={m}: {elapsed:.6f}s, {len(data)} bytes, stream validated",
              flush=True)

    result = {"library": str(library_path), "library_sha256": digest,
              "bridge_api": "seekdb_hipvs_build_hnsw_v1", "cases": reports}
    (root / "gpu-probe.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
