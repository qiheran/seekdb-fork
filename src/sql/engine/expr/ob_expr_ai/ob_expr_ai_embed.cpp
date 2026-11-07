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
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
    res.set_null();
  } else if (arg_content->is_null()) {
    // NULL content rows produce NULL results instead of an error.
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content = arg_content->get_string();
    if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      dim = arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_batch_ai_embed(const ObExpr &expr, ObEvalCtx &ctx,
                                       const ObBitVector &skip, const int64_t batch_size)
{
  INIT_SUCC(ret);
  ObDatumVector results = expr.locate_expr_datumvector(ctx);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  ObDatumVector model_v;
  ObDatumVector content_v;
  ObDatumVector dim_v;
  if (expr.arg_cnt_ == 3
      ? OB_FAIL(expr.eval_batch_param_value(ctx, skip, batch_size, model_v, content_v, dim_v))
      : OB_FAIL(expr.eval_batch_param_value(ctx, skip, batch_size, model_v, content_v))) {
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));

    // First pass: classify rows. NULL content rows resolve to NULL without
    // entering the request; the other rows are merged into one batch request.
    bool has_model = false;
    ObString model_id;
    int64_t dim = 0;
    bool has_dim = false;
    ObArray<int64_t> row_indices;
    ObArray<ObString> contents;
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; i++) {
      if (skip.at(i) || eval_flags.at(i)) {
        continue;
      }
      ObDatum *model_datum = model_v.at(i);
      ObDatum *content_datum = content_v.at(i);
      if (OB_ISNULL(model_datum) || OB_ISNULL(content_datum)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (model_datum->is_null()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
        results.datums_[i].set_null();
      } else if (content_datum->is_null()) {
        results.datums_[i].set_null();
      } else {
        ObString row_model = model_datum->get_string();
        if (!has_model) {
          model_id = row_model;
          has_model = true;
        } else if (row_model != model_id) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id must be the same in one batch");
        } else {
          // model id matches, keep collecting
        }
        if (OB_SUCC(ret)) {
          ObString row_content = content_datum->get_string();
          if (row_content.empty()) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, input is empty");
            results.datums_[i].set_null();
          } else if (OB_FAIL(row_indices.push_back(i))) {
          } else if (OB_FAIL(contents.push_back(row_content))) {
          } else {
          }
        }
        if (OB_SUCC(ret) && expr.arg_cnt_ == 3) {
          ObDatum *dim_datum = dim_v.at(i);
          if (OB_NOT_NULL(dim_datum) && !dim_datum->is_null()) {
            int64_t row_dim = dim_datum->get_int();
            if (!has_dim) {
              dim = row_dim;
              has_dim = true;
            } else if (row_dim != dim) {
              ret = OB_INVALID_ARGUMENT;
              LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be the same in one batch");
            } else {
            }
          }
        }
      }
    }

    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (!has_model || contents.empty()) {
      // nothing to request in this batch
    } else if (has_dim && dim > 0) {
      ObJsonInt *dim_json = nullptr;
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    } else if (has_dim && dim <= 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
    }

    if (OB_FAIL(ret) || !has_model || contents.empty()) {
      // nothing more to do
    } else {
      share::ObAiModelEndpointInfo resolved_endpoint;
      const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
      ObAIFuncExprInfo *info = nullptr;
      query::ObIAiEndpointResolver *endpoint_resolver =
          ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
      if (OB_ISNULL(endpoint_resolver)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
      } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                     model_id, temp_allocator, resolved_endpoint))) {
      } else {
        // All rows of the batch are merged into a single HTTP request; the
        // results are returned in the same order as the input contents.
        ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
        ObArray<ObString> embed_results;
        if (OB_FAIL(model.call_dense_embedding_vector_v2(contents, config, embed_results))) {
        } else if (embed_results.count() != contents.count()) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("ai_embed, result count is not equal to input count", K(ret));
        } else {
          for (int64_t j = 0; OB_SUCC(ret) && j < embed_results.count(); j++) {
            int64_t row_idx = row_indices.at(j);
            const ObString &vec_str = embed_results.at(j);
            char *res_buf = expr.get_str_res_mem(ctx, vec_str.length(), row_idx);
            if (OB_ISNULL(res_buf)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else {
              MEMCPY(res_buf, vec_str.ptr(), vec_str.length());
              results.datums_[row_idx].set_string(res_buf, vec_str.length());
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_batch_ai_embed;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
