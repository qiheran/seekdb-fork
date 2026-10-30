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

#ifndef OB_FILE_TABLE_UTILS_H_
#define OB_FILE_TABLE_UTILS_H_

#include "lib/ob_define.h"
#include "lib/container/ob_array.h"
#include "common/ob_obj_type.h"
#include "common/object/ob_obj_cast.h"
#include "sql/engine/expr/ob_expr_file_table.h"
#include "common/row/ob_row.h"

namespace oceanbase
{
namespace sql
{

class ObFileTableUtils
{
public:
  // read whole file into memory
  static int read_local_file(common::ObIAllocator &alloc, const common::ObString &path,
                             common::ObString &content);
  // infer column schema from file content
  static int infer_schema(common::ObIAllocator &alloc, ObFileTableFuncKind kind,
                          const common::ObString &path, bool has_header,
                          const common::ObString &format,
                          common::ObIArray<ObFileTableColDef> &columns);
  // scan all rows as ObNewRow (cells allocated from allocator)
  static int scan_rows(common::ObIAllocator &alloc, ObFileTableFuncKind kind,
                       const common::ObString &path, bool has_header,
                       const common::ObString &format,
                       const common::ObIArray<ObFileTableColDef> &columns,
                       common::ObIArray<common::ObNewRow> &rows);
  // expand glob pattern, produce sorted list of matched file paths
  static int list_glob(common::ObIAllocator &alloc, const common::ObString &pattern,
                       common::ObIArray<common::ObString> &files);
private:
  static int parse_csv_records(common::ObIAllocator &alloc, const common::ObString &content,
                               common::ObIArray<common::ObIArray<common::ObString> > &records);
  static int infer_from_values(common::ObIAllocator &alloc,
                               const common::ObIArray<common::ObIArray<common::ObString> > &records,
                               int64_t first_data_row,
                               common::ObIArray<ObFileTableColDef> &columns);
  static int convert_value(common::ObIAllocator &alloc, const common::ObString &val,
                           const ObFileTableColDef &col, common::ObObj &obj);
  static int scan_parquet(common::ObIAllocator &alloc, const common::ObString &path,
                          common::ObIArray<ObFileTableColDef> &columns,
                          common::ObIArray<common::ObNewRow> *rows);
  static int snappy_uncompress(const char *src, int64_t src_len, char *dst, int64_t dst_len);
};

} // end namespace sql
} // end namespace oceanbase

#endif /* OB_FILE_TABLE_UTILS_H_ */
