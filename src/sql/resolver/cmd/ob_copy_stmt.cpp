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

#define USING_LOG_PREFIX SQL_RESV

#include "sql/resolver/cmd/ob_copy_stmt.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/session/ob_basic_session_info.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

int ObCopyResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObCopyStmt *copy_stmt = NULL;
  CK (OB_LIKELY(T_COPY == parse_tree.type_ && 3 == parse_tree.num_child_));
  CK (OB_NOT_NULL(parse_tree.children_[0]));
  CK (OB_NOT_NULL(parse_tree.children_[1]));
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(allocator_) || OB_ISNULL(session_info_)) {
    ret = OB_NOT_INIT;
  } else if (NULL == (copy_stmt = create_stmt<ObCopyStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create copy stmt", K(ret));
  }
  // enable_file_sql check
  int64_t enable_file_sql = 0;
  if (OB_SUCC(ret)) {
    if (OB_FAIL(session_info_->get_sys_variable_by_name(ObString("enable_file_sql"),
                                                        enable_file_sql))) {
      LOG_WARN("get enable_file_sql failed", K(ret));
    } else if (0 == enable_file_sql) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy statement, set enable_file_sql to enable");
    }
  }
  // resolve inner select
  ObStmt *child_stmt = NULL;
  if (OB_SUCC(ret)) {
    ObResolver resolver(params_);
    if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                 *parse_tree.children_[0],
                                 child_stmt))) {
      LOG_WARN("resolve copy subquery failed", K(ret));
    } else if (OB_ISNULL(child_stmt) || !child_stmt->is_select_stmt()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy source must be a select statement");
    } else {
      copy_stmt->set_select_stmt(static_cast<ObSelectStmt *>(child_stmt));
    }
  }
  // path
  if (OB_SUCC(ret)) {
    const ParseNode &path_node = *parse_tree.children_[1];
    if (T_STRING != path_node.type_) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "copy target must be a constant string path");
    } else {
      ObString path;
      OZ (ob_write_string(*allocator_, ObString(path_node.str_len_, path_node.str_value_), path));
      copy_stmt->set_path(path);
    }
  }
  // options
  if (OB_SUCC(ret)) {
    const ParseNode *opts = parse_tree.children_[2];
    bool header_given = false;
    if (NULL != opts) {
      for (int64_t i = 0; OB_SUCC(ret) && i < opts->num_child_; ++i) {
        const ParseNode *opt = opts->children_[i];
        if (OB_ISNULL(opt) || T_COPY_OPTION != opt->type_ || opt->num_child_ != 2 ||
            OB_ISNULL(opt->children_[0])) {
          ret = OB_ERR_PARSER_SYNTAX;
          LOG_WARN("invalid copy option", K(ret));
        } else {
          ObString name(opt->children_[0]->str_len_, opt->children_[0]->str_value_);
          const ParseNode *val = opt->children_[1];
          if (name.case_compare("format") == 0) {
            if (NULL != val) {
              ObString fmt;
              if (T_STRING == val->type_) {
                fmt.assign_ptr(val->str_value_, val->str_len_);
              } else if (T_IDENT == val->type_ || NAME_OB == val->type_) {
                fmt.assign_ptr(val->str_value_, val->str_len_);
              } else {
                ret = OB_INVALID_ARGUMENT;
                LOG_USER_ERROR(OB_INVALID_ARGUMENT, "format expects a string value");
              }
              if (OB_SUCC(ret) && fmt.case_compare("csv") != 0) {
                ret = OB_NOT_SUPPORTED;
                LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy format other than csv");
              }
            }
          } else if (name.case_compare("header") == 0) {
            header_given = true;
            if (NULL != val) {
              if (T_INT == val->type_) {
                copy_stmt->set_header(val->value_ != 0);
              } else {
                ret = OB_INVALID_ARGUMENT;
                LOG_USER_ERROR(OB_INVALID_ARGUMENT, "header expects a boolean value");
              }
            } else {
              copy_stmt->set_header(true);
            }
          } else if (name.case_compare("nullstr") == 0) {
            if (NULL != val && T_STRING == val->type_) {
              ObString ns;
              OZ (ob_write_string(*allocator_, ObString(val->str_len_, val->str_value_), ns));
              copy_stmt->set_nullstr(ns);
            } else {
              ret = OB_INVALID_ARGUMENT;
              LOG_USER_ERROR(OB_INVALID_ARGUMENT, "nullstr expects a string value");
            }
          } else {
            ret = OB_ERR_PARSER_SYNTAX;
            LOG_USER_ERROR(OB_ERR_PARSER_SYNTAX, name.length(), name.ptr());
          }
        }
      }
    }
    UNUSED(header_given);
  }
  if (OB_SUCC(ret)) {
    stmt_ = copy_stmt;
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
