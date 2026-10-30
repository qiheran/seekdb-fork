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
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/engine/basic/ob_file_table_utils.h"
#include "sql/resolver/ob_resolver.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;
int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else {
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (NULL != format) {
          switch (format->type_) {
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  return ret;
}

static const ParseNode *find_file_table_node(const ParseNode *node)
{
  const ParseNode *found = NULL;
  if (NULL == node) {
  } else if (T_READ_CSV_EXPR == node->type_ || T_READ_JSON_EXPR == node->type_ ||
             T_READ_PARQUET_EXPR == node->type_ || T_GLOB_EXPR == node->type_) {
    found = node;
  } else {
    for (int64_t i = 0; NULL == found && i < node->num_child_; ++i) {
      found = find_file_table_node(node->children_[i]);
    }
  }
  return found;
}

int ObDescribeFileTableResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObSelectStmt *select_stmt = NULL;
  CK (OB_LIKELY(T_DESCRIBE_FILE_TABLE == parse_tree.type_ && 1 == parse_tree.num_child_));
  CK (OB_NOT_NULL(parse_tree.children_[0]));
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(allocator_) || OB_ISNULL(session_info_) || OB_ISNULL(params_.expr_factory_)) {
    ret = OB_NOT_INIT;
  }
  // check enable_file_sql
  int64_t enable_file_sql = 0;
  if (OB_SUCC(ret)) {
    if (OB_FAIL(session_info_->get_sys_variable_by_name(ObString("enable_file_sql"),
                                                        enable_file_sql))) {
      LOG_WARN("get enable_file_sql failed", K(ret));
    } else if (0 == enable_file_sql) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "file sql functions, set enable_file_sql to enable");
    }
  }
  const ParseNode *ft_node = NULL;
  if (OB_SUCC(ret)) {
    ft_node = find_file_table_node(parse_tree.children_[0]);
    if (OB_ISNULL(ft_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("no file table function in describe target", K(ret));
    }
  }
  ObFileTableFuncKind kind = FILE_FUNC_READ_CSV;
  ObString path;
  bool has_header = false;
  int64_t sample_size = -1;
  ObString format;
  if (OB_SUCC(ret)) {
    switch (ft_node->type_) {
      case T_READ_CSV_EXPR: kind = FILE_FUNC_READ_CSV; break;
      case T_READ_JSON_EXPR: kind = FILE_FUNC_READ_JSON; break;
      case T_READ_PARQUET_EXPR: kind = FILE_FUNC_READ_PARQUET; break;
      default: kind = FILE_FUNC_GLOB; break;
    }
    // path
    const ParseNode *path_node = ft_node->children_[0];
    if (OB_ISNULL(path_node) || path_node->type_ != T_STRING) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file path must be a constant string");
    } else {
      path.assign_ptr(path_node->str_value_, path_node->str_len_);
    }
    // named args
    if (OB_SUCC(ret) && NULL != ft_node->children_[1]) {
      const ParseNode &arg_list = *ft_node->children_[1];
      for (int64_t i = 0; OB_SUCC(ret) && i < arg_list.num_child_; ++i) {
        const ParseNode *arg_node = arg_list.children_[i];
        if (OB_ISNULL(arg_node) || T_FILE_TABLE_ARG != arg_node->type_ || arg_node->num_child_ != 2 ||
            OB_ISNULL(arg_node->children_[0]) || OB_ISNULL(arg_node->children_[1])) {
          ret = OB_ERR_PARSER_SYNTAX;
        } else {
          ObString arg_name(arg_node->children_[0]->str_len_, arg_node->children_[0]->str_value_);
          const ParseNode *val_node = arg_node->children_[1];
          if (arg_name.case_compare("header") == 0) {
            if (T_BOOL_VALUE == val_node->type_ && NULL != val_node->children_[0]) {
              has_header = val_node->children_[0]->value_ != 0;
            } else if (T_INT == val_node->type_) {
              has_header = val_node->value_ != 0;
            } else {
              ret = OB_INVALID_ARGUMENT;
              LOG_USER_ERROR(OB_INVALID_ARGUMENT, "header expects a boolean value");
            }
          } else if (arg_name.case_compare("sample_size") == 0) {
            if (T_INT == val_node->type_) {
              sample_size = val_node->value_;
            } else {
              ret = OB_INVALID_ARGUMENT;
              LOG_USER_ERROR(OB_INVALID_ARGUMENT, "sample_size expects an integer value");
            }
            if (OB_SUCC(ret) && sample_size != -1) {
              ret = OB_NOT_SUPPORTED;
              LOG_USER_ERROR(OB_NOT_SUPPORTED, "sample_size other than -1");
            }
          } else if (arg_name.case_compare("format") == 0) {
            if (T_STRING != val_node->type_) {
              ret = OB_INVALID_ARGUMENT;
              LOG_USER_ERROR(OB_INVALID_ARGUMENT, "format expects a string value");
            } else {
              format.assign_ptr(val_node->str_value_, val_node->str_len_);
            }
          } else {
            ret = OB_ERR_PARSER_SYNTAX;
            LOG_USER_ERROR(OB_ERR_PARSER_SYNTAX, arg_name.length(), arg_name.ptr());
          }
        }
      }
    }
  }
  ObSEArray<ObFileTableColDef, 8> columns;
  if (OB_SUCC(ret)) {
    OZ (ObFileTableUtils::infer_schema(*allocator_, kind, path, has_header, format, columns));
  }
  // build values table select with schema rows
  if (OB_SUCC(ret)) {
    if (NULL == (select_stmt = create_stmt<ObSelectStmt>())) {
      ret = OB_SQL_RESOLVER_NO_MEMORY;
    } else {
      void *table_buf = allocator_->alloc(sizeof(ObValuesTableDef));
      if (OB_ISNULL(table_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        ObValuesTableDef *table_def = new (table_buf) ObValuesTableDef();
        table_def->row_cnt_ = columns.count();
        table_def->column_cnt_ = 6;
        table_def->access_type_ = ObValuesTableDef::ACCESS_EXPR;
        table_def->is_const_ = true;
        static const char *col_names[] = {"column_name", "column_type", "null", "key", "default", "extra"};
        for (int64_t r = 0; OB_SUCC(ret) && r < columns.count(); ++r) {
          const ObFileTableColDef &col = columns.at(r);
          for (int64_t c = 0; OB_SUCC(ret) && c < 6; ++c) {
            char val_buf[OB_MAX_COLUMN_NAME_LENGTH + 16];
            int64_t pos = 0;
            if (c == 0) {
              pos = col.name_.length();
              memcpy(val_buf, col.name_.ptr(), col.name_.length());
              val_buf[pos] = '\0';
            } else if (c == 1) {
              switch (col.col_type_) {
                case ObIntType: strncpy(val_buf, "BIGINT", sizeof(val_buf) - 1); val_buf[sizeof(val_buf) - 1] = '\0'; break;
                case ObDoubleType: strncpy(val_buf, "DOUBLE", sizeof(val_buf) - 1); val_buf[sizeof(val_buf) - 1] = '\0'; break;
                case ObTinyIntType: strncpy(val_buf, "BOOLEAN", sizeof(val_buf) - 1); val_buf[sizeof(val_buf) - 1] = '\0'; break;
                case ObDateType: strncpy(val_buf, "DATE", sizeof(val_buf) - 1); val_buf[sizeof(val_buf) - 1] = '\0'; break;
                case ObDateTimeType: strncpy(val_buf, "DATETIME", sizeof(val_buf) - 1); val_buf[sizeof(val_buf) - 1] = '\0'; break;
                default: strncpy(val_buf, "VARCHAR", sizeof(val_buf) - 1); val_buf[sizeof(val_buf) - 1] = '\0'; break;
              }
              pos = strlen(val_buf);
            } else if (c == 2) {
              strncpy(val_buf, col.nullable_ ? "YES" : "NO", sizeof(val_buf) - 1);
              val_buf[sizeof(val_buf) - 1] = '\0';
              pos = strlen(val_buf);
            } else {
              val_buf[0] = '\0';
              pos = 0;
            }
            // store and build const expr
            char *stored = static_cast<char *>(allocator_->alloc(pos + 1));
            if (OB_ISNULL(stored)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else {
              memcpy(stored, val_buf, pos);
              stored[pos] = '\0';
              ObConstRawExpr *const_expr = NULL;
              ObRawExprResType rt;
              ObObj obj;
              if (OB_FAIL(params_.expr_factory_->create_raw_expr(T_STRING, const_expr))) {
              } else if (OB_ISNULL(const_expr)) {
                ret = OB_ERR_UNEXPECTED;
              } else {
                obj.set_varchar(ObString(static_cast<int32_t>(pos), stored));
                obj.set_collation_type(CS_TYPE_UTF8MB4_BIN);
                obj.set_collation_level(CS_LEVEL_IMPLICIT);
                const_expr->set_value(obj);
                rt.set_meta(obj.get_meta());
                rt.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObVarcharType]);
                const_expr->set_result_type(rt);
                OZ (table_def->access_exprs_.push_back(const_expr));
              }
            }
          }
        }
        if (OB_SUCC(ret)) {
          for (int64_t c = 0; OB_SUCC(ret) && c < 6; ++c) {
            ObRawExprResType rt;
            rt.set_type(ObVarcharType);
            rt.set_collation_type(CS_TYPE_UTF8MB4_BIN);
            rt.set_collation_level(CS_LEVEL_IMPLICIT);
            rt.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObVarcharType]);
            OZ (table_def->column_types_.push_back(rt));
          }
        }
        if (OB_SUCC(ret)) {
          OZ (ObResolverUtils::create_values_table_query(session_info_, allocator_,
                                                         params_.expr_factory_, params_.query_ctx_,
                                                         select_stmt, table_def));
        }
      }
    }
  }
  return ret;
}
