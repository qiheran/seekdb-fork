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

#ifndef OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_TEXT_H_
#define OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_TEXT_H_

#include "lib/string/ob_string.h"
#include "lib/container/ob_array.h"
#include "lib/charset/ob_charset.h"
#include "share/ob_define.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

// Parse "FUNC(arg1, arg2, ...)" into func name and top-level args.
// Quote (single/double) and paren aware.
inline int ob_ai_extract_func_call(const common::ObString &expr,
                                   common::ObString &func_name,
                                   common::ObIArray<common::ObString> &args)
{
  int ret = common::OB_SUCCESS;
  const char *p = expr.ptr();
  const char *end = expr.ptr() + expr.length();
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
  const char *name_start = p;
  while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) ++p;
  func_name.assign_ptr(name_start, static_cast<int32_t>(p - name_start));
  while (p < end && (*p == ' ' || *p == '\t')) ++p;
  if (p >= end || *p != '(') {
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    ++p;
    int64_t depth = 1;
    const char *arg_start = p;
    bool in_squote = false, in_dquote = false, in_bquote = false;
    while (p < end && depth > 0) {
      char ch = *p;
      if (in_squote) {
        if (ch == '\\') { ++p; }
        else if (ch == '\'') { in_squote = false; }
      } else if (in_dquote) {
        if (ch == '\\') { ++p; }
        else if (ch == '"') { in_dquote = false; }
      } else if (in_bquote) {
        if (ch == '`') { in_bquote = false; }
      } else if (ch == '\'') {
        in_squote = true;
      } else if (ch == '"') {
        in_dquote = true;
      } else if (ch == '`') {
        in_bquote = true;
      } else if (ch == '(') {
        ++depth;
      } else if (ch == ')') {
        --depth;
        if (0 == depth) {
          common::ObString arg(static_cast<int32_t>(p - arg_start), arg_start);
          if (OB_FAIL(args.push_back(arg))) {}
        }
      } else if (ch == ',' && 1 == depth) {
        common::ObString arg(static_cast<int32_t>(p - arg_start), arg_start);
        if (OB_FAIL(args.push_back(arg))) {}
        arg_start = p + 1;
      }
      ++p;
    }
    if (OB_SUCC(ret) && depth != 0) {
      ret = common::OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

inline void ob_ai_trim(common::ObString &s)
{
  while (s.length() > 0 && (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r')) { s += 1; }
  while (s.length() > 0) {
    char c = s[s.length() - 1];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      s.assign_ptr(s.ptr(), s.length() - 1);
    } else {
      break;
    }
  }
}

// Strip one layer of quotes from a string literal ('abc' / "abc").
inline int ob_ai_unquote_literal(const common::ObString &in, common::ObString &out)
{
  int ret = common::OB_SUCCESS;
  common::ObString s = in;
  ob_ai_trim(s);
  if (s.length() >= 2 && ((s[0] == '\'' && s[s.length() - 1] == '\'') || (s[0] == '"' && s[s.length() - 1] == '"'))) {
    out.assign_ptr(s.ptr() + 1, s.length() - 2);
  } else if (s.length() == 0) {
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    out = s;
  }
  return ret;
}

inline bool ob_ai_is_ident_char(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c < 0;
}

// Check if `word` appears in `text` as a standalone identifier token, ignoring
// content inside string literals. Approximate (charset naive) match.
inline bool ob_ai_text_contains_ident(const common::ObString &text, const common::ObString &word)
{
  if (word.empty() || text.empty()) { return false; }
  const char *p = text.ptr();
  const int64_t n = text.length();
  bool in_squote = false, in_dquote = false, in_bquote = false;
  for (int64_t i = 0; i < n; ++i) {
    char ch = p[i];
    if (in_squote) {
      if (ch == '\\') { ++i; }
      else if (ch == '\'') { in_squote = false; }
    } else if (in_dquote) {
      if (ch == '\\') { ++i; }
      else if (ch == '"') { in_dquote = false; }
    } else if (in_bquote) {
      if (ch == '`') { in_bquote = false; }
    } else if (ch == '\'') {
      in_squote = true;
    } else if (ch == '"') {
      in_dquote = true;
    } else if (ch == '`') {
      in_bquote = true;
    } else if (ob_ai_is_ident_char(ch)
        && (0 == i || !ob_ai_is_ident_char(p[i-1]))
        && (i + word.length() <= n)) {
      bool eq = true;
      for (int64_t j = 0; j < word.length(); ++j) {
        char a = p[i + j];
        char b = word[j];
        if (a >= 'A' && a <= 'Z') { a = a - 'A' + 'a'; }
        if (b >= 'A' && b <= 'Z') { b = b - 'A' + 'a'; }
        if (a != b) { eq = false; break; }
      }
      if (eq && (i + word.length() >= n || !ob_ai_is_ident_char(p[i + word.length()]))) {
        return true;
      }
    }
  }
  return false;
}

// Escape a string for inclusion inside a JSON string value.
inline int ob_ai_escape_json(common::ObIAllocator &alloc, const common::ObString &in, common::ObString &out)
{
  int ret = common::OB_SUCCESS;
  char *buf = nullptr;
  const int64_t cap = in.length() * 6 + 8;
  if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(cap)))) {
    ret = common::OB_ALLOCATE_MEMORY_FAILED;
  } else {
    int64_t pos = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < in.length(); ++i) {
      char c = in[i];
      switch (c) {
        case '"': ret = databuff_printf(buf, cap, pos, "\\\""); break;
        case '\\': ret = databuff_printf(buf, cap, pos, "\\\\"); break;
        case '\n': ret = databuff_printf(buf, cap, pos, "\\n"); break;
        case '\r': ret = databuff_printf(buf, cap, pos, "\\r"); break;
        case '\t': ret = databuff_printf(buf, cap, pos, "\\t"); break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            ret = databuff_printf(buf, cap, pos, "\\u%04x", c);
          } else {
            buf[pos++] = c;
          }
          break;
      }
    }
    if (OB_SUCC(ret)) {
      out.assign_ptr(buf, static_cast<int32_t>(pos));
    }
  }
  return ret;
}

} // namespace schema
} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_TEXT_H_
