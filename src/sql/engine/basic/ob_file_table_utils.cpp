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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_file_table_utils.h"
#include <stdio.h>
#include <string.h>
#include <glob.h>
#include <sys/stat.h>
#include "lib/charset/ob_charset.h"
#include "lib/string/ob_string.h"
#include "common/timezone/ob_time_convert.h"
#include "lib/json_type/ob_json_tree.h"
#include "lib/json_type/ob_json_parse.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

static const int64_t MAX_FILE_TABLE_FILE_SIZE = 256LL * 1024 * 1024;

int ObFileTableUtils::read_local_file(ObIAllocator &alloc, const ObString &path, ObString &content)
{
  int ret = OB_SUCCESS;
  FILE *fp = NULL;
  char *buf = NULL;
  ObString npath;
  if (path.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("file path is empty", K(ret));
  } else if (OB_FAIL(ob_write_string(alloc, path, npath))) {
  } else if (npath.length() >= OB_MAX_FILE_NAME_LENGTH) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("file path too long", K(ret), K(npath.length()));
  } else {
    char cpath[OB_MAX_FILE_NAME_LENGTH];
    memcpy(cpath, npath.ptr(), npath.length());
    cpath[npath.length()] = '\0';
    struct stat st;
    if (0 != stat(cpath, &st)) {
      ret = OB_FILE_NOT_EXIST;
      LOG_USER_ERROR(OB_FILE_NOT_EXIST, cpath);
      LOG_WARN("cannot stat file", K(ret), K(cpath));
    } else if (S_ISDIR(st.st_mode)) {
      ret = OB_FILE_NOT_EXIST;
      LOG_USER_ERROR(OB_FILE_NOT_EXIST, cpath);
      LOG_WARN("path is a directory", K(ret), K(cpath));
    } else if (st.st_size > MAX_FILE_TABLE_FILE_SIZE) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("file too large for file table", K(ret), K(st.st_size));
    } else if (NULL == (fp = fopen(cpath, "rb"))) {
      ret = OB_FILE_NOT_OPENED;
      LOG_USER_ERROR(OB_FILE_NOT_OPENED, cpath);
      LOG_WARN("cannot open file", K(ret), K(cpath));
    } else {
      int64_t size = st.st_size;
      if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(size + 1)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("alloc memory failed", K(ret), K(size));
      } else if (size > 0 && 1 != fread(buf, size, 1, fp)) {
        ret = OB_IO_ERROR;
        LOG_WARN("read file failed", K(ret), K(size));
      } else {
        buf[size] = '\0';
        content.assign_ptr(buf, static_cast<int32_t>(size));
      }
    }
  }
  if (NULL != fp) {
    fclose(fp);
    fp = NULL;
  }
  return ret;
}

static bool is_int_str(const ObString &s, int64_t &out)
{
  if (s.empty() || s.length() >= 32) {
    return false;
  }
  char buf[32];
  memcpy(buf, s.ptr(), s.length());
  buf[s.length()] = '\0';
  char *end = NULL;
  errno = 0;
  int64_t v = strtoll(buf, &end, 10);
  if (errno == 0 && NULL != end && *end == '\0' && end != buf) {
    out = v;
    return true;
  }
  return false;
}

static bool is_double_str(const ObString &s, double &out)
{
  if (s.empty() || s.length() >= 64) {
    return false;
  }
  char buf[64];
  memcpy(buf, s.ptr(), s.length());
  buf[s.length()] = '\0';
  char *end = NULL;
  errno = 0;
  double v = strtod(buf, &end);
  if (errno == 0 && NULL != end && *end == '\0' && end != buf) {
    out = v;
    return true;
  }
  return false;
}

static bool is_bool_str(const ObString &s, bool &out)
{
  if (s.case_compare("true") == 0) {
    out = true;
    return true;
  } else if (s.case_compare("false") == 0) {
    out = false;
    return true;
  }
  return false;
}

static bool is_date_str(const ObString &s)
{
  // YYYY-MM-DD
  if (s.length() != 10) {
    return false;
  }
  const char *p = s.ptr();
  for (int32_t i = 0; i < 10; ++i) {
    if (i == 4 || i == 7) {
      if (p[i] != '-') { return false; }
    } else if (p[i] < '0' || p[i] > '9') {
      return false;
    }
  }
  int32_t month = (p[5] - '0') * 10 + (p[6] - '0');
  int32_t day = (p[8] - '0') * 10 + (p[9] - '0');
  return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

static bool is_datetime_str(const ObString &s)
{
  // YYYY-MM-DD[ T]HH:MM:SS
  if (s.length() != 19) {
    return false;
  }
  if (!is_date_str(s.substring(0, 10))) {
    return false;
  }
  const char *p = s.ptr();
  if (p[10] != ' ' && p[10] != 'T') {
    return false;
  }
  for (int32_t i = 11; i < 19; ++i) {
    if (i == 13 || i == 16) {
      if (p[i] != ':') { return false; }
    } else if (p[i] < '0' || p[i] > '9') {
      return false;
    }
  }
  int32_t hour = (p[11] - '0') * 10 + (p[12] - '0');
  int32_t minute = (p[14] - '0') * 10 + (p[15] - '0');
  int32_t second = (p[17] - '0') * 10 + (p[18] - '0');
  return hour < 24 && minute < 60 && second < 60;
}

// ---------------- csv ----------------

struct ObCsvFieldBuffer
{
  ObIAllocator &alloc_;
  char *buf_;
  int64_t cap_;
  int64_t pos_;
  explicit ObCsvFieldBuffer(ObIAllocator &alloc)
      : alloc_(alloc), buf_(NULL), cap_(0), pos_(0)
  {}
  int append(char c)
  {
    int ret = OB_SUCCESS;
    if (pos_ >= cap_) {
      int64_t new_cap = (cap_ == 0) ? 64 : cap_ * 2;
      char *nb = static_cast<char *>(alloc_.alloc(new_cap));
      if (OB_ISNULL(nb)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (pos_ > 0) {
          memcpy(nb, buf_, pos_);
        }
        buf_ = nb;
        cap_ = new_cap;
      }
    }
    if (OB_SUCC(ret)) {
      buf_[pos_++] = c;
    }
    return ret;
  }
  ObString slice()
  {
    return ObString(static_cast<int32_t>(pos_), buf_);
  }
};

int ObFileTableUtils::parse_csv_records(ObIAllocator &alloc, const ObString &content,
                                        ObIArray<ObIArray<ObString> > &records)
{
  int ret = OB_SUCCESS;
  int32_t len = content.length();
  const char *p = content.ptr();
  ObSEArray<ObString, 16> cur_row;
  cur_row.set_allocator(&alloc);
  ObCsvFieldBuffer fb(alloc);
  bool in_quotes = false;
  bool row_has_content = false;
  auto finish_field = [&]() -> int {
    int r = OB_SUCCESS;
    OZ(cur_row.push_back(fb.slice()));
    fb.pos_ = 0;
    return r;
  };
  auto finish_row = [&]() -> int {
    int r = OB_SUCCESS;
    if (row_has_content || cur_row.count() > 0) {
      if (OB_FAIL(records.push_back(cur_row))) {
      }
    }
    cur_row.reset();
    row_has_content = false;
    return r;
  };
  for (int32_t i = 0; OB_SUCC(ret) && i < len; ++i) {
    char c = p[i];
    if (in_quotes) {
      if (c == '"') {
        if (i + 1 < len && p[i + 1] == '"') {
          OZ(fb.append('"'));
          i++;
        } else {
          in_quotes = false;
        }
      } else {
        OZ(fb.append(c));
      }
    } else if (c == '"' && fb.pos_ == 0) {
      // field starting with a quote (any quote after start handled as content)
      in_quotes = true;
      row_has_content = true;
    } else if (c == ',') {
      OZ(finish_field());
      row_has_content = true;
    } else if (c == '\r') {
      if (i + 1 < len && p[i + 1] == '\n') {
        i++;
      }
      OZ(finish_field());
      OZ(finish_row());
    } else if (c == '\n') {
      OZ(finish_field());
      OZ(finish_row());
    } else {
      OZ(fb.append(c));
      row_has_content = true;
    }
  }
  if (OB_SUCC(ret)) {
    if (fb.pos_ > 0 || cur_row.count() > 0 || row_has_content) {
      OZ(finish_field());
      OZ(finish_row());
    }
  }
  return ret;
}

static int infer_col_type(const ObIArray<ObIArray<ObString> > &records,
                          int64_t first_data_row, int64_t c,
                          ObFileTableColDef &col)
{
  int ret = OB_SUCCESS;
  bool has_value = false;
  bool has_empty = false;
  bool all_bool = true;
  bool all_int = true;
  bool all_num = true;
  bool all_date = true;
  bool all_datetime = true;
  for (int64_t r = first_data_row; OB_SUCC(ret) && r < records.count(); ++r) {
    const ObIArray<ObString> &row = records.at(r);
    ObString val;
    if (c < row.count()) {
      val = row.at(c);
    }
    if (val.empty()) {
      has_empty = true;
      continue;
    }
    has_value = true;
    bool b = false;
    int64_t iv = 0;
    double dv = 0;
    if (!is_bool_str(val, b)) {
      all_bool = false;
    }
    if (!is_int_str(val, iv)) {
      all_int = false;
    }
    if (!is_int_str(val, iv) && !is_double_str(val, dv)) {
      all_num = false;
    }
    if (!is_date_str(val)) {
      all_date = false;
    }
    if (!is_datetime_str(val)) {
      all_datetime = false;
    }
  }
  col.nullable_ = has_empty || !has_value;
  if (!has_value) {
    col.col_type_ = ObVarcharType;
  } else if (all_datetime) {
    col.col_type_ = ObDateTimeType;
  } else if (all_date) {
    col.col_type_ = ObDateType;
  } else if (all_bool) {
    col.col_type_ = ObTinyIntType;
  } else if (all_int) {
    col.col_type_ = ObIntType;
  } else if (all_num) {
    col.col_type_ = ObDoubleType;
  } else {
    col.col_type_ = ObVarcharType;
  }
  return ret;
}

static bool name_exists(const ObIArray<ObFileTableColDef> &columns, const ObString &name,
                        int64_t limit)
{
  for (int64_t i = 0; i < limit; ++i) {
    if (columns.at(i).name_.case_compare(name) == 0) {
      return true;
    }
  }
  return false;
}

static int store_unique_name(ObIAllocator &alloc, const ObString &name,
                             ObIArray<ObFileTableColDef> &columns, int64_t self_idx,
                             ObString &stored)
{
  int ret = OB_SUCCESS;
  ObString cand = name;
  if (name_exists(columns, cand, self_idx)) {
    int64_t suffix = 2;
    while (OB_SUCC(ret)) {
      char buf[OB_MAX_COLUMN_NAME_LENGTH];
      int64_t pos = 0;
      if (OB_FAIL(databuff_printf(buf, sizeof(buf), pos, "%.*s__%ld",
                                  name.length(), name.ptr(), suffix))) {
      } else {
        ObString try_name(static_cast<int32_t>(pos), buf);
        if (!name_exists(columns, try_name, self_idx)) {
          cand = try_name;
          break;
        }
        suffix++;
      }
    }
  }
  if (OB_SUCC(ret)) {
    OZ(ob_write_string(alloc, cand, stored));
  }
  return ret;
}

// ---------------- json line helpers ----------------

static int collect_json_line(ObIAllocator &alloc, ObJsonObject *jobj,
                             ObIArray<ObString> &names,
                             ObIArray<int32_t> &kinds) // per column: agg flags
{
  int ret = OB_SUCCESS;
  int64_t cnt = jobj->element_count();
  for (int64_t i = 0; OB_SUCC(ret) && i < cnt; ++i) {
    ObString key;
    if (OB_FAIL(jobj->get_key(i, key))) {
      LOG_WARN("get json key failed", K(ret));
    } else {
      bool found = false;
      for (int64_t j = 0; !found && j < names.count(); ++j) {
        if (names.at(j).case_compare(key) == 0) {
          found = true;
        }
      }
      if (!found) {
        ObString stored;
        OZ(ob_write_string(alloc, key, stored));
        OZ(names.push_back(stored));
      }
    }
  }
  UNUSED(kinds);
  return ret;
}

static int infer_json_col_type(ObIAllocator &alloc, const ObString &content,
                               const ObIArray<ObString> &names,
                               ObIArray<ObFileTableColDef> &columns)
{
  int ret = OB_SUCCESS;
  // flags per column
  ObSEArray<int32_t, 8> flags; // bit0 has_value, bit1 has_other(int), bit2 has_double,
                               // bit3 has_bool, bit4 has_date, bit5 has_datetime, bit6 has_other_str
  ObSEArray<int32_t, 8> nullable;
  for (int64_t c = 0; OB_SUCC(ret) && c < names.count(); ++c) {
    OZ(flags.push_back(0));
    OZ(nullable.push_back(1));
  }
  int32_t len = content.length();
  const char *p = content.ptr();
  int32_t i = 0;
  auto flag_has_value = 0x1;
  auto flag_int = 0x2;
  auto flag_double = 0x4;
  auto flag_bool = 0x8;
  auto flag_date = 0x10;
  auto flag_datetime = 0x20;
  auto flag_str = 0x40;
  while (OB_SUCC(ret) && i < len) {
    while (i < len && (p[i] == '\n' || p[i] == '\r' || p[i] == ' ' || p[i] == '\t')) {
      i++;
    }
    if (i >= len) {
      break;
    }
    int32_t line_start = i;
    while (i < len && p[i] != '\n' && p[i] != '\r') {
      i++;
    }
    ObString line(static_cast<int32_t>(p + i - (p + line_start)), p + line_start);
    line = line.trim_space();
    if (line.empty()) {
      continue;
    }
    ObJsonNode *jbase = NULL;
    if (OB_FAIL(ObJsonBaseFactory::get_json_base(&alloc, line, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, reinterpret_cast<ObIJsonBase *&>(jbase)))) {
      LOG_WARN("parse json line failed", K(ret));
    } else if (OB_ISNULL(jbase) || jbase->json_type() != ObJsonNodeType::J_OBJECT) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("json line is not object", K(ret));
    } else {
      ObJsonObject *jobj = static_cast<ObJsonObject *>(jbase);
      for (int64_t c = 0; OB_SUCC(ret) && c < names.count(); ++c) {
        ObJsonNode *node = jobj->get_value(names.at(c));
        if (NULL == node || node->json_type() == ObJsonNodeType::J_NULL) {
          continue;
        }
        flags.at(c) |= flag_has_value;
        switch (node->json_type()) {
          case ObJsonNodeType::J_INT:
          case ObJsonNodeType::J_UINT:
            flags.at(c) |= flag_int;
            break;
          case ObJsonNodeType::J_DOUBLE:
          case ObJsonNodeType::J_DECIMAL:
            flags.at(c) |= flag_int | flag_double;
            break;
          case ObJsonNodeType::J_BOOLEAN:
            flags.at(c) |= flag_bool;
            break;
          case ObJsonNodeType::J_STRING: {
            ObJsonString *snode = static_cast<ObJsonString *>(node);
            ObString sv(static_cast<int32_t>(snode->get_data_length()), snode->get_data());
            if (is_datetime_str(sv)) {
              flags.at(c) |= flag_datetime;
            } else if (is_date_str(sv)) {
              flags.at(c) |= flag_date;
            } else {
              flags.at(c) |= flag_str;
            }
            flags.at(c) |= flag_int | flag_double; // string convertible to double? no;
                                                   // but keep separate: remove below
            flags.at(c) &= ~(flag_int | flag_double);
            break;
          }
          default:
            flags.at(c) |= flag_str;
            break;
        }
      }
    }
  }
  for (int64_t c = 0; OB_SUCC(ret) && c < names.count(); ++c) {
    ObFileTableColDef col;
    int32_t f = flags.at(c);
    if (!(f & flag_has_value)) {
      col.col_type_ = ObVarcharType;
    } else if (f & flag_datetime) {
      col.col_type_ = ObDateTimeType;
    } else if (f & flag_date) {
      col.col_type_ = ObDateType;
    } else if (f & flag_bool) {
      col.col_type_ = ObTinyIntType;
    } else if (f & flag_int) {
      col.col_type_ = ObIntType;
    } else if (f & flag_double) {
      col.col_type_ = ObDoubleType;
    } else {
      col.col_type_ = ObVarcharType;
    }
    OZ(columns.push_back(col));
    ObString stored;
    OZ(store_unique_name(alloc, names.at(c), columns, c, stored));
    columns.at(c).name_ = stored;
  }
  return ret;
}

int ObFileTableUtils::infer_schema(ObIAllocator &alloc, ObFileTableFuncKind kind,
                                   const ObString &path, bool has_header,
                                   const ObString &format,
                                   ObIArray<ObFileTableColDef> &columns)
{
  int ret = OB_SUCCESS;
  if (FILE_FUNC_GLOB == kind) {
    ObFileTableColDef col;
    col.col_type_ = ObVarcharType;
    col.nullable_ = false;
    ObString stored;
    OZ(store_unique_name(alloc, ObString("file"), columns, 0, stored));
    col.name_ = stored;
    OZ(columns.push_back(col));
  } else if (FILE_FUNC_READ_PARQUET == kind) {
    OZ(scan_parquet(alloc, path, columns, NULL));
  } else if (FILE_FUNC_READ_CSV == kind) {
    ObString content;
    OZ(read_local_file(alloc, path, content));
    ObSEArray<ObIArray<ObString>, 16> records;
    records.set_allocator(&alloc);
    OZ(parse_csv_records(alloc, content, records));
    if (OB_SUCC(ret)) {
      if (records.count() == 0) {
        // empty file: no columns
      } else if (has_header) {
        const ObIArray<ObString> &header = records.at(0);
        int64_t ncols = 0;
        for (int64_t r = 1; r < records.count(); ++r) {
          ncols = std::max(ncols, records.at(r).count());
        }
        for (int64_t c = 0; OB_SUCC(ret) && c < ncols; ++c) {
          ObFileTableColDef col;
          OZ(infer_col_type(records, 1, c, col));
          ObString name;
          if (c < header.count() && !header.at(c).empty()) {
            name = header.at(c);
          } else {
            char buf[64];
            int64_t pos = 0;
            if (OB_FAIL(databuff_printf(buf, sizeof(buf), pos, "column%ld", c))) {
            } else {
              name.assign_ptr(buf, static_cast<int32_t>(pos));
            }
          }
          if (OB_SUCC(ret)) {
            ObString stored;
            OZ(store_unique_name(alloc, name, columns, c, stored));
            col.name_ = stored;
            OZ(columns.push_back(col));
          }
        }
      } else {
        int64_t ncols = 0;
        for (int64_t r = 0; r < records.count(); ++r) {
          ncols = std::max(ncols, records.at(r).count());
        }
        for (int64_t c = 0; OB_SUCC(ret) && c < ncols; ++c) {
          ObFileTableColDef col;
          OZ(infer_col_type(records, 0, c, col));
          char buf[64];
          int64_t pos = 0;
          if (OB_FAIL(databuff_printf(buf, sizeof(buf), pos, "column%ld", c))) {
          } else {
            ObString stored;
            OZ(ob_write_string(alloc, ObString(static_cast<int32_t>(pos), buf), stored));
            col.name_ = stored;
            OZ(columns.push_back(col));
          }
        }
      }
    }
  } else if (FILE_FUNC_READ_JSON == kind) {
    ObString content;
    OZ(read_local_file(alloc, path, content));
    // collect names in first-seen order
    ObSEArray<ObString, 8> names;
    int32_t len = content.length();
    const char *p = content.ptr();
    int32_t i = 0;
    while (OB_SUCC(ret) && i < len) {
      while (i < len && (p[i] == '\n' || p[i] == '\r' || p[i] == ' ' || p[i] == '\t')) {
        i++;
      }
      if (i >= len) {
        break;
      }
      int32_t line_start = i;
      while (i < len && p[i] != '\n' && p[i] != '\r') {
        i++;
      }
      ObString line(static_cast<int32_t>(p + i - (p + line_start)), p + line_start);
      line = line.trim_space_only();
      if (line.empty()) {
        continue;
      }
      ObJsonNode *jbase = NULL;
      if (OB_FAIL(ObJsonBaseFactory::get_json_base(&alloc, line, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, reinterpret_cast<ObIJsonBase *&>(jbase)))) {
        LOG_WARN("parse json line failed", K(ret));
      } else if (OB_ISNULL(jbase) || jbase->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("json line is not object", K(ret));
      } else {
        ObSEArray<int32_t, 8> kinds;
        OZ(collect_json_line(alloc, static_cast<ObJsonObject *>(jbase), names, kinds));
      }
    }
    if (OB_SUCC(ret)) {
      OZ(infer_json_col_type(alloc, content, names, columns));
    }
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("unsupported file table function kind", K(ret), K(kind));
  }
  return ret;
}

int ObFileTableUtils::convert_value(ObIAllocator &alloc, const ObString &val,
                                    const ObFileTableColDef &col, ObObj &obj)
{
  int ret = OB_SUCCESS;
  if (val.empty()) {
    obj.set_null();
  } else {
    switch (col.col_type_) {
      case ObIntType: {
        int64_t v = 0;
        if (OB_UNLIKELY(!is_int_str(val, v))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("convert int failed", K(ret), K(val));
        } else {
          obj.set_int(v);
        }
        break;
      }
      case ObDoubleType: {
        double v = 0;
        if (OB_UNLIKELY(!is_double_str(val, v))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("convert double failed", K(ret), K(val));
        } else {
          obj.set_double(v);
        }
        break;
      }
      case ObTinyIntType: {
        bool b = false;
        if (OB_UNLIKELY(!is_bool_str(val, b))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("convert bool failed", K(ret), K(val));
        } else {
          obj.set_tinyint(b ? 1 : 0);
        }
        break;
      }
      case ObDateType: {
        int32_t v = 0;
        if (OB_FAIL(ObTimeConverter::str_to_date(val, v))) {
          LOG_WARN("convert date failed", K(ret), K(val));
        } else {
          obj.set_type(ObDateType);
          obj.val_.int32_ = v;
        }
        break;
      }
      case ObDateTimeType: {
        ObTimeConvertCtx cvrt_ctx(NULL, false);
        int64_t v = 0;
        int16_t scale = 6;
        if (OB_FAIL(ObTimeConverter::str_to_datetime(val, cvrt_ctx, v, &scale))) {
          LOG_WARN("convert datetime failed", K(ret), K(val));
        } else {
          obj.set_type(ObDateTimeType);
          obj.val_.int64_ = v;
        }
        break;
      }
      default: {
        obj.set_varchar(val);
        obj.set_collation_type(col.collation_type_);
        obj.set_collation_level(CS_LEVEL_IMPLICIT);
        break;
      }
    }
  }
  return ret;
}

static int make_new_row(ObIAllocator &alloc, ObObj *cells, int64_t ncols, ObNewRow &row)
{
  row.assign(cells, ncols);
  return OB_SUCCESS;
}

static int convert_json_node_full(ObIAllocator &alloc, ObJsonNode *node,
                                  const ObFileTableColDef &col, ObObj &obj)
{
  int ret = OB_SUCCESS;
  switch (node->json_type()) {
    case ObJsonNodeType::J_INT: {
      int64_t v = static_cast<ObJsonInt *>(node)->value();
      if (col.col_type_ == ObIntType) {
        obj.set_int(v);
      } else if (col.col_type_ == ObDoubleType) {
        obj.set_double(static_cast<double>(v));
      } else if (col.col_type_ == ObTinyIntType) {
        obj.set_tinyint(v != 0 ? 1 : 0);
      } else if (col.col_type_ == ObDateTimeType || col.col_type_ == ObDateType) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        char buf[32];
        int64_t pos = 0;
        if (OB_FAIL(databuff_printf(buf, sizeof(buf), pos, "%ld", v))) {
        } else {
          ObString s(static_cast<int32_t>(pos), buf);
          OZ(ob_write_string(alloc, s, s));
          obj.set_varchar(s);
          obj.set_collation_type(col.collation_type_);
          obj.set_collation_level(CS_LEVEL_IMPLICIT);
        }
      }
      break;
    }
    case ObJsonNodeType::J_UINT: {
      uint64_t v = static_cast<ObJsonUint *>(node)->value();
      if (col.col_type_ == ObIntType) {
        obj.set_int(static_cast<int64_t>(v));
      } else if (col.col_type_ == ObDoubleType) {
        obj.set_double(static_cast<double>(v));
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
      break;
    }
    case ObJsonNodeType::J_DOUBLE: {
      double v = static_cast<ObJsonDouble *>(node)->value();
      if (col.col_type_ == ObDoubleType) {
        obj.set_double(v);
      } else if (col.col_type_ == ObIntType) {
        obj.set_int(static_cast<int64_t>(v));
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
      break;
    }
    case ObJsonNodeType::J_DECIMAL: {
      number::ObNumber nm = static_cast<ObJsonDecimal *>(node)->value();
      double v = 0;
      if (OB_FAIL(nm.to_double(v))) {
      } else if (col.col_type_ == ObDoubleType) {
        obj.set_double(v);
      } else if (col.col_type_ == ObIntType) {
        obj.set_int(static_cast<int64_t>(v));
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
      break;
    }
    case ObJsonNodeType::J_BOOLEAN: {
      bool v = static_cast<ObJsonBoolean *>(node)->value();
      if (col.col_type_ == ObTinyIntType) {
        obj.set_tinyint(v ? 1 : 0);
      } else if (col.col_type_ == ObIntType) {
        obj.set_int(v ? 1 : 0);
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
      break;
    }
    case ObJsonNodeType::J_STRING: {
      ObJsonString *snode = static_cast<ObJsonString *>(node);
      ObString sv(static_cast<int32_t>(snode->get_data_length()), snode->get_data());
      if (col.col_type_ == ObDateType || col.col_type_ == ObDateTimeType) {
        OZ(ObFileTableUtils::convert_value(alloc, sv, col, obj));
      } else {
        ObString stored;
        OZ(ob_write_string(alloc, sv, stored));
        obj.set_varchar(stored);
        obj.set_collation_type(col.collation_type_);
        obj.set_collation_level(CS_LEVEL_IMPLICIT);
      }
      break;
    }
    default: {
      ret = OB_ERR_UNEXPECTED;
      break;
    }
  }
  return ret;
}

int ObFileTableUtils::scan_rows(ObIAllocator &alloc, ObFileTableFuncKind kind,
                                const ObString &path, bool has_header,
                                const ObString &format,
                                const ObIArray<ObFileTableColDef> &columns,
                                ObIArray<ObNewRow> &rows)
{
  int ret = OB_SUCCESS;
  if (FILE_FUNC_GLOB == kind) {
    ObSEArray<ObString, 8> files;
    OZ(list_glob(alloc, path, files));
    for (int64_t i = 0; OB_SUCC(ret) && i < files.count(); ++i) {
      ObObj *cells = static_cast<ObObj *>(alloc.alloc(sizeof(ObObj)));
      if (OB_ISNULL(cells)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        cells = new (cells) ObObj();
        cells->set_varchar(files.at(i));
        cells->set_collation_type(CS_TYPE_UTF8MB4_BIN);
        cells->set_collation_level(CS_LEVEL_IMPLICIT);
        ObNewRow row;
        OZ(make_new_row(alloc, cells, 1, row));
        OZ(rows.push_back(row));
      }
    }
  } else if (FILE_FUNC_READ_PARQUET == kind) {
    OZ(scan_parquet(alloc, path, const_cast<ObIArray<ObFileTableColDef> &>(columns), &rows));
  } else if (FILE_FUNC_READ_CSV == kind) {
    ObString content;
    OZ(read_local_file(alloc, path, content));
    ObSEArray<ObIArray<ObString>, 16> records;
    records.set_allocator(&alloc);
    OZ(parse_csv_records(alloc, content, records));
    int64_t first_data_row = has_header ? 1 : 0;
    for (int64_t r = first_data_row; OB_SUCC(ret) && r < records.count(); ++r) {
      const ObIArray<ObString> &rec = records.at(r);
      if (rec.count() == 1 && rec.at(0).empty()) {
        continue; // blank line
      }
      ObObj *cells = static_cast<ObObj *>(alloc.alloc(sizeof(ObObj) * columns.count()));
      if (OB_ISNULL(cells)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        for (int64_t c = 0; OB_SUCC(ret) && c < columns.count(); ++c) {
          new (cells + c) ObObj();
          ObString val;
          if (c < rec.count()) {
            val = rec.at(c);
          }
          OZ(convert_value(alloc, val, columns.at(c), cells[c]));
        }
        if (OB_SUCC(ret)) {
          ObNewRow row;
          OZ(make_new_row(alloc, cells, static_cast<int64_t>(columns.count()), row));
          OZ(rows.push_back(row));
        }
      }
    }
  } else if (FILE_FUNC_READ_JSON == kind) {
    ObString content;
    OZ(read_local_file(alloc, path, content));
    int32_t len = content.length();
    const char *p = content.ptr();
    int32_t i = 0;
    while (OB_SUCC(ret) && i < len) {
      while (i < len && (p[i] == '\n' || p[i] == '\r' || p[i] == ' ' || p[i] == '\t')) {
        i++;
      }
      if (i >= len) {
        break;
      }
      int32_t line_start = i;
      while (i < len && p[i] != '\n' && p[i] != '\r') {
        i++;
      }
      ObString line(static_cast<int32_t>(p + i - (p + line_start)), p + line_start);
      line = line.trim_space_only();
      if (line.empty()) {
        continue;
      }
      ObJsonNode *jbase = NULL;
      if (OB_FAIL(ObJsonBaseFactory::get_json_base(&alloc, line, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, reinterpret_cast<ObIJsonBase *&>(jbase)))) {
        LOG_WARN("parse json line failed", K(ret));
      } else if (OB_ISNULL(jbase) || jbase->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("json line is not object", K(ret));
      } else {
        ObJsonObject *jobj = static_cast<ObJsonObject *>(jbase);
        ObObj *cells = static_cast<ObObj *>(alloc.alloc(sizeof(ObObj) * columns.count()));
        if (OB_ISNULL(cells)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          for (int64_t c = 0; OB_SUCC(ret) && c < columns.count(); ++c) {
            new (cells + c) ObObj();
            ObJsonNode *node = jobj->get_value(columns.at(c).name_);
            if (NULL == node || node->json_type() == ObJsonNodeType::J_NULL) {
              cells[c].set_null();
            } else if (OB_FAIL(convert_json_node_full(alloc, node, columns.at(c), cells[c]))) {
              LOG_WARN("convert json node failed", K(ret));
            }
          }
          if (OB_SUCC(ret)) {
            ObNewRow row;
            OZ(make_new_row(alloc, cells, static_cast<int64_t>(columns.count()), row));
            OZ(rows.push_back(row));
          }
        }
      }
    }
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

int ObFileTableUtils::list_glob(ObIAllocator &alloc, const ObString &pattern,
                                ObIArray<ObString> &files)
{
  int ret = OB_SUCCESS;
  ObString npat;
  OZ(ob_write_string(alloc, pattern, npat));
  char cpat[OB_MAX_FILE_NAME_LENGTH];
  if (OB_FAIL(ret)) {
  } else if (npat.length() >= OB_MAX_FILE_NAME_LENGTH) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("glob pattern too long", K(ret));
  } else {
    memcpy(cpat, npat.ptr(), npat.length());
    cpat[npat.length()] = '\0';
    glob_t g;
    memset(&g, 0, sizeof(g));
    int gr = ::glob(cpat, GLOB_MARK, NULL, &g);
    if (0 != gr) {
      globfree(&g);
    } else {
      for (size_t i = 0; OB_SUCC(ret) && i < g.gl_pathc; ++i) {
        struct stat st;
        if (0 == stat(g.gl_pathv[i], &st) && S_ISREG(st.st_mode)) {
          ObString fpath(static_cast<int32_t>(strlen(g.gl_pathv[i])), g.gl_pathv[i]);
          ObString stored;
          OZ(ob_write_string(alloc, fpath, stored));
          OZ(files.push_back(stored));
        }
      }
      globfree(&g);
    }
  }
  return ret;
}

// ---------------- snappy ----------------

int ObFileTableUtils::snappy_uncompress(const char *src, int64_t src_len, char *dst, int64_t dst_len)
{
  int ret = OB_SUCCESS;
  int64_t sp = 0;
  uint64_t ulen = 0;
  int shift = 0;
  while (sp < src_len) {
    uint8_t b = static_cast<uint8_t>(src[sp++]);
    ulen |= static_cast<uint64_t>(b & 0x7f) << shift;
    if (!(b & 0x80)) {
      break;
    }
    shift += 7;
  }
  if (ulen != static_cast<uint64_t>(dst_len)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("snappy length mismatch", K(ret), K(ulen), K(dst_len));
  }
  int64_t dp = 0;
  while (OB_SUCC(ret) && sp < src_len) {
    uint8_t tag = static_cast<uint8_t>(src[sp++]);
    uint8_t tag_type = tag & 0x3;
    if (tag_type == 0) {
      int64_t lit_len = (tag >> 2) + 1;
      if (lit_len > 60) {
        int64_t extra = lit_len - 60;
        lit_len = 0;
        for (int64_t k = 0; k < extra; ++k) {
          lit_len |= static_cast<int64_t>(static_cast<uint8_t>(src[sp++])) << (8 * k);
        }
        lit_len += 1;
      }
      if (sp + lit_len > src_len || dp + lit_len > dst_len) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("snappy literal overflow", K(ret));
      } else {
        memcpy(dst + dp, src + sp, lit_len);
        sp += lit_len;
        dp += lit_len;
      }
    } else {
      int64_t copy_len = 0;
      int64_t offset = 0;
      if (tag_type == 1) {
        copy_len = ((tag >> 2) & 0x7) + 4;
        offset = ((static_cast<int64_t>(tag & 0xe0)) << 3) | static_cast<uint8_t>(src[sp++]);
      } else if (tag_type == 2) {
        copy_len = (tag >> 2) + 1;
        offset = static_cast<uint8_t>(src[sp]) | (static_cast<uint8_t>(src[sp + 1]) << 8);
        sp += 2;
      } else {
        copy_len = (tag >> 2) + 1;
        offset = static_cast<uint8_t>(src[sp]) | (static_cast<uint8_t>(src[sp + 1]) << 8) |
                 (static_cast<uint8_t>(src[sp + 2]) << 16) | (static_cast<uint8_t>(src[sp + 3]) << 24);
        sp += 4;
      }
      if (offset > dp || dp + copy_len > dst_len) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("snappy copy overflow", K(ret));
      } else {
        for (int64_t k = 0; k < copy_len; ++k) {
          dst[dp + k] = dst[dp - offset + k];
        }
        dp += copy_len;
      }
    }
  }
  if (OB_SUCC(ret) && dp != dst_len) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("snappy output length mismatch", K(ret), K(dp), K(dst_len));
  }
  return ret;
}

// ---------------- parquet ----------------

struct ObParquetThriftReader
{
  const char *buf_;
  int64_t len_;
  int64_t pos_;
  ObParquetThriftReader(const char *buf, int64_t len) : buf_(buf), len_(len), pos_(0) {}
  int read_uleb(uint64_t &v)
  {
    int ret = OB_SUCCESS;
    v = 0;
    int shift = 0;
    while (OB_SUCC(ret) && pos_ < len_) {
      uint8_t b = static_cast<uint8_t>(buf_[pos_++]);
      v |= static_cast<uint64_t>(b & 0x7f) << shift;
      if (!(b & 0x80)) {
        break;
      }
      shift += 7;
    }
    return ret;
  }
  int read_zigzag(int64_t &v)
  {
    uint64_t u = 0;
    int ret = read_uleb(u);
    v = static_cast<int64_t>(u >> 1) ^ -static_cast<int64_t>(u & 1);
    return ret;
  }
  int read_field_begin(int16_t &field_id, int8_t &type, int16_t last_id)
  {
    int ret = OB_SUCCESS;
    if (pos_ >= len_) {
      type = 0;
      field_id = 0;
    } else {
      uint8_t b = static_cast<uint8_t>(buf_[pos_++]);
      if (b == 0) {
        type = 0;
        field_id = 0;
      } else {
        int8_t delta = (b >> 4) & 0x0f;
        type = b & 0x0f;
        if (delta == 0) {
          uint64_t id = 0;
          OZ(read_uleb(id));
          field_id = static_cast<int16_t>(id);
        } else {
          field_id = last_id + delta;
        }
      }
    }
    return ret;
  }
  int skip(int8_t type)
  {
    int ret = OB_SUCCESS;
    int16_t fid = 0;
    int8_t t = 0;
    switch (type) {
      case 1:
      case 2: break;
      case 3: case 4: case 5: case 6: {
        int64_t v;
        OZ(read_zigzag(v));
        break;
      }
      case 7: pos_ += 8; break;
      case 8: {
        uint64_t l = 0;
        OZ(read_uleb(l));
        pos_ += l;
        break;
      }
      case 9:
      case 10:
      case 11: {
        uint8_t hb = static_cast<uint8_t>(buf_[pos_++]);
        int64_t sz = (hb >> 4) & 0x0f;
        int8_t et = hb & 0x0f;
        if (sz == 15) {
          uint64_t l2 = 0;
          OZ(read_uleb(l2));
          sz = l2;
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < sz; ++i) {
          OZ(skip(et));
        }
        break;
      }
      case 12: {
        while (OB_SUCC(ret)) {
          OZ(read_field_begin(fid, t, 0));
          if (t == 0) {
            break;
          }
          OZ(skip(t));
        }
        break;
      }
      default:
        ret = OB_ERR_UNEXPECTED;
        break;
    }
    return ret;
  }
};

struct ObParquetColValue
{
  ObObjType type_;
  int64_t int_val_;
  double dbl_val_;
  ObString str_val_;
  bool bool_val_;
  ObParquetColValue() : type_(ObVarcharType), int_val_(0), dbl_val_(0), bool_val_(false) {}
};

static int parquet_plain_decode(ObIAllocator &alloc, const char *data, int64_t len,
                                int32_t parquet_type, int64_t count,
                                ObIArray<ObParquetColValue> &vals)
{
  int ret = OB_SUCCESS;
  int64_t p = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    ObParquetColValue v;
    switch (parquet_type) {
      case 0: { // BOOLEAN bit-packed lsb
        if (p >= len) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          v.bool_val_ = (data[p] >> (i % 8)) & 1;
          if ((i + 1) % 8 == 0) {
            p++;
          }
          v.type_ = ObTinyIntType;
        }
        break;
      }
      case 1: { // INT32
        if (p + 4 > len) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          int32_t x;
          memcpy(&x, data + p, 4);
          p += 4;
          v.int_val_ = x;
          v.type_ = ObIntType;
        }
        break;
      }
      case 2: { // INT64
        if (p + 8 > len) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          int64_t x;
          memcpy(&x, data + p, 8);
          p += 8;
          v.int_val_ = x;
          v.type_ = ObIntType;
        }
        break;
      }
      case 4: { // FLOAT
        if (p + 4 > len) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          float x;
          memcpy(&x, data + p, 4);
          p += 4;
          v.dbl_val_ = x;
          v.type_ = ObDoubleType;
        }
        break;
      }
      case 5: { // DOUBLE
        if (p + 8 > len) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          double x;
          memcpy(&x, data + p, 8);
          p += 8;
          v.dbl_val_ = x;
          v.type_ = ObDoubleType;
        }
        break;
      }
      case 6: { // BYTE_ARRAY
        if (p + 4 > len) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          int32_t l;
          memcpy(&l, data + p, 4);
          p += 4;
          if (p + l > len) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            ObString s(l, data + p);
            OZ(ob_write_string(alloc, s, v.str_val_));
            p += l;
            v.type_ = ObVarcharType;
          }
        }
        break;
      }
      default:
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("unsupported parquet physical type", K(ret), K(parquet_type));
        break;
    }
    if (OB_SUCC(ret)) {
      OZ(vals.push_back(v));
    }
  }
  return ret;
}

int ObFileTableUtils::scan_parquet(ObIAllocator &alloc, const ObString &path,
                                   ObIArray<ObFileTableColDef> &columns,
                                   ObIArray<ObNewRow> *rows)
{
  int ret = OB_SUCCESS;
  ObString content;
  OZ(read_local_file(alloc, path, content));
  if (OB_FAIL(ret)) {
  } else if (content.length() < 12 ||
             ObString(4, content.ptr()).compare(ObString("PAR1")) != 0 ||
             ObString(4, content.ptr() + content.length() - 4).compare(ObString("PAR1")) != 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid parquet file", K(ret), K(content.length()));
  } else {
    uint32_t foot_len = 0;
    memcpy(&foot_len, content.ptr() + content.length() - 8, 4);
    if (foot_len <= 0 || foot_len > static_cast<uint32_t>(content.length() - 12)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("bad parquet footer length", K(ret), K(foot_len));
    } else {
      const char *meta = content.ptr() + content.length() - 8 - foot_len;
      ObParquetThriftReader tr(meta, foot_len);
      ObSEArray<ObString, 8> leaf_names;
      ObSEArray<int32_t, 8> leaf_types;
      struct ChunkInfo { int32_t codec; int64_t offset; int64_t num_values; };
      ObSEArray<ChunkInfo, 8> chunks;
      int64_t num_rows = 0;
      int16_t fid = 0;
      int8_t t = 0;
      while (OB_SUCC(ret)) {
        OZ(tr.read_field_begin(fid, t, fid));
        if (t == 0) {
          break;
        } else if (t != 9) {
          if (fid == 3) {
            OZ(tr.read_zigzag(num_rows));
          } else {
            OZ(tr.skip(t));
          }
          continue;
        }
        uint8_t hb = static_cast<uint8_t>(tr.buf_[tr.pos_++]);
        int64_t sz = (hb >> 4) & 0x0f;
        int8_t et = hb & 0x0f;
        if (sz == 15) {
          uint64_t l2 = 0;
          OZ(tr.read_uleb(l2));
          sz = l2;
        }
        if (fid == 2) {
          for (int64_t i = 0; OB_SUCC(ret) && i < sz; ++i) {
            ObString name;
            int32_t ptype = -1;
            int32_t num_children = 0;
            int16_t sfid = 0;
            int8_t st = 0;
            while (OB_SUCC(ret)) {
              OZ(tr.read_field_begin(sfid, st, sfid));
              if (st == 0) {
                break;
              } else if (sfid == 1 && st == 5) {
                int64_t v = 0;
                OZ(tr.read_zigzag(v));
                ptype = static_cast<int32_t>(v);
              } else if (sfid == 4 && st == 8) {
                uint64_t l = 0;
                OZ(tr.read_uleb(l));
                name.assign_ptr(const_cast<char*>(tr.buf_ + tr.pos_), static_cast<int32_t>(l));
                tr.pos_ += l;
              } else if (sfid == 5 && st == 5) {
                int64_t v = 0;
                OZ(tr.read_zigzag(v));
                num_children = static_cast<int32_t>(v);
              } else {
                OZ(tr.skip(st));
              }
            }
            if (OB_SUCC(ret) && num_children == 0 && !name.empty()) {
              OZ(leaf_names.push_back(name));
              OZ(leaf_types.push_back(ptype));
            }
          }
        } else if (fid == 4) {
          for (int64_t i = 0; OB_SUCC(ret) && i < sz; ++i) {
            int16_t rf = 0;
            int8_t rt = 0;
            while (OB_SUCC(ret)) {
              OZ(tr.read_field_begin(rf, rt, rf));
              if (rt == 0) {
                break;
              } else if (rf == 1 && rt == 9) {
                uint8_t hb2 = static_cast<uint8_t>(tr.buf_[tr.pos_++]);
                int64_t sz2 = (hb2 >> 4) & 0x0f;
                int8_t et2 = hb2 & 0x0f;
                if (sz2 == 15) {
                  uint64_t l3 = 0;
                  OZ(tr.read_uleb(l3));
                  sz2 = l3;
                }
                for (int64_t j = 0; OB_SUCC(ret) && j < sz2; ++j) {
                  int16_t cf = 0;
                  int8_t ct = 0;
                  ChunkInfo ci;
                  ci.codec = 0;
                  ci.offset = 0;
                  ci.num_values = 0;
                  while (OB_SUCC(ret)) {
                    OZ(tr.read_field_begin(cf, ct, cf));
                    if (ct == 0) {
                      break;
                    } else if (cf == 3 && ct == 12) {
                      int16_t mf = 0;
                      int8_t mt = 0;
                      while (OB_SUCC(ret)) {
                        OZ(tr.read_field_begin(mf, mt, mf));
                        if (mt == 0) {
                          break;
                        } else if (mf == 4 && mt == 5) {
                          int64_t v = 0;
                          OZ(tr.read_zigzag(v));
                          ci.codec = static_cast<int32_t>(v);
                        } else if (mf == 5 && (mt == 5 || mt == 6)) {
                          OZ(tr.read_zigzag(ci.num_values));
                        } else if (mf == 9 && (mt == 5 || mt == 6)) {
                          OZ(tr.read_zigzag(ci.offset));
                        } else {
                          OZ(tr.skip(mt));
                        }
                      }
                    } else {
                      OZ(tr.skip(ct));
                    }
                  }
                  if (OB_SUCC(ret)) {
                    OZ(chunks.push_back(ci));
                  }
                }
              } else {
                OZ(tr.skip(rt));
              }
            }
          }
        } else {
          OZ(tr.skip(t));
        }
      }
      if (OB_SUCC(ret) && columns.count() == 0) {
        for (int64_t i = 0; OB_SUCC(ret) && i < leaf_names.count(); ++i) {
          ObFileTableColDef col;
          ObString stored;
          OZ(ob_write_string(alloc, leaf_names.at(i), stored));
          col.name_ = stored;
          col.nullable_ = true;
          switch (leaf_types.at(i)) {
            case 0: col.col_type_ = ObTinyIntType; break;
            case 1:
            case 2: col.col_type_ = ObIntType; break;
            case 4:
            case 5: col.col_type_ = ObDoubleType; break;
            case 6: col.col_type_ = ObVarcharType; break;
            default: ret = OB_NOT_SUPPORTED; break;
          }
          if (OB_SUCC(ret)) {
            OZ(columns.push_back(col));
          }
        }
      }
      if (OB_SUCC(ret) && NULL != rows) {
        // decode data pages: chunks are ordered same as leaf columns
        ObSEArray<const ObIArray<ObParquetColValue> *, 8> col_vals;
        for (int64_t c = 0; OB_SUCC(ret) && c < chunks.count() && c < columns.count(); ++c) {
          const ChunkInfo &ci = chunks.at(c);
          ObSEArray<ObParquetColValue, 16> *vals = static_cast<ObSEArray<ObParquetColValue, 16> *>(
              alloc.alloc(sizeof(ObSEArray<ObParquetColValue, 16>)));
          if (OB_ISNULL(vals)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            vals = new (vals) ObSEArray<ObParquetColValue, 16>(&alloc);
            int64_t page_off = ci.offset;
            bool done = false;
            while (OB_SUCC(ret) && !done) {
              ObParquetThriftReader pr(content.ptr() + page_off, content.length() - page_off);
              int16_t pf = 0;
              int8_t pt = 0;
              int32_t page_type = -1;
              int64_t uncomp_size = 0;
              int64_t comp_size = 0;
              int64_t num_values = 0;
              int64_t num_nulls = 0;
              int64_t def_len = 0;
              int64_t rep_len = 0;
              bool is_compressed = true;
              while (OB_SUCC(ret)) {
                OZ(pr.read_field_begin(pf, pt, pf));
                if (pt == 0) {
                  break;
                } else if (pf == 1 && (pt == 5 || pt == 6)) {
                  int64_t v = 0;
                  OZ(pr.read_zigzag(v));
                  page_type = static_cast<int32_t>(v);
                } else if (pf == 2 && (pt == 5 || pt == 6)) {
                  OZ(pr.read_zigzag(uncomp_size));
                } else if (pf == 3 && (pt == 5 || pt == 6)) {
                  OZ(pr.read_zigzag(comp_size));
                } else if (pf == 8 && pt == 12) {
                  int16_t vf = 0;
                  int8_t vt = 0;
                  while (OB_SUCC(ret)) {
                    OZ(pr.read_field_begin(vf, vt, vf));
                    if (vt == 0) {
                      break;
                    } else if (vf == 1 && (vt == 5 || vt == 6)) {
                      OZ(pr.read_zigzag(num_values));
                    } else if (vf == 2 && (vt == 5 || vt == 6)) {
                      OZ(pr.read_zigzag(num_nulls));
                    } else if (vf == 5 && (vt == 5 || vt == 6)) {
                      OZ(pr.read_zigzag(def_len));
                    } else if (vf == 6 && (vt == 5 || vt == 6)) {
                      OZ(pr.read_zigzag(rep_len));
                    } else if (vf == 7 && (vt == 1 || vt == 2)) {
                      is_compressed = (vt == 1);
                    } else {
                      OZ(pr.skip(vt));
                    }
                  }
                } else {
                  OZ(pr.skip(pt));
                }
              }
              if (OB_FAIL(ret)) {
              } else if (page_type != 3 && page_type != 0) {
                page_off += comp_size;
                continue;
              } else {
                int64_t body_off = page_off + pr.pos_;
                int64_t data_start = body_off;
                if (page_type == 3) {
                  data_start = body_off + rep_len + def_len;
                }
                int64_t data_len = (page_type == 3) ? (comp_size - (data_start - body_off)) : uncomp_size;
                const char *data = content.ptr() + data_start;
                char *uncomp = NULL;
                int64_t real_data_len = data_len;
                if (page_type == 3 && num_nulls > 0) {
                  ret = OB_NOT_SUPPORTED;
                  LOG_WARN("null values in parquet not supported yet", K(ret));
                } else if (page_type == 0 && num_nulls > 0) {
                  ret = OB_NOT_SUPPORTED;
                  LOG_WARN("null values in parquet not supported yet", K(ret));
                }
                if (OB_SUCC(ret) && is_compressed) {
                  if (ci.codec == 1) {
                    int64_t ulen = uncomp_size - (page_type == 3 ? (rep_len + def_len) : 0);
                    if (OB_ISNULL(uncomp = static_cast<char *>(alloc.alloc(ulen + 1)))) {
                      ret = OB_ALLOCATE_MEMORY_FAILED;
                    } else {
                      OZ(snappy_uncompress(data, data_len, uncomp, ulen));
                      data = uncomp;
                      real_data_len = ulen;
                    }
                  } else if (ci.codec != 0) {
                    ret = OB_NOT_SUPPORTED;
                    LOG_WARN("unsupported parquet codec", K(ret), K(ci.codec));
                  }
                }
                if (OB_SUCC(ret)) {
                  int32_t ptype = (c < leaf_types.count()) ? leaf_types.at(c) : 1;
                  OZ(parquet_plain_decode(alloc, data, real_data_len, ptype,
                                          num_values > 0 ? num_values : ci.num_values, *vals));
                }
                done = true;
              }
            }
            if (OB_SUCC(ret)) {
              OZ(col_vals.push_back(vals));
            }
          }
        }
        if (OB_SUCC(ret)) {
          int64_t nrows = num_rows;
          for (int64_t r = 0; OB_SUCC(ret) && r < nrows; ++r) {
            ObObj *cells = static_cast<ObObj *>(alloc.alloc(sizeof(ObObj) * columns.count()));
            if (OB_ISNULL(cells)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else {
              for (int64_t c = 0; OB_SUCC(ret) && c < columns.count() && c < col_vals.count(); ++c) {
                new (cells + c) ObObj();
                const ObIArray<ObParquetColValue> &vals = *col_vals.at(c);
                if (r >= vals.count()) {
                  cells[c].set_null();
                } else {
                  const ObParquetColValue &v = vals.at(r);
                  switch (v.type_) {
                    case ObIntType: cells[c].set_int(v.int_val_); break;
                    case ObDoubleType: cells[c].set_double(v.dbl_val_); break;
                    case ObTinyIntType: cells[c].set_tinyint(v.bool_val_ ? 1 : 0); break;
                    default: {
                      cells[c].set_varchar(v.str_val_);
                      cells[c].set_collation_type(CS_TYPE_UTF8MB4_BIN);
                      cells[c].set_collation_level(CS_LEVEL_IMPLICIT);
                      break;
                    }
                  }
                }
              }
              if (OB_SUCC(ret)) {
                ObNewRow row;
                OZ(make_new_row(alloc, cells, static_cast<int64_t>(columns.count()), row));
                OZ(rows->push_back(row));
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
