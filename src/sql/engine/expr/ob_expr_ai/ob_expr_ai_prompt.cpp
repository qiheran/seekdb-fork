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
#include "ob_expr_ai_prompt.h"
#include <regex>
#include "ob_ai_func.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
#include "sql/engine/expr/ob_expr_json_func_helper.h"


using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

ObExprAIPrompt::ObExprAIPrompt(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_PROMPT, 
                    N_AI_PROMPT, 
                    MORE_THAN_ZERO,
                    VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIPrompt::~ObExprAIPrompt() 
{
}

int ObExprAIPrompt::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const 
{
  int ret = OB_SUCCESS;
  ObExprResType template_type = types_stack[0];
  // Templates and arguments accept TEXT/LONGTEXT (ObTextTC) in addition to
  // ordinary string types; the evaluation reads LOB content through the
  // standard JSON helper.
  if (!ob_is_string_tc(template_type.get_type()) && !ob_is_text_tc(template_type.get_type())) {
    ret = OB_ERR_INVALID_TYPE_FOR_OP;
  } else {
    types_stack[0].set_calc_type(ObVarcharType);
    types_stack[0].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
  }

  for (int64_t i = 1; i < param_num && OB_SUCC(ret); i++) {
    if (ob_is_string_tc(types_stack[i].get_type()) || ob_is_text_tc(types_stack[i].get_type())) {
      types_stack[i].set_calc_type(ObVarcharType);
      types_stack[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    } else if (ob_is_json(types_stack[i].get_type())
               || types_stack[i].get_type() == ObUnknownType) {
      // JSON arguments (objects/arrays, including nested prompt results) keep
      // their JSON calc type; evaluation parses the binary payload into a tree
      // node so the args array preserves the object shape. NULL literals are
      // carried as JSON null.
      types_stack[i].set_calc_type(ObJsonType);
      types_stack[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    } else {
      ret = OB_ERR_INVALID_TYPE_FOR_OP;
    }
  }

  if (OB_SUCC(ret)) {
    type.set_json();
    type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
  }
  return ret;
}

int ObExprAIPrompt::eval_ai_prompt(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor tmp_allocator(tmp_alloc_g.get_allocator());
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_PROMPT));

  ObString template_str;
  ObJsonArray *args_array = NULL;

  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(tmp_allocator, args_array))) {
  } else if (expr.arg_cnt_ < 1) {
    ret = OB_INVALID_ARGUMENT;
  }
  // The template (argument 0) is evaluated through the shared JSON helper so
  // that TEXT/LONGTEXT templates are read correctly.
  ObIJsonBase *template_jbase = NULL;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObJsonExprHelper::get_json_val(expr, ctx, &tmp_allocator, 0, template_jbase))) {
  } else if (OB_ISNULL(template_jbase) || template_jbase->json_type() != ObJsonNodeType::J_STRING) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt template must be a string");
  } else {
    template_str = static_cast<ObJsonString *>(template_jbase)->get_str();
  }
  // Every argument (1..N) is evaluated through the shared JSON helper so that
  // string, TEXT/LONGTEXT and JSON (nested prompt) inputs are all carried as
  // JSON nodes.
  for (int64_t i = 1; OB_SUCC(ret) && i < expr.arg_cnt_; i++) {
    ObExpr *arg = expr.args_[i];
    ObDatum *datum = NULL;
    ObJsonNode *arg_node = NULL;
    if (OB_FAIL(tmp_allocator.eval_arg(arg, ctx, datum))) {
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: failed to evaluate argument");
    } else if (datum->is_null()) {
      // NULL arguments keep their position as JSON null in the args array.
      void *null_buf = NULL;
      if (OB_ISNULL(null_buf = tmp_allocator.alloc(sizeof(ObJsonNull)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        arg_node = new (null_buf) ObJsonNull();
      }
    } else if (ob_is_json(arg->datum_meta_.type_)) {
      // JSON-typed argument: the datum carries binary JSON (behind a possible
      // LOB header); strip the header and parse it into a tree node so the
      // args array preserves the object shape.
      ObString raw = datum->get_string();
      if (OB_FAIL(ObTextStringHelper::read_real_string_data(
              ctx.exec_ctx_, tmp_allocator, *datum, arg->datum_meta_,
              arg->obj_meta_.has_lob_header(), raw))) {
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: failed to read json argument");
      } else {
        ObIJsonBase *tree_base = NULL;
        if (OB_FAIL(ObJsonBaseFactory::get_json_base(&tmp_allocator, raw,
                ObJsonInType::JSON_BIN, ObJsonInType::JSON_TREE, tree_base,
                ObJsonParser::JSN_STRICT_FLAG,
                ObJsonExprHelper::get_json_max_depth_config()))) {
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_prompt: failed to parse json argument");
        } else if (OB_ISNULL(tree_base)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          arg_node = static_cast<ObJsonNode *>(tree_base);
        }
      }
    } else {
      ObString arg_str = datum->get_string();
      ObJsonString *str_node = NULL;
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(tmp_allocator, arg_str, str_node))) {
      } else {
        arg_node = str_node;
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_ISNULL(arg_node)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(args_array->append(arg_node))) {
      }
    }
  }

  // A template-only prompt (no extra arguments) is valid: args stays [].

  ObJsonObject *prompt_object = NULL;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObAIFuncPromptObjectUtils::construct_prompt_object(tmp_allocator, template_str, args_array, prompt_object))) {
  } else {
    ObString raw_str;
    if (OB_FAIL(prompt_object->get_raw_binary(raw_str, &tmp_allocator))) {
    } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
    }
  }
  return ret;
}



int ObExprAIPrompt::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  int ret = OB_SUCCESS;
  rt_expr.eval_func_ = ObExprAIPrompt::eval_ai_prompt;
  return ret;
}

} // namespace sql
} // namespace oceanbase
// touch
