/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_META_H_
#define OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_META_H_

#include "lib/string/ob_string.h"
#include "lib/container/ob_array.h"
#include "lib/allocator/ob_allocator.h"
#include "share/ob_define.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

// Physical hidden column name prefixes for AI columns.
static const char *OB_AI_STATE_COL_PREFIX = "__obai_state_";
static const char *OB_AI_REQ_COL_PREFIX = "__obai_req_";
static const char *OB_AI_SPEC_COL_PREFIX = "__obai_spec_";

// AI column hidden state values (stored in the state hidden column).
static const char *OB_AI_STATE_PENDING = "PENDING";
static const char *OB_AI_STATE_READY = "READY";
static const char *OB_AI_STATE_FAILED = "FAILED";

// __all_ai_column_task.state values
static const int64_t OB_AI_TASK_STATE_PENDING = 0;
static const int64_t OB_AI_TASK_STATE_RETRYING = 3;

enum ObAiFuncKind : int32_t
{
  AI_FUNC_COMPLETE = 0,
  AI_FUNC_EMBED = 1,
};

enum ObAiResultKind : int32_t
{
  AI_RESULT_STRING = 0,
  AI_RESULT_JSON = 1,
  AI_RESULT_INT = 2,
  AI_RESULT_VECTOR = 3,
};

// In-memory representation of the per-AI-column metadata. The schema column
// keeps the serialized form in ObColumnSchemaV2::ai_meta_; this struct is the
// parsed view used by resolvers / worker / strict read checks.
struct ObAiColumnMeta
{
public:
  ObAiColumnMeta() { reset(); }
  ~ObAiColumnMeta() {}
  void reset()
  {
    func_kind_ = AI_FUNC_COMPLETE;
    result_kind_ = AI_RESULT_STRING;
    max_retries_ = 0;
    vector_dim_ = -1;
    sync_mode_.reset();
    model_name_.reset();
    system_prompt_.reset();
    prompt_expr_.reset();
    normalized_expr_.reset();
    spec_json_.reset();
    result_col_name_.reset();
    state_col_name_.reset();
    req_col_name_.reset();
    spec_col_name_.reset();
    dep_column_ids_.reset();
    state_col_id_ = 0;
    req_col_id_ = 0;
    spec_col_id_ = 0;
    result_col_id_ = 0;
  }
  bool is_valid() const { return model_name_.length() > 0 && state_col_id_ > 0 && req_col_id_ > 0 && spec_col_id_ > 0; }

  int32_t func_kind_;           // ObAiFuncKind
  int32_t result_kind_;         // ObAiResultKind
  int64_t max_retries_;
  int64_t vector_dim_;          // -1 when not vector / not specified
  common::ObString sync_mode_;
  common::ObString model_name_;
  common::ObString system_prompt_;
  common::ObString prompt_expr_;
  common::ObString normalized_expr_;
  common::ObString spec_json_;
  common::ObString result_col_name_;
  common::ObString state_col_name_;
  common::ObString req_col_name_;
  common::ObString spec_col_name_;
  common::ObArray<uint64_t> dep_column_ids_;
  uint64_t state_col_id_;
  uint64_t req_col_id_;
  uint64_t spec_col_id_;
  uint64_t result_col_id_;
};

// ------------------------------------------------------------------
// Binary (length-prefixed) serialization of ObAiColumnMeta. All strings are
// deep copied into `alloc` when parsing.
// ------------------------------------------------------------------
inline int ob_ai_write_str(char *&p, const common::ObString &s)
{
  int ret = common::OB_SUCCESS;
  const int32_t len = static_cast<int32_t>(s.length());
  MEMCPY(p, &len, sizeof(len));
  p += sizeof(len);
  if (len > 0) {
    MEMCPY(p, s.ptr(), len);
    p += len;
  }
  return ret;
}

inline int ob_ai_read_str(const char *&p, const char *end, common::ObString &s)
{
  int ret = common::OB_SUCCESS;
  int32_t len = 0;
  if (p + sizeof(len) > end) {
    ret = common::OB_ERR_UNEXPECTED;
  } else {
    MEMCPY(&len, p, sizeof(len));
    p += sizeof(len);
    if (len < 0 || p + len > end) {
      ret = common::OB_ERR_UNEXPECTED;
    } else if (len > 0) {
      s.assign_ptr(p, len);
      p += len;
    } else {
      s.assign_ptr("", 0);
    }
  }
  return ret;
}

inline int64_t ob_ai_encoded_size(const ObAiColumnMeta &meta)
{
  int64_t len = 0;
  len += sizeof(int32_t) * 2 + sizeof(int64_t) * 3; // kinds, retries, dim
  const common::ObString *strs[] = {
      &meta.sync_mode_, &meta.model_name_, &meta.system_prompt_, &meta.prompt_expr_,
      &meta.normalized_expr_, &meta.spec_json_, &meta.result_col_name_,
      &meta.state_col_name_, &meta.req_col_name_, &meta.spec_col_name_ };
  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(strs)/sizeof(strs[0])); ++i) {
    len += sizeof(int32_t) + strs[i]->length();
  }
  len += sizeof(int32_t) + meta.dep_column_ids_.count() * sizeof(uint64_t);
  len += sizeof(uint64_t) * 4;
  return len;
}

inline int build_ai_meta(common::ObIAllocator &alloc, const ObAiColumnMeta &meta,
                         common::ObString &out)
{
  int ret = common::OB_SUCCESS;
  const int64_t size = ob_ai_encoded_size(meta);
  char *buf = nullptr;
  if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(size)))) {
    ret = common::OB_ALLOCATE_MEMORY_FAILED;
  } else {
    char *p = buf;
    const int32_t func_kind = meta.func_kind_;
    const int32_t result_kind = meta.result_kind_;
    MEMCPY(p, &func_kind, sizeof(func_kind)); p += sizeof(func_kind);
    MEMCPY(p, &result_kind, sizeof(result_kind)); p += sizeof(result_kind);
    MEMCPY(p, &meta.max_retries_, sizeof(int64_t)); p += sizeof(int64_t);
    MEMCPY(p, &meta.vector_dim_, sizeof(int64_t)); p += sizeof(int64_t);
    MEMCPY(p, &meta.result_col_id_, sizeof(uint64_t)); p += sizeof(uint64_t);
    MEMCPY(p, &meta.state_col_id_, sizeof(uint64_t)); p += sizeof(uint64_t);
    MEMCPY(p, &meta.req_col_id_, sizeof(uint64_t)); p += sizeof(uint64_t);
    MEMCPY(p, &meta.spec_col_id_, sizeof(uint64_t)); p += sizeof(uint64_t);
    const common::ObString *strs[] = {
        &meta.sync_mode_, &meta.model_name_, &meta.system_prompt_, &meta.prompt_expr_,
        &meta.normalized_expr_, &meta.spec_json_, &meta.result_col_name_,
        &meta.state_col_name_, &meta.req_col_name_, &meta.spec_col_name_ };
    for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(sizeof(strs)/sizeof(strs[0])); ++i) {
      ret = ob_ai_write_str(p, *strs[i]);
    }
    if (OB_SUCC(ret)) {
      const int32_t dep_cnt = static_cast<int32_t>(meta.dep_column_ids_.count());
      MEMCPY(p, &dep_cnt, sizeof(dep_cnt)); p += sizeof(dep_cnt);
      for (int64_t i = 0; OB_SUCC(ret) && i < dep_cnt; ++i) {
        const uint64_t id = meta.dep_column_ids_.at(i);
        MEMCPY(p, &id, sizeof(id)); p += sizeof(id);
      }
    }
    if (OB_SUCC(ret)) {
      out.assign_ptr(buf, static_cast<int32_t>(p - buf));
    }
  }
  return ret;
}

// NOTE: strings inside `meta` point into `buf`; the caller must make sure the
// buffer (or a deep copy) outlives the struct, or pass an allocator and copy.
inline int parse_ai_meta(const common::ObString &buf, ObAiColumnMeta &meta)
{
  int ret = common::OB_SUCCESS;
  const char *p = buf.ptr();
  const char *end = buf.ptr() + buf.length();
  if (buf.empty()) {
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    int32_t func_kind = 0;
    int32_t result_kind = 0;
    MEMCPY(&func_kind, p, sizeof(func_kind)); p += sizeof(func_kind);
    MEMCPY(&result_kind, p, sizeof(result_kind)); p += sizeof(result_kind);
    MEMCPY(&meta.max_retries_, p, sizeof(int64_t)); p += sizeof(int64_t);
    MEMCPY(&meta.vector_dim_, p, sizeof(int64_t)); p += sizeof(int64_t);
    MEMCPY(&meta.result_col_id_, p, sizeof(uint64_t)); p += sizeof(uint64_t);
    MEMCPY(&meta.state_col_id_, p, sizeof(uint64_t)); p += sizeof(uint64_t);
    MEMCPY(&meta.req_col_id_, p, sizeof(uint64_t)); p += sizeof(uint64_t);
    MEMCPY(&meta.spec_col_id_, p, sizeof(uint64_t)); p += sizeof(uint64_t);
    meta.func_kind_ = static_cast<ObAiFuncKind>(func_kind);
    meta.result_kind_ = static_cast<ObAiResultKind>(result_kind);
    common::ObString *strs[] = {
        &meta.sync_mode_, &meta.model_name_, &meta.system_prompt_, &meta.prompt_expr_,
        &meta.normalized_expr_, &meta.spec_json_, &meta.result_col_name_,
        &meta.state_col_name_, &meta.req_col_name_, &meta.spec_col_name_ };
    for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(sizeof(strs)/sizeof(strs[0])); ++i) {
      ret = ob_ai_read_str(p, end, *strs[i]);
    }
    if (OB_SUCC(ret)) {
      int32_t dep_cnt = 0;
      if (p + sizeof(dep_cnt) > end) {
        ret = common::OB_ERR_UNEXPECTED;
      } else {
        MEMCPY(&dep_cnt, p, sizeof(dep_cnt)); p += sizeof(dep_cnt);
        for (int64_t i = 0; OB_SUCC(ret) && i < dep_cnt; ++i) {
          uint64_t id = 0;
          if (p + sizeof(id) > end) {
            ret = common::OB_ERR_UNEXPECTED;
          } else {
            MEMCPY(&id, p, sizeof(id)); p += sizeof(id);
            ret = meta.dep_column_ids_.push_back(id);
          }
        }
      }
    }
  }
  return ret;
}

} // namespace schema
} // namespace share
} // namespace oceanbase

#include "share/schema/ob_ai_column_text.h"

#endif // OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_META_H_
