/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
// CPU-only checks for the hipVS HNSW stream validator. No GPU, ROCm, or seekdb
// build is required: the header under test is self-contained.
#include "lib/vector/hipvs_hnsw_import.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using oceanbase::common::obvsag::validate_and_remap_hipvs_hnsw;

namespace {

constexpr int kRows = 4;
constexpr int kDim = 4;
constexpr int kM = 5;
constexpr uint64_t kDataOffset = 4 + 8 * kM;
constexpr uint64_t kLabelOffset = kDataOffset + 4 * kDim;
constexpr uint64_t kStride = kLabelOffset + 8;
constexpr uint64_t kUpperBlock = 4 + 4 * kM;

struct Fixture {
  std::vector<int8_t> data;
  std::vector<float> vectors;
  std::vector<int64_t> ids;
};

void put_u32(std::vector<int8_t> &data, size_t pos, uint32_t value)
{
  std::memcpy(data.data() + pos, &value, sizeof(value));
}

void put_u64(std::vector<int8_t> &data, size_t pos, uint64_t value)
{
  std::memcpy(data.data() + pos, &value, sizeof(value));
}

void put_f32(std::vector<int8_t> &data, size_t pos, float value)
{
  std::memcpy(data.data() + pos, &value, sizeof(value));
}

Fixture make_fixture()
{
  Fixture f;
  f.vectors.resize(static_cast<size_t>(kRows) * kDim);
  f.ids = {900, 12, 77, 5};
  for (int row = 0; row < kRows; ++row) {
    for (int d = 0; d < kDim; ++d) {
      f.vectors[static_cast<size_t>(row) * kDim + d] =
          static_cast<float>(row * 10 + d) + 0.5f;
    }
  }
  const size_t size = 96 + static_cast<size_t>(kRows) * kStride + kRows * 4;
  f.data.assign(size, 0);

  put_u64(f.data, 0, 0);
  put_u64(f.data, 8, kRows);
  put_u64(f.data, 16, kRows);
  put_u64(f.data, 24, kStride);
  put_u64(f.data, 32, kLabelOffset);
  put_u64(f.data, 40, kDataOffset);
  put_u32(f.data, 48, 0);       // max_level
  put_u32(f.data, 52, 0);       // entry point
  put_u64(f.data, 56, kM);
  put_u64(f.data, 64, 2 * kM);
  put_u64(f.data, 72, kM);
  const double mult = 1.0 / std::log(static_cast<double>(kM));
  std::memcpy(f.data.data() + 80, &mult, sizeof(mult));
  put_u64(f.data, 88, 200);     // ef_construction

  for (int row = 0; row < kRows; ++row) {
    const size_t pos = 96 + static_cast<size_t>(row) * kStride;
    put_u32(f.data, pos, 1);
    put_u32(f.data, pos + 4, static_cast<uint32_t>((row + 1) % kRows));
    for (int d = 0; d < kDim; ++d) {
      put_f32(f.data, pos + kDataOffset + 4 * d,
              f.vectors[static_cast<size_t>(row) * kDim + d]);
    }
    put_u64(f.data, pos + kLabelOffset, static_cast<uint64_t>(row));
    put_u32(f.data, 96 + static_cast<size_t>(kRows) * kStride + 4 * row, 0);
  }
  return f;
}

uint64_t read_u64(const std::vector<int8_t> &data, size_t pos)
{
  uint64_t value = 0;
  std::memcpy(&value, data.data() + pos, sizeof(value));
  return value;
}

int failures = 0;

void expect(bool condition, const char *name)
{
  if (condition) {
    std::printf("ok   %s\n", name);
  } else {
    std::printf("FAIL %s\n", name);
    ++failures;
  }
}

void expect_reject(const Fixture &f, const char *name)
{
  std::vector<int8_t> copy = f.data;
  std::string error;
  const bool accepted = validate_and_remap_hipvs_hnsw(
      copy, f.vectors.data(), f.ids.data(), kRows, kDim, kM, error);
  expect(!accepted && !error.empty(), name);
}

} // namespace

int main()
{
  const Fixture base = make_fixture();

  {
    std::vector<int8_t> copy = base.data;
    std::string error;
    const bool accepted = validate_and_remap_hipvs_hnsw(
        copy, base.vectors.data(), base.ids.data(), kRows, kDim, kM, error);
    expect(accepted, "accepts a well-formed stream");
    bool labels_ok = true;
    for (int row = 0; row < kRows; ++row) {
      const size_t pos = 96 + static_cast<size_t>(row) * kStride;
      labels_ok = labels_ok && read_u64(copy, pos + kLabelOffset) ==
                                   static_cast<uint64_t>(base.ids[row]);
    }
    expect(labels_ok, "remaps source row labels to seekdb VIDs");
    expect(read_u64(copy, 8) == kRows, "shrinks max_elements to the imported count");
  }

  {
    Fixture f = base;
    put_u64(f.data, 24, kStride + 4);
    expect_reject(f, "rejects a stride mismatch");
  }
  {
    Fixture f = base;
    put_u64(f.data, 16, kRows - 1);
    expect_reject(f, "rejects a wrong element count");
  }
  {
    Fixture f = base;
    put_u32(f.data, 96 + kStride + kLabelOffset, 0);  // row 1 label = row 0
    expect_reject(f, "rejects duplicate source rows");
  }
  {
    Fixture f = base;
    put_u32(f.data, 96 + 4, kRows);  // neighbor out of range
    expect_reject(f, "rejects an out-of-range level-zero neighbor");
  }
  {
    Fixture f = base;
    put_f32(f.data, 96 + 2 * kStride + kDataOffset, -1.0f);
    expect_reject(f, "rejects a label/vector mismatch");
  }
  {
    Fixture f = base;
    put_u32(f.data, 52, kRows);  // entry point out of range
    expect_reject(f, "rejects an out-of-range entry point");
  }
  {
    Fixture f = base;
    f.data.pop_back();
    expect_reject(f, "rejects a truncated stream");
  }
  {
    Fixture f = base;
    f.data.push_back(0);
    expect_reject(f, "rejects trailing bytes");
  }
  {
    Fixture f = base;
    f.ids[1] = f.ids[0];
    expect_reject(f, "rejects duplicate seekdb VIDs");
  }
  {
    Fixture f = base;
    put_u64(f.data, 64, 2 * kM + 2);  // maxM0 mismatch
    expect_reject(f, "rejects header hierarchy parameters");
  }

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
