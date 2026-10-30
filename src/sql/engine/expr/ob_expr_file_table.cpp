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

#include "sql/engine/expr/ob_expr_file_table.h"
#include "sql/engine/ob_exec_context.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER(ObFileTableColDef, name_, col_type_, precision_, scale_, collation_type_, nullable_);
OB_SERIALIZE_MEMBER(ObFileTableExtraInfo, func_kind_, path_, header_, sample_size_, format_, columns_);

int ObFileTableExtraInfo::deep_copy(common::ObIAllocator &allocator,
                                    const ObExprOperatorType type,
                                    ObIExprExtraInfo *&copied_info) const
{
  int ret = OB_SUCCESS;
  ObFileTableExtraInfo *other = NULL;
  if (OB_FAIL(ObExprExtraInfoFactory::alloc(allocator, type, copied_info))) {
  } else if (OB_ISNULL(other = static_cast<ObFileTableExtraInfo *>(copied_info))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    other->func_kind_ = func_kind_;
    other->header_ = header_;
    other->sample_size_ = sample_size_;
    OZ(ob_write_string(allocator, path_, other->path_));
    OZ(ob_write_string(allocator, format_, other->format_));
    OZ(other->columns_.assign(columns_));
  }
  return ret;
}

ObExprFileTable::ObExprFileTable(ObIAllocator &alloc, ObExprOperatorType type, const char *name)
    : ObFuncExprOperator(alloc, type, name, 4, NOT_VALID_FOR_GENERATED_COL, NOT_ROW_DIMENSION)
{
  disable_operand_auto_cast();
}

OB_SERIALIZE_MEMBER_INHERIT(ObExprFileTable, ObExprOperator);

int ObExprFileTable::eval_file_table(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  UNUSED(expr);
  UNUSED(ctx);
  UNUSED(res);
  int ret = OB_ERR_UNEXPECTED;
  LOG_WARN("file table marker expr should never be evaluated", K(ret));
  return ret;
}

int ObExprFileTable::calc_result_type(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t param_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  UNUSED(types);
  UNUSED(param_num);
  UNUSED(type_ctx);
  type.set_type(ObLongTextType);
  type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  type.set_collation_level(CS_LEVEL_IMPLICIT);
  type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObLongTextType]);
  return OB_SUCCESS;
}

int ObExprFileTable::cg_expr(ObExprCGCtx &expr_cg_ctx, const ObRawExpr &raw_expr, ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  rt_expr.eval_func_ = eval_file_table;
  rt_expr.eval_batch_func_ = eval_file_table;
  rt_expr.eval_vector_func_ = NULL;
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
