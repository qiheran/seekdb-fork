/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace oceanbase {
namespace common {
namespace obvsag {

// hipVS exports the CPU-hierarchy HNSW in hnswlib's native 64-bit
// little-endian layout. Labels are source row numbers, not seekdb VIDs.
// Validate every byte before importing, then rewrite the labels in place.
inline bool validate_and_remap_hipvs_hnsw(std::vector<int8_t> &data,
                                         const float *vectors,
                                         const int64_t *ids,
                                         int64_t rows,
                                         int dim,
                                         int m,
                                         std::string &error)
{
  const auto fail = [&error](const char *message) {
    error = message;
    return false;
  };
  const uint16_t endian = 1;
  if (vectors == nullptr || ids == nullptr || rows <= 0 || rows > INT32_MAX ||
      dim <= 0 || m < 5 || m > 128 || sizeof(size_t) != 8 ||
      *reinterpret_cast<const uint8_t *>(&endian) != 1 || data.size() < 96) {
    return fail("invalid hipVS HNSW import arguments");
  }

  const uint64_t n = static_cast<uint64_t>(rows);
  const uint64_t dtype_size = 4;
  const uint64_t data_offset = 4 + 8 * static_cast<uint64_t>(m);
  const uint64_t label_offset = data_offset + dtype_size * static_cast<uint64_t>(dim);
  const uint64_t stride = label_offset + 8;
  const auto read_u64 = [&data](size_t pos) {
    uint64_t value = 0;
    std::memcpy(&value, data.data() + pos, sizeof(value));
    return value;
  };
  const auto read_u32 = [&data](size_t pos) {
    uint32_t value = 0;
    std::memcpy(&value, data.data() + pos, sizeof(value));
    return value;
  };
  const auto write_u64 = [&data](size_t pos, uint64_t value) {
    std::memcpy(data.data() + pos, &value, sizeof(value));
  };

  if (read_u64(0) != 0 || read_u64(8) < n || read_u64(16) != n ||
      read_u64(24) != stride || read_u64(32) != label_offset ||
      read_u64(40) != data_offset || read_u32(48) > 64 ||
      read_u32(52) >= n || read_u64(56) != static_cast<uint64_t>(m) ||
      read_u64(64) != static_cast<uint64_t>(2 * m) ||
      read_u64(72) != static_cast<uint64_t>(m)) {
    return fail("hipVS HNSW header does not match the requested FP32/L2 index");
  }
  double multiplier = 0;
  std::memcpy(&multiplier, data.data() + 80, sizeof(multiplier));
  if (!std::isfinite(multiplier) || multiplier <= 0 ||
      read_u64(88) < static_cast<uint64_t>(m)) {
    return fail("invalid hipVS HNSW hierarchy parameters");
  }
  if (n > (data.size() - 96) / stride || data.size() - 96 - n * stride < 4 * n) {
    return fail("truncated hipVS HNSW level-zero graph");
  }

  // Keep only the rows we actually imported; never inherit an oversized
  // pre-allocation from the builder.
  write_u64(8, n);

  std::vector<int64_t> sorted_ids(ids, ids + rows);
  std::sort(sorted_ids.begin(), sorted_ids.end());
  if (std::adjacent_find(sorted_ids.begin(), sorted_ids.end()) != sorted_ids.end()) {
    return fail("duplicate seekdb VIDs in hipVS snapshot");
  }

  std::vector<uint8_t> seen(n, 0);
  for (uint64_t row = 0; row < n; ++row) {
    const size_t pos = 96 + static_cast<size_t>(row) * stride;
    const uint32_t degree = read_u32(pos);
    const uint64_t source_row = read_u64(pos + label_offset);
    if (degree > static_cast<uint32_t>(2 * m) || source_row >= n || seen[source_row]++) {
      return fail("invalid degree, deleted node, or duplicate source row in hipVS HNSW");
    }
    for (uint32_t j = 0; j < degree; ++j) {
      if (read_u32(pos + 4 + 4 * static_cast<size_t>(j)) >= n) {
        return fail("hipVS HNSW level-zero neighbor outside the snapshot");
      }
    }
    if (std::memcmp(data.data() + pos + data_offset,
                    vectors + source_row * static_cast<uint64_t>(dim),
                    dtype_size * static_cast<uint64_t>(dim)) != 0) {
      return fail("hipVS HNSW label/vector correspondence changed");
    }
    write_u64(pos + label_offset, static_cast<uint64_t>(ids[source_row]));
  }

  const uint64_t upper_block = 4 + 4 * static_cast<uint64_t>(m);
  size_t pos = 96 + static_cast<size_t>(n) * stride;
  uint64_t entry_levels = 0;
  const uint32_t entry = read_u32(52);
  const uint32_t max_level = read_u32(48);
  for (uint64_t row = 0; row < n; ++row) {
    if (data.size() - pos < 4) {
      return fail("missing hipVS HNSW upper-layer size");
    }
    const uint32_t length = read_u32(pos);
    pos += 4;
    if (length % upper_block != 0 ||
        length / upper_block > static_cast<uint64_t>(max_level) ||
        length > data.size() - pos) {
      return fail("invalid hipVS HNSW upper-layer extent");
    }
    if (row == entry) {
      entry_levels = length / upper_block;
    }
    for (size_t level = 0; level < length; level += upper_block) {
      const uint32_t degree = read_u32(pos + level);
      if (degree > static_cast<uint32_t>(m)) {
        return fail("invalid hipVS HNSW upper-layer degree");
      }
      for (uint32_t j = 0; j < degree; ++j) {
        if (read_u32(pos + level + 4 + 4 * static_cast<size_t>(j)) >= n) {
          return fail("hipVS HNSW upper-layer neighbor outside the snapshot");
        }
      }
    }
    pos += length;
  }
  if (entry_levels != static_cast<uint64_t>(max_level) || pos != data.size()) {
    return fail("invalid hipVS HNSW entry point or trailing data");
  }
  return true;
}

} // namespace obvsag
} // namespace common
} // namespace oceanbase
