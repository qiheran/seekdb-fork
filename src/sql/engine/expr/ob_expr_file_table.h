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

#ifndef OB_EXPR_FILE_TABLE_H_
#define OB_EXPR_FILE_TABLE_H_

#include "sql/engine/expr/ob_expr_operator.h"
#include "sql/engine/expr/ob_i_expr_extra_info.h"
#include "lib/json/ob_json_parse.h"

namespace oceanbase
{
namespace sql
{

enum ObFileTableFuncKind
{
  FILE_FUNC_READ_CSV = 0,
  FILE_FUNC_READ_JSON = 1,
  FILE_FUNC_READ_PARQUET = 2,
  FILE_FUNC_GLOB = 3,
  FILE_FUNC_MAX = 4,
};

struct ObFileTableColDef
{
  OB_UNIS_VERSION(1);
public:
  ObFileTableColDef() : col_type_(common::ObVarcharType), precision_(-1), scale_(-1),
      collation_type_(common::CS_TYPE_UTF8MB4_BIN), nullable_(true) {}
  bool is_valid() const { return !name_.empty() && col_type_ > common::ObMinType && col_type_ < common::ObMaxType; }
  TO_STRING_KV(K_(name), K_(col_type), K_(precision), K_(scale), K_(collation_type), K_(nullable));
  common::ObString name_;
  common::ObObjType col_type_;
  int64_t precision_;
  int64_t scale_;
  common::ObCollationType collation_type_;
  bool nullable_;
};

struct ObFileTableExtraInfo : public ObIExprExtraInfo
{
  OB_UNIS_VERSION(1);
public:
  ObFileTableExtraInfo(common::ObIAllocator &alloc, ObExprOperatorType type)
      : ObIExprExtraInfo(alloc, type), func_kind_(FILE_FUNC_READ_CSV),
        path_(), header_(false), sample_size_(-1), format_()
  {}
  virtual ~ObFileTableExtraInfo() {}
  void reset()
  {
    path_.reset();
    format_.reset();
    columns_.reset();
    header_ = false;
    sample_size_ = -1;
    func_kind_ = FILE_FUNC_READ_CSV;
  }
  virtual int deep_copy(common::ObIAllocator &allocator,
                        const ObExprOperatorType type,
                        ObIExprExtraInfo *&copied_info) const override;
  ObFileTableFuncKind func_kind_;
  common::ObString path_;
  bool header_;
  int64_t sample_size_;
  common::ObString format_;
  common::ObArray<ObFileTableColDef> columns_;
};

// marker operator for file table functions (read_csv/read_json/read_parquet/glob)
// used in FROM clause as a FUNCTION_TABLE item. rows are produced by
// ObFunctionTableOp file scan path, this operator is never evaluated.
class ObExprFileTable : public ObExprOperator
{
  OB_UNIS_VERSION(1);
public:
  ObExprFileTable(common::ObIAllocator &alloc, ObExprOperatorType type, const char *name);
  virtual ~ObExprFileTable() {}
  static int eval_file_table(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res);
  virtual int calc_result_type(ObExprResType &type,
                               ObExprResType *types,
                               int64_t param_num,
                               common::ObExprTypeCtx &type_ctx) const override;
  virtual int cg_expr(ObExprCGCtx &expr_cg_ctx, const ObRawExpr &raw_expr,
                      ObExpr &rt_expr) const override;
protected:
  DISALLOW_COPY_AND_ASSIGN(ObExprFileTable);
};

#define DEF_FILE_TABLE_OP(CLASS, TYPE, NAME_STR)                            \
  class CLASS : public ObExprFileTable                                      \
  {                                                                         \
  public:                                                                   \
    explicit CLASS(common::ObIAllocator &alloc)                             \
        : ObExprFileTable(alloc, TYPE, NAME_STR) {}                         \
    virtual ~CLASS() {}                                                     \
  };

DEF_FILE_TABLE_OP(ObExprReadCsv, T_READ_CSV_EXPR, "read_csv")
DEF_FILE_TABLE_OP(ObExprReadJson, T_READ_JSON_EXPR, "read_json")
DEF_FILE_TABLE_OP(ObExprReadParquet, T_READ_PARQUET_EXPR, "read_parquet")
DEF_FILE_TABLE_OP(ObExprGlob, T_GLOB_EXPR, "glob")

} // end namespace sql
} // end namespace oceanbase

#endif /* OB_EXPR_FILE_TABLE_H_ */
