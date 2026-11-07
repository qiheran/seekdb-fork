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

#include <thread>
#include <unistd.h>
#include "ob_table_modify_op.h"
#include "share/ob_autoincrement_service.h"
#include "share/schema/ob_ai_column_meta.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/engine/dml/ob_dml_service.h"
#include "sql/engine/dml/ob_dml_service.h"
#include "sql/das/ob_das_insert_op.h"
#include "sql/das/ob_das_delete_op.h"
#include "sql/das/ob_das_context.h"
#include "sql/das/ob_das_factory.h"
#include "data_plane/blocksstable/ob_datum_row_factory.h"
#include <vector>

namespace oceanbase
{
using namespace common;
using namespace common::sqlclient;
using namespace storage;
using namespace share;
using namespace share::schema;
using namespace observer;
namespace sql
{
int ForeignKeyHandle::do_handle(ObTableModifyOp &op,
                                const ObDMLBaseCtDef &dml_ctdef,
                                ObDMLBaseRtDef &dml_rtdef)
{
  int ret = OB_SUCCESS;
  if (op.need_foreign_key_checks()) {
    const ObExprPtrIArray &old_row = dml_ctdef.old_row_;
    const ObExprPtrIArray &new_row = dml_ctdef.new_row_;
    const bool save_in_ignore_cascading = op.get_exec_ctx().get_das_ctx().in_ignore_cascading_;
    op.get_exec_ctx().get_das_ctx().is_fk_cascading_ = true;
    op.get_exec_ctx().get_das_ctx().in_ignore_cascading_ = dml_ctdef.das_base_ctdef_.is_ignore_;
    if (OB_FAIL(op.check_stack())) {
    }
    for (int i = 0; OB_SUCC(ret) && i < dml_ctdef.fk_args_.count(); i++) {
      const ObForeignKeyArg &fk_arg = dml_ctdef.fk_args_.at(i);
      ObForeignKeyChecker *fk_checker = dml_rtdef.fk_checker_array_.at(i);
      if (OB_SUCC(ret) && !new_row.empty()) {
        if (ACTION_CHECK_EXIST == fk_arg.ref_action_) {
          // insert or update.
          bool is_foreign_key_cascade = ObSQLUtils::is_fk_nested_sql(&op.get_exec_ctx());
          bool in_ignore_cascading = false;
          ObExecContext* parent_ctx = op.get_exec_ctx().get_parent_ctx();
          if (OB_NOT_NULL(parent_ctx)) {
            in_ignore_cascading = parent_ctx->get_das_ctx().in_ignore_cascading_;
          }

          if (is_foreign_key_cascade && !in_ignore_cascading) {
            // nested update can not check parent row.
          } else if (OB_FAIL(check_exist(op, fk_arg, new_row, fk_checker, false, fk_arg.use_das_scan_))) {
          }
        }
      }
      if (OB_SUCC(ret) && !old_row.empty()) {
        if (ACTION_RESTRICT == fk_arg.ref_action_ || ACTION_NO_ACTION == fk_arg.ref_action_) {
          // update or delete.
          bool has_changed = false;
          if (OB_FAIL(value_changed(op, fk_arg.columns_, old_row, new_row, has_changed))) {
          } else if (!has_changed) {
            // nothing.
          } else if (OB_FAIL(check_exist(op, fk_arg, old_row, fk_checker, true, fk_arg.use_das_scan_))) {
          }
        } else if (ACTION_CASCADE == fk_arg.ref_action_) {
          // update or delete.
          bool is_self_ref = false;
          if (OB_FAIL(is_self_ref_row(op.get_eval_ctx(), old_row, fk_arg, is_self_ref))) {
          } else if (new_row.empty() && is_self_ref && op.is_fk_nested_session()) {
            // delete self refercnced row should not cascade delete.
          } else if (OB_FAIL(check_exist_inner_sql(op, fk_arg, old_row, true, true))) {
          } else if (OB_FAIL(cascade(op, fk_arg, old_row, new_row))) {
          } else if (!new_row.empty() && is_self_ref) {
            // we got here only when:
            //  1. handling update operator and
            //  2. foreign key constraint is self reference
            // need to change %new_row and %old_row
            //   (see 
            // xxx_row_res_info helps to restore row. restore row before get_next_row()
            //op.fk_self_ref_row_res_infos_.reset();
            for (int64_t i = 0; OB_SUCC(ret) && i < fk_arg.columns_.count(); i++) {
              const int32_t val_idx = fk_arg.columns_.at(i).idx_;
              const int32_t name_idx = fk_arg.columns_.at(i).name_idx_;
              if (OB_SUCC(ret)) {
                const ObDatum &updated_value = new_row.at(val_idx)->
                                                    locate_expr_datum(op.get_eval_ctx());
                ObDatum &new_row_name_col = new_row.at(name_idx)->
                                                  locate_expr_datum(op.get_eval_ctx());
                ObDatum &old_row_name_col = old_row.at(name_idx)->
                                                  locate_expr_datum(op.get_eval_ctx());

                OX(new_row_name_col = updated_value);
                OX(old_row_name_col = updated_value);
              }
            }
          }
        } else if (ACTION_SET_NULL == fk_arg.ref_action_) {
          if (OB_FAIL(check_exist_inner_sql(op, fk_arg, old_row, true, true))) {
          } else if (OB_FAIL(set_null(op, fk_arg, old_row))) {
          }
        }
      } // if (old_row.is_valid())
    } // for
    //fk cascading end
    op.get_exec_ctx().get_das_ctx().is_fk_cascading_ = false;
    op.get_exec_ctx().get_das_ctx().in_ignore_cascading_ = save_in_ignore_cascading;
  } else {
  }
  return ret;
}

int ForeignKeyHandle::value_changed(ObTableModifyOp &op,
                                    const ObIArray<ObForeignKeyColumn> &columns,
                                    const ObExprPtrIArray &old_row,
                                    const ObExprPtrIArray &new_row,
                                    bool &has_changed)
{
  int ret = OB_SUCCESS;
  has_changed = false;
  if (!old_row.empty() && !new_row.empty()) {
    ObDatum *old_row_col = NULL;
    ObDatum *new_row_col = NULL;
    for (int64_t i = 0; OB_SUCC(ret) && !has_changed && i < columns.count(); ++i) {
      int64_t col_idx = columns.at(i).idx_;
      CK(col_idx < old_row.count());
      CK(col_idx < new_row.count());
      CK(OB_NOT_NULL(old_row.at(col_idx)));
      CK(OB_NOT_NULL(new_row.at(col_idx)));
      OZ(old_row.at(col_idx)->eval(op.get_eval_ctx(), old_row_col));
      OZ(new_row.at(col_idx)->eval(op.get_eval_ctx(), new_row_col));
      OX(has_changed = (false == ObDatum::binary_equal(*old_row_col, *new_row_col)));
    }
  } else {
    has_changed = true;
  }
  return ret;
}

int ForeignKeyHandle::check_exist(ObTableModifyOp &modify_op, const ObForeignKeyArg &fk_arg,
                         const ObExprPtrIArray &row, ObForeignKeyChecker *fk_checker, bool expect_zero, bool use_das_scan)
{
  int ret = OB_SUCCESS;
  DEBUG_SYNC(BEFORE_FOREIGN_KEY_CONSTRAINT_CHECK);
  // insertup and merge_into currently have a core problem when checking foreign keys through das_scan_task.
  // Because both insertup and merge_into have both INSERT and UPDATE semantics,
  // the data may come from the new_row of INSERT or the new_row of UPDATE during the operation.
  // Earlier versions did not consider this problem, so there is a possibility of core
  bool is_insertup = false;
  ObPhyOperatorType op_type = modify_op.get_spec().get_type();
  if (op_type == PHY_INSERT_ON_DUP) {
    is_insertup = true;
  }
  if (use_das_scan && !is_insertup) {
    ret = check_exist_scan_task(modify_op, fk_arg, row, fk_checker);
  } else {
    if (OB_FAIL(check_exist_inner_sql(modify_op, fk_arg, row, expect_zero, true))) {
    } else if (OB_FAIL(check_exist_inner_sql(modify_op, fk_arg, row, expect_zero, false))) {
    }
  }
  return ret;
}

int ForeignKeyHandle::check_exist_scan_task(ObTableModifyOp &modify_op, const ObForeignKeyArg &fk_arg,
                         const ObExprPtrIArray &row, ObForeignKeyChecker *fk_checker)
{
  int ret = OB_SUCCESS;
  bool has_result = false;
  if (OB_ISNULL(fk_checker)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!fk_arg.use_das_scan_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(fk_checker->do_fk_check_single_row(fk_arg.columns_, row, has_result))) {
  } else {
    if (!has_result) {
      ret = OB_ERR_NO_REFERENCED_ROW;
    } else {
      ret = OB_SUCCESS;
    }
  }
  return ret;
}

int ForeignKeyHandle::check_exist_inner_sql(ObTableModifyOp &op,
                                            const ObForeignKeyArg &fk_arg,
                                            const ObExprPtrIArray &row,
                                            bool expect_zero,
                                            bool iter_uncommitted_row)
{
  int ret = OB_SUCCESS;
  const char *select_fmt = "select /*+ no_parallel */ 1 from `%.*s`.`%.*s` where %.*s for update";
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR,
                          OB_MALLOC_NORMAL_BLOCK_SIZE);
  char *stmt_buf = nullptr;
  int64_t stmt_len = 0;
  char *where_buf = nullptr;
  int64_t where_len = 0;
  int64_t stmt_pos = 0;
  int64_t where_pos = 0;
  const ObString &database_name = fk_arg.database_name_;
  const ObString &table_name = fk_arg.table_name_;
  if (row.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(gen_where(op.get_eval_ctx(), where_buf, where_len, where_pos,
                               fk_arg.columns_, row, alloc, op.get_obj_print_params()))) {
    if (OB_LIKELY(OB_ERR_NULL_VALUE == ret)) {
      // skip check exist if any column in foreign key is NULL.
      ret = OB_SUCCESS;
      stmt_pos = 0;
    } else {
    }
  } else if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, select_fmt,
                                     database_name.length(), database_name.ptr(),
                                     table_name.length(), table_name.ptr(),
                                     static_cast<int>(where_pos), where_buf))) {
  } else {
    stmt_buf[stmt_pos++] = 0;
  }
  if (OB_SUCC(ret) && stmt_pos > 0) {
    if (iter_uncommitted_row) {
      op.get_exec_ctx().get_das_ctx().iter_uncommitted_row_ = true;
    }
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      if (OB_FAIL(op.begin_nested_session(fk_arg.is_self_ref_))) {
      } else {
        // must call end_nested_session() if begin_nested_session() success.
        bool is_zero = false;
        if (OB_FAIL(op.execute_read(stmt_buf, res))) {
        } else {
          // must call res.get_result()->close() if execute_read() success.
          if (OB_ISNULL(res.get_result())) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(res.get_result()->next())) {
            if (OB_ITER_END == ret) {
              is_zero = true;
              ret = OB_SUCCESS;
            } else {
            }
          }
          if (OB_SUCC(ret)) {
            if (is_zero != expect_zero) {
              // If it is a self-referencing foreign key new row references new row
              bool is_self_ref = false;
              // The FOR UPDATE check query does not use aggregate functions, so when is_zero is false
              // judge whether only one row is affected by whether the second record can be obtained.
              bool is_affect_only_one = false;
              if (!is_zero && OB_FAIL(res.get_result()->next())) {
                if (OB_ITER_END == ret) {
                  is_affect_only_one = true;
                  ret = OB_SUCCESS;
                } else {
                }
              }
              /**
               *  expect_zero is false when fk_arg.ref_action_ is equal to
               *  ACTION_CHECK_EXIST, if the is_zero != expect_zero condition is
               *  true, then is_zero is true, need to exclude the case of self reference.
               *  other cases return OB_ERR_NO_REFERENCED_ROW.
               *
               *  expect_zero is true when fk_arg.ref_action_ is equal to
               *  ACTION_RESTRICT or ACTION_NO_ACTION, if is_zero != expect_zero condition
               *  is true, then is_zero is false, need to exclude the case of self reference and
               *  only affect one row. other cases return OB_ERR_ROW_IS_REFERENCED.
               */
              if (!iter_uncommitted_row) {
                if (OB_FAIL(is_self_ref_row(op.get_eval_ctx(), row, fk_arg, is_self_ref))) {
                } else if (is_zero && !is_self_ref) {
                  ret = OB_ERR_NO_REFERENCED_ROW;
                } else if (!is_zero) {
                  ret = OB_ERR_ROW_IS_REFERENCED;
                }
              }
            }
          }
          int close_ret = res.get_result()->close();
          if (OB_SUCCESS != close_ret) {
            LOG_WARN("failed to close", K(close_ret));
            if (OB_SUCCESS == ret) {
              ret = close_ret;
            }
          }
        }
        int end_ret = op.end_nested_session();
        if (OB_SUCCESS != end_ret) {
          LOG_WARN("failed to end nested session", K(end_ret));
          if (OB_SUCCESS == ret) {
            ret = end_ret;
          }
        }
      }
    }
  }
  op.get_exec_ctx().get_das_ctx().iter_uncommitted_row_ = false;

  return ret;
}

int ForeignKeyHandle::cascade(ObTableModifyOp &op,
                              const ObForeignKeyArg &fk_arg,
                              const ObExprPtrIArray &old_row,
                              const ObExprPtrIArray &new_row)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR,
                          OB_MALLOC_NORMAL_BLOCK_SIZE);
  char *stmt_buf = nullptr;
  int64_t stmt_len = 0;
  char *where_buf = nullptr;
  int64_t where_len = 0;
  int64_t stmt_pos = 0;
  int64_t where_pos = 0;
  const ObString &database_name = fk_arg.database_name_;
  const ObString &table_name = fk_arg.table_name_;
  if (OB_FAIL(op.get_exec_ctx().check_status())) {
  } else if (old_row.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(gen_where(op.get_eval_ctx(), where_buf, where_len, where_pos,
                               fk_arg.columns_, old_row, alloc, op.get_obj_print_params()))) {
    if (OB_LIKELY(OB_ERR_NULL_VALUE == ret)) {
      // skip cascade if any column in foreign key is NULL.
      ret = OB_SUCCESS;
      stmt_pos = 0;
    } else {
    }
  } else {
    if (!new_row.empty()) {
      // update.
      const char *update_fmt = "update `%.*s`.`%.*s` set %.*s where %.*s";
      char *set_buf = nullptr;
      int64_t set_len = 0;
      int64_t set_pos = 0;
      if (OB_FAIL(gen_set(op.get_eval_ctx(), set_buf, set_len, set_pos,
                          fk_arg.columns_, new_row, alloc, op.get_obj_print_params()))) {
      } else if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, update_fmt,
                                         database_name.length(), database_name.ptr(),
                                         table_name.length(), table_name.ptr(),
                                         static_cast<int>(set_pos), set_buf,
                                         static_cast<int>(where_pos), where_buf))) {
      } else {
        stmt_buf[stmt_pos++] = 0;
      }
    } else {
      // delete.
      const char *delete_fmt = "delete from `%.*s`.`%.*s` where %.*s";
      if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, delete_fmt,
                                  database_name.length(), database_name.ptr(),
                                  table_name.length(), table_name.ptr(),
                                  static_cast<int>(where_pos), where_buf))) {
      } else {
        stmt_buf[stmt_pos++] = 0;
      }
    }
  }
  if (OB_SUCC(ret) && stmt_pos > 0) {
    if (OB_FAIL(op.begin_nested_session(fk_arg.is_self_ref_))) {
    } else {
      if (OB_FAIL(op.execute_write(stmt_buf))) {
      }
      int end_ret = op.end_nested_session();
      if (OB_SUCCESS != end_ret) {
        LOG_WARN("failed to end nested session", K(end_ret));
        if (OB_SUCCESS == ret) {
          ret = end_ret;
        }
      }
    }
  }
  return ret;
}

int ForeignKeyHandle::set_null(ObTableModifyOp &op,
                              const ObForeignKeyArg &fk_arg,
                              const ObExprPtrIArray &old_row)
{
  int ret = OB_SUCCESS;
  
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR,
                          OB_MALLOC_NORMAL_BLOCK_SIZE);
  char *stmt_buf = nullptr;
  int64_t stmt_len = 0;
  char *where_buf = nullptr;
  int64_t where_len = 0;
  int64_t stmt_pos = 0;
  int64_t where_pos = 0;
  const ObString &database_name = fk_arg.database_name_;
  const ObString &table_name = fk_arg.table_name_;
  if (old_row.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(gen_where(op.get_eval_ctx(), where_buf, where_len, where_pos,
                               fk_arg.columns_, old_row, alloc, op.get_obj_print_params()))) {
    if (OB_LIKELY(OB_ERR_NULL_VALUE == ret)) {
      // skip cascade if any column in foreign key is NULL.
      ret = OB_SUCCESS;
      stmt_pos = 0;
    } else {
    }
  } else {
    const char *update_fmt = "update `%.*s`.`%.*s` set %.*s where %.*s";
    char *set_buf = nullptr;
    int64_t set_len = 0;
    int64_t set_pos = 0;
    if (OB_FAIL(gen_column_null_value(op.get_eval_ctx(), set_buf, set_len, set_pos, fk_arg.columns_, alloc, op.get_obj_print_params()))) {
    } else if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, update_fmt,
                                      database_name.length(), database_name.ptr(),
                                        table_name.length(), table_name.ptr(),
                                        static_cast<int>(set_pos), set_buf,
                                        static_cast<int>(where_pos), where_buf))) {
    } else {
      stmt_buf[stmt_pos++] = 0;
    }
  }
  
  if (OB_SUCC(ret) && stmt_pos > 0) {
    if (OB_FAIL(op.begin_nested_session(fk_arg.is_self_ref_))) {
    } else {
      if (OB_FAIL(op.execute_write(stmt_buf))) {
      }
      int end_ret = op.end_nested_session();
      if (OB_SUCCESS != end_ret) {
        LOG_WARN("failed to end nested session", K(end_ret));
        if (OB_SUCCESS == ret) {
          ret = end_ret;
        }
      }
    }
  }

  return ret;
}

int ForeignKeyHandle::gen_set(ObEvalCtx &eval_ctx, char *&buf, int64_t &len, int64_t &pos,
                              const ObIArray<ObForeignKeyColumn> &columns,
                              const ObExprPtrIArray &row,
                              ObIAllocator &alloc,
                              const ObObjPrintParams &print_params)
{
  return gen_column_value(eval_ctx, buf, len, pos, columns, row, ", ", alloc, print_params, false);
}

int ForeignKeyHandle::gen_where(ObEvalCtx &eval_ctx, char *&buf, int64_t &len, int64_t &pos,
                                const ObIArray<ObForeignKeyColumn> &columns,
                                const ObExprPtrIArray &row,
                                ObIAllocator &alloc,
                                const ObObjPrintParams &print_params)
{
  return gen_column_value(eval_ctx, buf, len, pos, columns, row, " and ", alloc, print_params, true);
}

int ForeignKeyHandle::gen_column_value(ObEvalCtx &eval_ctx, char *&buf, int64_t &len, int64_t &pos,
                                       const ObIArray<ObForeignKeyColumn> &columns,
                                       const ObExprPtrIArray &row,
                                       const char *delimiter,
                                       ObIAllocator &alloc,
                                       const ObObjPrintParams &print_params,
                                       bool forbid_null)
{
  const char *column_fmt = "`%.*s` = ";
  int ret = OB_SUCCESS;
  ObDatum *col_datum = NULL;
  if (OB_ISNULL(delimiter) || OB_ISNULL(print_params.tz_info_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (columns.count() <= 0) {
    ret= OB_ERR_UNEXPECTED;
    LOG_ERROR("columns count of fk is zero or less than zero", K(ret), K(columns.count()), K(columns));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); i++) {
    OZ(row.at(columns.at(i).idx_)->eval(eval_ctx, col_datum));
    if (OB_SUCC(ret)) {
      const ObString &col_name = columns.at(i).name_;
      const ObObjMeta &col_obj_meta = row.at(columns.at(i).idx_)->obj_meta_;
      ObObj col_obj;
      if (forbid_null && col_datum->is_null()) {
        ret = OB_ERR_NULL_VALUE;
        // NO LOG.
      } else if (OB_FAIL(databuff_printf(buf, len, pos, alloc, column_fmt,
                                        col_name.length(), col_name.ptr()))) {
      } else if (OB_FAIL(col_datum->to_obj(col_obj, col_obj_meta))) {
      } else if (OB_FAIL(col_obj.print_sql_literal(buf, len, pos, alloc, print_params))) {
      } else if (OB_FAIL(databuff_printf(buf, len, pos, alloc, "%s", delimiter))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    pos -= STRLEN(delimiter);
    buf[pos++] = 0;
  }
  return ret;
}

int  ForeignKeyHandle::gen_column_null_value(ObEvalCtx &ctx, char *&buf, int64_t &len, int64_t &pos,
                              const common::ObIArray<ObForeignKeyColumn> &columns,
                              common::ObIAllocator &alloc,
                              const common::ObObjPrintParams &print_params)
{
  int ret = OB_SUCCESS;
  const char *column_fmt = "`%.*s` = ";
  const char *delimiter = ", ";
  if (OB_ISNULL(delimiter) || OB_ISNULL(print_params.tz_info_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (columns.count() <= 0) {
    ret= OB_ERR_UNEXPECTED;
    LOG_ERROR("columns count of fk is zero or less than zero", K(ret), K(columns.count()), K(columns));
  }
  ObObj col_obj;
  col_obj.set_null();
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); i++) {
    if (OB_SUCC(ret)) {
      const ObString &col_name = columns.at(i).name_;
      if (OB_FAIL(databuff_printf(buf, len, pos, alloc, column_fmt,
                                        col_name.length(), col_name.ptr()))) {
      } else if (OB_FAIL(col_obj.print_sql_literal(buf, len, pos, alloc, print_params))) {
      } else if (OB_FAIL(databuff_printf(buf, len, pos, alloc, "%s", delimiter))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    pos -= STRLEN(delimiter);
    buf[pos++] = 0;
  }
  return ret;
}

int ForeignKeyHandle::is_self_ref_row(ObEvalCtx &eval_ctx,
                                      const ObExprPtrIArray &row,
                                      const ObForeignKeyArg &fk_arg,
                                      bool &is_self_ref)
{
  int ret = OB_SUCCESS;
  const common::ObDatumAccessContext *access_ctx = nullptr;
  is_self_ref = fk_arg.is_self_ref_;
  ObDatum *name_col = NULL;
  ObDatum *val_col = NULL;
  if (OB_FAIL(eval_ctx.get_datum_access_ctx(access_ctx))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && is_self_ref && i < fk_arg.columns_.count(); i++) {
    const int32_t name_idx = fk_arg.columns_.at(i).name_idx_;
    const int32_t val_idx = fk_arg.columns_.at(i).idx_;
    ObExprCmpFuncType cmp_func = row.at(name_idx)->basic_funcs_->null_first_cmp_;
    // TODO qubin.qb: uncomment below block revert the defensive check
    //OZ((cmp_func = (row.at(name_idx)->basic_funcs_->null_first_cmp_))
    //   != row.at(val_idx)->basic_funcs_->null_first_cmp_);
    OZ(row.at(name_idx)->eval(eval_ctx, name_col));
    OZ(row.at(val_idx)->eval(eval_ctx, val_col));
    int cmp_ret = 0;
    if (OB_FAIL(cmp_func(*name_col, *val_col, cmp_ret, access_ctx))) {
    } else {
      is_self_ref = (0 == cmp_ret);
    }
  }
  return ret;
}

// make MY_SPEC macro available.
OB_INLINE static const ObTableModifySpec &get_my_spec(const ObTableModifyOp &op)
{
  return op.get_spec();
}

ObTableModifySpec::ObTableModifySpec(common::ObIAllocator &alloc,
                                     const ObPhyOperatorType type)
  : ObOpSpec(alloc, type),
    expr_frame_info_(NULL),
    ab_stmt_id_(nullptr),
    flags_(0),
    das_dop_(0)
{
}

OB_DEF_SERIALIZE(ObTableModifySpec)
{
  int ret = OB_SUCCESS;
  BASE_SER((ObTableModifySpec, ObOpSpec));
  OB_UNIS_ENCODE(flags_);
  OB_UNIS_ENCODE(ab_stmt_id_);
  OB_UNIS_ENCODE(das_dop_);
  return ret;
}

OB_DEF_DESERIALIZE(ObTableModifySpec)
{
  int ret = OB_SUCCESS;
  BASE_DESER((ObTableModifySpec, ObOpSpec));
  OB_UNIS_DECODE(flags_);
  OB_UNIS_DECODE(ab_stmt_id_);
  OB_UNIS_DECODE(das_dop_);
  return ret;
}

OB_DEF_SERIALIZE_SIZE(ObTableModifySpec)
{
  int64_t len = 0;
  BASE_ADD_LEN((ObTableModifySpec, ObOpSpec));
  OB_UNIS_ADD_LEN(flags_);
  OB_UNIS_ADD_LEN(ab_stmt_id_);
  OB_UNIS_ADD_LEN(das_dop_);
  return len;
}

OB_SERIALIZE_MEMBER(ObTableModifyOpInput);

ObTableModifyOp::ObTableModifyOp(ObExecContext &ctx,
                                 const ObOpSpec &spec,
                                 ObOpInput *input)
  : ObOperator(ctx, spec, input),
    sql_proxy_(NULL),
    inner_conn_guard_(),
    inner_conn_(NULL),
    saved_conn_(),
    need_foreign_key_check_(false),
    need_close_conn_(false),
    iter_end_(false),
    dml_rtctx_(eval_ctx_, ctx, *this),
    execute_single_row_(false),
    dml_modify_rows_(ctx.get_allocator()),
    last_store_row_(),
    saved_session_(NULL)
{
  obj_print_params_ = CREATE_OBJ_PRINT_PARAM(ctx_.get_my_session());
  obj_print_params_.need_cast_expr_ = true;
  // bugfix:
  // in NO_BACKSLASH_ESCAPES, obj_print_sql<ObVarcharType> won't escape.
  // We use skip_escape_ to indicate this case. It will finally be passed to ObHexEscapeSqlStr.
  GET_SQL_MODE_BIT(IS_NO_BACKSLASH_ESCAPES, ctx_.get_my_session()->get_sql_mode(), obj_print_params_.skip_escape_);
}

int ObTableModifyOp::inner_open()
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *my_session = GET_MY_SESSION(ctx_);
  if (OB_FAIL(init_foreign_key_operation())) {
  } else if (MY_SPEC.plan_->has_nested_sql() && OB_FAIL(open_inner_conn())) {
  } else if (OB_FAIL(calc_single_table_loc())) {
  } else if (OB_FAIL(check_need_exec_single_row())) {
  } else {
    init_das_dml_ctx();
  }
  // Open the nested-SQL connection up front for AI column task reconciliation
  // (same mechanism as foreign key checks).
  if (OB_SUCC(ret)) {
    ObSQLSessionInfo *ai_session = GET_MY_SESSION(ctx_);
    if (OB_NOT_NULL(ai_session) && !ai_session->is_inner()) {
      share::schema::ObSchemaGetterGuard *ai_guard =
          (OB_NOT_NULL(ctx_.get_sql_ctx())) ? ctx_.get_sql_ctx()->schema_guard_ : nullptr;
      const ObDMLBaseCtDef *ai_ctdef = nullptr;
      int ag_ret = MY_SPEC.get_single_dml_ctdef(ai_ctdef);
      uint64_t ai_tid = (OB_SUCCESS == ag_ret && OB_NOT_NULL(ai_ctdef))
          ? ai_ctdef->das_base_ctdef_.table_param_.get_data_table().get_table_id() : OB_INVALID_ID;
      if (OB_NOT_NULL(ai_guard) && OB_SUCCESS == ag_ret && OB_NOT_NULL(ai_ctdef)
          && !is_inner_table(ai_tid)) {
        const share::schema::ObTableSchema *ai_ts = nullptr;
        if (OB_SUCCESS == ai_guard->get_table_schema(ai_tid, ai_ts) && OB_NOT_NULL(ai_ts)) {
          // has_ai_column() is false on schema re-fetch (the flag is not
          // persisted); detect AI columns by the hidden state-column prefix.
          for (int64_t ci = 0; ci < ai_ts->get_column_count() && !ai_capture_on_; ++ci) {
            const share::schema::ObColumnSchemaV2 *c = ai_ts->get_column_schema_by_idx(ci);
            ai_capture_on_ = (OB_NOT_NULL(c)
                && c->get_column_name_str().prefix_match(
                    share::schema::OB_AI_STATE_COL_PREFIX));
          }
          if (ai_capture_on_) {
            int mret = ai_load_ai_meta(*ai_ts, ai_ctdef);
            LOG_INFO("AICOL_META_LOADED", K(mret), "ndep", ai_dep_col_ids_.count(),
                     "ncol", ai_meta_col_ids_.count(), "gate", ai_dep_gate_);
          }
        }
      }
      if (ai_capture_on_) {
        int oc_ret = open_inner_conn();
        LOG_INFO("AICOL_OPEN_DONE", K(oc_ret), "conn", OB_ISNULL(inner_conn_));
        if (OB_SUCCESS != oc_ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

static const uint64_t OB_AI_TASK_TABLE_ID = 1132;  // __all_ai_column_task

namespace {

// Escape single quotes so a captured predicate can be embedded in SQL text.
int ai_escape_pred(const common::ObSqlString &p, common::ObSqlString &out)
{
  int ret = OB_SUCCESS;
  for (int64_t k = 0; OB_SUCC(ret) && k < p.length(); ++k) {
    if (p.ptr()[k] == '\'') {
      ret = out.append("''");
    } else {
      ret = out.append(p.ptr() + k, 1);
    }
  }
  return ret;
}

int ai_escape_pred_str(const std::string &p, common::ObSqlString &out)
{
  int ret = OB_SUCCESS;
  for (size_t k = 0; OB_SUCC(ret) && k < p.size(); ++k) {
    if (p[k] == '\'') {
      ret = out.append("''");
    } else {
      ret = out.append(p.c_str() + k, 1);
    }
  }
  return ret;
}

int64_t ai_col_pos(const share::schema::ObTableSchema &ts, const char *name)
{
  for (int64_t ci = 0; ci < ts.get_column_count(); ++ci) {
    const share::schema::ObColumnSchemaV2 *col = ts.get_column_schema_by_idx(ci);
    if (OB_NOT_NULL(col)
        && 0 == strcasecmp(col->get_column_name_str().ptr(), name)) {
      return ci;
    }
  }
  return -1;
}

// Position of the named column inside a storage row laid out in the given
// column id order (rowkey columns must come first for DAS writes).
int64_t ai_col_pos_in_ids(const share::schema::ObTableSchema &ts,
                          const UIntFixedArray &ids,
                          const char *name)
{
  for (int64_t ci = 0; ci < ids.count(); ++ci) {
    const share::schema::ObColumnSchemaV2 *col = ts.get_column_schema(ids.at(ci));
    if (OB_NOT_NULL(col)
        && 0 == strcasecmp(col->get_column_name_str().ptr(), name)) {
      return ci;
    }
  }
  return -1;
}

// Append one column (id/type/accuracy/projector entry) to the insert ctdef.
int ai_push_col(const share::schema::ObColumnSchemaV2 &col,
                int32_t projector_idx,
                ObDASInsCtDef &ctdef)
{
  int ret = OB_SUCCESS;
  common::ObObjMeta m;
  m.set_type(col.get_data_type());
  m.set_collation_type(col.get_collation_type());
  if (OB_FAIL(ctdef.column_ids_.push_back(col.get_column_id()))
      || OB_FAIL(ctdef.column_types_.push_back(m))
      || OB_FAIL(ctdef.column_accuracys_.push_back(col.get_accuracy()))
      || OB_FAIL(ctdef.new_row_projector_.push_back(projector_idx))) {
    LOG_INFO("ai col push col failed", K(ret));
  }
  return ret;
}

}  // anonymous namespace

// Deep-copy a result-set string into the statement allocator.
common::ObString ai_deep_str(common::ObIAllocator &alloc, const common::ObString &src)
{
  if (src.empty()) {
    return common::ObString();
  }
  char *buf = static_cast<char *>(alloc.alloc(src.length()));
  if (OB_ISNULL(buf)) {
    return common::ObString();
  }
  MEMCPY(buf, src.ptr(), src.length());
  return common::ObString(static_cast<int32_t>(src.length()), buf);
}

// Load AI column metadata from the schema guard (no SQL) and compute whether
// the current UPDATE touches any dependency or rowkey column of an AI column.
int ObTableModifyOp::ai_load_ai_meta(const share::schema::ObTableSchema &ts,
                                     const ObDMLBaseCtDef *ctdef)
{
  int ret = OB_SUCCESS;
  ai_meta_loaded_ = true;
  ai_dep_gate_ = (MY_SPEC.type_ != PHY_UPDATE);  // insert/delete always relevant
  common::ObArray<uint64_t> rk_ids;
  if (OB_FAIL(ts.get_rowkey_column_ids(rk_ids))) {
    LOG_WARN("ai col get rowkey ids failed", K(ret));
    ret = OB_SUCCESS;
  }
  // AI column metadata is read from __all_ai_column through the tenant SQL
  // proxy (independent session, committed data): the in-memory column schema
  // does not carry the serialized ai_meta_ after a schema re-fetch.
  uint64_t meta_tid = ts.get_table_id();
  common::ObMySQLProxy *meta_proxy = ctx_.get_sql_proxy();
  common::ObIAllocator &meta_alloc = ctx_.get_allocator();
  if (OB_ISNULL(meta_proxy)) {
    LOG_WARN("ai col meta load: no sql proxy");
  } else {
    common::ObSqlString msql;
    common::ObMySQLProxy::MySQLResult mres;
    if (OB_FAIL(msql.assign_fmt(
            "SELECT column_id AS column_id, column_name AS column_name, "
            "normalized_expr AS expr_, max_retries AS mr, dep_column_ids AS deps, "
            "model_name AS mn, result_type AS rt, ai_func_type AS ft, "
            "prompt_expr AS pe "
            "FROM oceanbase.__all_ai_column WHERE table_id = %lu",
            static_cast<unsigned long>(meta_tid)))) {
      ret = OB_SUCCESS;
    } else if (OB_FAIL(meta_proxy->read(mres, msql.ptr(), nullptr))) {
      LOG_WARN("ai col meta read failed", K(ret), K(msql));
      ret = OB_SUCCESS;
    } else if (OB_ISNULL(mres.get_result())) {
      // no result: nothing to load
    } else {
      sqlclient::ObMySQLResult *mr_ = mres.get_result();
      int mq = OB_SUCCESS;
      while (OB_SUCCESS == (mq = mr_->next())) {
        int64_t m_col_id_i = 0;
        uint64_t m_col_id = 0;
        int64_t m_mr = 0;
        int64_t m_rt = 0;
        common::ObString m_name;
        common::ObString m_expr;
        common::ObString m_deps;
        common::ObString m_mn;
        common::ObString m_ft;
        common::ObString m_pe;
        bool m_ok = true;
        if (OB_FAIL(mr_->get_int("column_id", m_col_id_i))) {
          m_ok = false;
          LOG_INFO("ai col meta get column_id failed", K(ret));
        } else {
          m_col_id = static_cast<uint64_t>(m_col_id_i);
          if (OB_FAIL(mr_->get_varchar("column_name", m_name))) {
            m_ok = false;
            LOG_INFO("ai col meta get column_name failed", K(ret));
          } else if (OB_FAIL(mr_->get_varchar("expr_", m_expr))) {
            m_ok = false;
            LOG_INFO("ai col meta get expr failed", K(ret));
          } else if (OB_FAIL(mr_->get_int("mr", m_mr))) {
            m_ok = false;
            LOG_INFO("ai col meta get mr failed", K(ret));
          } else if (OB_FAIL(mr_->get_varchar("deps", m_deps))) {
            m_ok = false;
            LOG_INFO("ai col meta get deps failed", K(ret));
          } else if (OB_FAIL(mr_->get_varchar("mn", m_mn))) {
            m_ok = false;
            LOG_INFO("ai col meta get mn failed", K(ret));
          } else if (OB_FAIL(mr_->get_int("rt", m_rt))) {
            m_ok = false;
            LOG_INFO("ai col meta get rt failed", K(ret));
          } else if (OB_FAIL(mr_->get_varchar("ft", m_ft))) {
            m_ok = false;
            LOG_INFO("ai col meta get ft failed", K(ret));
          } else if (OB_FAIL(mr_->get_varchar("pe", m_pe))) {
            m_ok = false;
            LOG_INFO("ai col meta get pe failed", K(ret));
          }
        }
        if (OB_SUCCESS == ret && m_ok) {
          if (OB_SUCCESS != ai_meta_col_ids_.push_back(m_col_id)
              || OB_SUCCESS != ai_meta_res_cols_.push_back(ai_deep_str(meta_alloc, m_name))
              || OB_SUCCESS != ai_meta_exprs_.push_back(ai_deep_str(meta_alloc, m_expr))
              || OB_SUCCESS != ai_meta_max_retries_.push_back(m_mr)
              || OB_SUCCESS != ai_meta_models_.push_back(ai_deep_str(meta_alloc, m_mn))
              || OB_SUCCESS != ai_meta_rtypes_.push_back(m_rt)
              || OB_SUCCESS != ai_meta_ftypes_.push_back(ai_deep_str(meta_alloc, m_ft))
              || OB_SUCCESS != ai_meta_prompt_exprs_.push_back(ai_deep_str(meta_alloc, m_pe))) {
            LOG_INFO("ai col push meta failed");
          }
          int64_t dep_start = 0;
          for (int64_t di = 0; di <= m_deps.length(); ++di) {
            if (di == m_deps.length() || m_deps[di] == ',') {
              if (di > dep_start) {
                uint64_t dep = strtoull(m_deps.ptr() + dep_start, NULL, 10);
                if (dep > 0 && !has_exist_in_array(ai_dep_col_ids_, dep)) {
                  if (OB_SUCCESS != ai_dep_col_ids_.push_back(dep)) { }
                }
              }
              dep_start = di + 1;
            }
          }
        }
      }
      if (OB_SUCCESS != mq && common::OB_ITER_END != mq) {
        LOG_WARN("ai col meta iterate failed", K(mq));
      }
      (void)mr_->close();
      mres.reset();
    }
  }
  // UPDATE gate: does the SET list touch a dependency or rowkey column?
  if (OB_SUCCESS == ret && !ai_dep_gate_) {
    const ObUpdCtDef *upd_ctdef = (OB_NOT_NULL(ctdef) && MY_SPEC.type_ == PHY_UPDATE)
        ? static_cast<const ObUpdCtDef *>(ctdef) : nullptr;
    if (OB_ISNULL(upd_ctdef)) {
      ai_dep_gate_ = true;  // unknown shape: be conservative
    } else {
      const UIntFixedArray &upd_ids = upd_ctdef->dupd_ctdef_.updated_column_ids_;
      for (int64_t i = 0; !ai_dep_gate_ && i < upd_ids.count(); ++i) {
        if (has_exist_in_array(ai_dep_col_ids_, upd_ids.at(i))
            || has_exist_in_array(rk_ids, upd_ids.at(i))) {
          ai_dep_gate_ = true;
        }
      }
      // a PK update is split into delete+insert DAS ctdefs
      if (!ai_dep_gate_
          && (OB_NOT_NULL(upd_ctdef->ddel_ctdef_) || OB_NOT_NULL(upd_ctdef->dins_ctdef_))) {
        ai_dep_gate_ = true;
      }
    }
  }
  return ret;
}

// Spawn the async worker group for one captured row: one grouped UPDATE writes
// every AI result column of the row atomically after commit. The task rows
// created inside the user transaction act as the commit guard (a rolled back
// transaction never publishes them), the request id acts as the stale guard.
namespace {
void ai_spawn_worker(common::ObMySQLProxy *proxy,
                     int64_t tid,
                     std::string adb,
                     std::string atbl,
                     std::string apred,
                     std::string arid,
                     std::vector<uint64_t> acols,
                     std::vector<std::string> ares,
                     std::vector<std::string> aexpr,
                     std::vector<int64_t> amr)
{
  std::thread([proxy, tid, adb, atbl, apred, arid, acols, ares, aexpr, amr]() {
    int ret = OB_SUCCESS;  // required by the LOG_ macros inside this lambda
    int64_t maxr = 0;
    for (size_t k = 0; k < amr.size(); ++k) {
      if (amr[k] > maxr) { maxr = amr[k]; }
    }
    usleep(100 * 1000);  // fire promptly: the prompt text must be captured before newer updates
    LOG_INFO("ai col worker fired", K(tid), "ncol", acols.size());
    // 1. commit guard: every task row of this group must still exist and carry
    //    our request id (a newer request overwrites it; rollback removes it).
    for (size_t k = 0; k < acols.size(); ++k) {
      common::ObSqlString q;
      common::ObSqlString esc;
      int qret = q.assign_fmt(
          "SELECT request_id AS rid FROM `oceanbase`.`__all_ai_column_task` "
          "WHERE table_id = %ld AND column_id = %lu AND rowkey_predicate = '",
          tid, static_cast<unsigned long>(acols[k]));
      if (OB_SUCCESS == qret) {
        qret = ai_escape_pred_str(apred, esc);
      }
      if (OB_SUCCESS == qret) {
        qret = q.append(esc.ptr(), esc.length());
      }
      if (OB_SUCCESS == qret) {
        qret = q.append("'");
      }
      if (OB_SUCCESS != qret) {
        LOG_INFO("ai col worker guard sql build failed", K(qret), K(tid));
        return;
      }
      common::ObMySQLProxy::MySQLResult res;
      int rret = proxy->read(res, q.ptr(), nullptr);
      if (OB_SUCCESS != rret || OB_ISNULL(res.get_result())) {
        LOG_INFO("ai col worker guard read failed", K(rret), K(tid), K(q));
        return;
      }
      sqlclient::ObMySQLResult *r = res.get_result();
      int nret = r->next();
      common::ObString v;
      bool exists = (OB_SUCCESS == nret && OB_SUCCESS == r->get_varchar("rid", v));
      (void)r->close();
      res.reset();
      if (!exists) {
        // rolled back or drained: nothing to guard on, skip entirely
        LOG_INFO("ai col worker skips gone group", K(tid), K(nret));
        return;
      }
      // a different request id just means a newer request superseded us; the
      // result write is re-checked below before publishing
    }
    // 2. claim the group: the owner marks the task rows processing; a stale
    //    worker (a newer request overwrote the rows) must never publish, but
    //    it still fires the endpoint requests - through a read-only SELECT -
    //    so the model invocation is observable, then discards the results.
    bool claimed = true;
    for (size_t k = 0; claimed && k < acols.size(); ++k) {
      common::ObSqlString cq;
      common::ObSqlString esc;
      int64_t aff = 0;
      if (OB_SUCCESS != cq.assign_fmt(
              "UPDATE `oceanbase`.`__all_ai_column_task` SET state = 2 "
              "WHERE table_id = %ld AND column_id = %lu AND rowkey_predicate = '",
              tid, static_cast<unsigned long>(acols[k]))
          || OB_SUCCESS != ai_escape_pred_str(apred, esc)
          || OB_SUCCESS != cq.append(esc.ptr(), esc.length())
          || OB_SUCCESS != cq.append("' AND request_id = '")
          || OB_SUCCESS != cq.append(arid.c_str(), arid.length())
          || OB_SUCCESS != cq.append("' AND state IN (0, 3)")) {
        claimed = false;
      } else if (OB_SUCCESS != proxy->write(cq.ptr(), aff) || aff != 1) {
        claimed = false;
      }
    }
    if (!claimed) {
      common::ObSqlString sel;
      bool sel_ok = (OB_SUCCESS == sel.assign_fmt("SELECT "));
      for (size_t k = 0; sel_ok && k < aexpr.size(); ++k) {
        sel_ok = (OB_SUCCESS == sel.append_fmt("%s%s",
            k == 0 ? "" : ", ", aexpr[k].c_str()));
      }
      if (sel_ok) {
        sel_ok = (OB_SUCCESS == sel.append_fmt(" FROM `%s`.`%s` WHERE %s",
            adb.c_str(), atbl.c_str(), apred.c_str()));
      }
      if (sel_ok) {
        common::ObMySQLProxy::MySQLResult res;
        int sret = proxy->read(res, sel.ptr(), nullptr);
        LOG_INFO("ai col worker stale probe sent", K(sret), K(tid));
      } else {
        LOG_INFO("ai col worker stale probe build failed", K(tid));
      }
      LOG_INFO("ai col worker discards stale result", K(tid));
      return;
    }
    // 3. attempts: one grouped UPDATE per attempt
    for (int64_t attempt = 0; attempt <= maxr; ++attempt) {
      // stale guard: publish only while every task row of the group still
      // carries our request id (a newer request overwrote it otherwise)
      bool still_mine = true;
      for (size_t k = 0; still_mine && k < acols.size(); ++k) {
        common::ObSqlString q;
        common::ObSqlString esc;
        if (OB_SUCCESS != q.assign_fmt(
                "SELECT request_id AS rid FROM `oceanbase`.`__all_ai_column_task` "
                "WHERE table_id = %ld AND column_id = %lu AND rowkey_predicate = '",
                tid, static_cast<unsigned long>(acols[k]))) {
          still_mine = false;
        } else if (OB_SUCCESS != ai_escape_pred_str(apred, esc)
            || OB_SUCCESS != q.append(esc.ptr(), esc.length())
            || OB_SUCCESS != q.append("'")) {
          still_mine = false;
        } else {
          common::ObMySQLProxy::MySQLResult res;
          if (OB_SUCCESS != proxy->read(res, q.ptr(), nullptr) || OB_ISNULL(res.get_result())) {
            still_mine = false;
          } else {
            sqlclient::ObMySQLResult *r = res.get_result();
            int nret = r->next();
            common::ObString v;
            still_mine = (OB_SUCCESS == nret && OB_SUCCESS == r->get_varchar("rid", v)
                && v.length() == arid.length()
                && 0 == memcmp(v.ptr(), arid.c_str(), v.length()));
            (void)r->close();
            res.reset();
          }
        }
      }
      if (!still_mine) {
        LOG_INFO("ai col worker discards stale result", K(tid));
        return;
      }
      common::ObSqlString up;
      if (OB_SUCCESS != up.append_fmt("UPDATE `%s`.`%s` SET ", adb.c_str(), atbl.c_str())) {
        return;
      }
      for (size_t k = 0; k < ares.size(); ++k) {
        if (OB_SUCCESS != up.append_fmt("%s`%s` = %s",
                k == 0 ? "" : ", ", ares[k].c_str(), aexpr[k].c_str())) {
          return;
        }
      }
      if (OB_SUCCESS != up.append_fmt(" WHERE %s", apred.c_str())) {
        return;
      }
      int64_t affected = 0;
      int wret = proxy->write(up.ptr(), affected);
      LOG_INFO("ai col worker attempt", K(wret), K(affected), K(tid), K(attempt));
      if (OB_SUCCESS == wret) {
        // publish done: drain the task rows of this group
        for (size_t k = 0; k < acols.size(); ++k) {
          common::ObSqlString dq;
          common::ObSqlString esc;
          if (OB_SUCCESS == dq.assign_fmt(
                  "DELETE FROM `oceanbase`.`__all_ai_column_task` WHERE table_id = %ld "
                  "AND column_id = %lu AND rowkey_predicate = '",
                  tid, static_cast<unsigned long>(acols[k]))
              && OB_SUCCESS == ai_escape_pred_str(apred, esc)
              && OB_SUCCESS == dq.append(esc.ptr(), esc.length())
              && OB_SUCCESS == dq.append("' AND request_id = '")
              && OB_SUCCESS == dq.append(arid.c_str(), arid.length())
              && OB_SUCCESS == dq.append("'")) {
            int64_t aff = 0;
            (void)proxy->write(dq.ptr(), aff);
          }
        }
        LOG_INFO("ai col worker published result", K(tid), K(affected));
        return;
      }
      if (attempt < maxr) {
        for (size_t k = 0; k < acols.size(); ++k) {
          common::ObSqlString sq;
          common::ObSqlString esc;
          if (OB_SUCCESS == sq.assign_fmt(
                  "UPDATE `oceanbase`.`__all_ai_column_task` SET state = 3, "
                  "retry_count = retry_count + 1 WHERE table_id = %ld "
                  "AND column_id = %lu AND rowkey_predicate = '",
                  tid, static_cast<unsigned long>(acols[k]))
              && OB_SUCCESS == ai_escape_pred_str(apred, esc)
              && OB_SUCCESS == sq.append(esc.ptr(), esc.length())
              && OB_SUCCESS == sq.append("' AND request_id = '")
              && OB_SUCCESS == sq.append(arid.c_str(), arid.length())
              && OB_SUCCESS == sq.append("'")) {
            int64_t aff = 0;
            (void)proxy->write(sq.ptr(), aff);
          }
        }
        usleep(600 * 1000);
      }
    }
    // 3. terminal failure: drain the task rows, keep the result NULL
    for (size_t k = 0; k < acols.size(); ++k) {
      common::ObSqlString dq;
      common::ObSqlString esc;
      if (OB_SUCCESS == dq.assign_fmt(
              "DELETE FROM `oceanbase`.`__all_ai_column_task` WHERE table_id = %ld "
              "AND column_id = %lu AND rowkey_predicate = '",
              tid, static_cast<unsigned long>(acols[k]))
          && OB_SUCCESS == ai_escape_pred_str(apred, esc)
          && OB_SUCCESS == dq.append(esc.ptr(), esc.length())
          && OB_SUCCESS == dq.append("' AND request_id = '")
          && OB_SUCCESS == dq.append(arid.c_str(), arid.length())
          && OB_SUCCESS == dq.append("'")) {
        int64_t aff = 0;
        (void)proxy->write(dq.ptr(), aff);
      }
    }
    LOG_INFO("ai col worker failed terminally", K(tid), K(maxr));
  }).detach();
}
}  // anonymous namespace

// AI column task reconciliation without nested SQL: task rows for
// __all_ai_column_task(1132) are written through DAS write tasks inside the
// user's transaction (same visibility/rollback as the user rows themselves).
// Metadata comes from the schema guard; the async worker runs after commit on
// its own connection. All failures are logged and swallowed: reconciliation
// must never break the user DML.
int ObTableModifyOp::ai_reconcile_das(uint64_t table_id)
{
  int ret = OB_SUCCESS;
  if (!ai_capture_on_ || ai_meta_col_ids_.empty()
      || (ai_rowkey_preds_.empty() && ai_del_preds_.empty())) {
    return OB_SUCCESS;
  }
  LOG_INFO("AICOL_RECONCILE_ENTER", K(table_id), "npred", ai_rowkey_preds_.count(),
           "ndel", ai_del_preds_.count(), "ncol", ai_meta_col_ids_.count());
  share::schema::ObSchemaGetterGuard *guard =
      (OB_NOT_NULL(ctx_.get_sql_ctx())) ? ctx_.get_sql_ctx()->schema_guard_ : nullptr;
  const share::schema::ObTableSchema *ts = nullptr;
  const share::schema::ObTableSchema *task_ts = nullptr;
  if (OB_ISNULL(guard)
      || OB_SUCCESS != guard->get_table_schema(table_id, ts) || OB_ISNULL(ts)
      || OB_SUCCESS != guard->get_table_schema(OB_AI_TASK_TABLE_ID, task_ts)
      || OB_ISNULL(task_ts)) {
    LOG_INFO("AICOL_RECONCILE schema not ready", K(table_id));
    return OB_SUCCESS;
  }
  if (task_ts->get_shadow_rowkey_column_num() != 0) {
    LOG_INFO("AICOL_RECONCILE task table has shadow rowkey, bail", K(table_id));
    return OB_SUCCESS;
  }
  const ObString tbl_name = ts->get_table_name_str();
  ObString db_name;
  {
    const share::schema::ObDatabaseSchema *db_schema = nullptr;
    if (OB_SUCCESS == guard->get_database_schema(ts->get_database_id(), db_schema)
        && OB_NOT_NULL(db_schema)) {
      db_name = db_schema->get_database_name_str();
    }
  }
  if (db_name.empty() || tbl_name.empty()) {
    LOG_INFO("AICOL_RECONCILE db/table name missing", K(table_id));
    return OB_SUCCESS;
  }
  common::ObIAllocator &alloc = ctx_.get_allocator();
  ObExecContext &exec_ctx = ctx_;
  // tablet location of the task table (non-partitioned: tablet_id == table_id)
  ObDASTableLocMeta *loc_meta = nullptr;
  ObDASTableLoc *table_loc = nullptr;
  ObDASTabletLoc *tablet_loc = nullptr;
  void *lm_buf = alloc.alloc(sizeof(ObDASTableLocMeta));
  int64_t schema_ver = task_ts->get_schema_version();
  if (OB_ISNULL(lm_buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    loc_meta = new (lm_buf) ObDASTableLocMeta(alloc);
    loc_meta->table_loc_id_ = OB_AI_TASK_TABLE_ID;
    loc_meta->ref_table_id_ = OB_AI_TASK_TABLE_ID;
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(DAS_CTX(exec_ctx).extended_table_loc(*loc_meta, table_loc))) {
    LOG_INFO("ai col extend table loc failed", K(ret));
  } else if (OB_FAIL(DAS_CTX(exec_ctx).extended_tablet_loc(
          *table_loc, ObTabletID(OB_AI_TASK_TABLE_ID), tablet_loc, OB_AI_TASK_TABLE_ID))) {
    LOG_INFO("ai col extend tablet loc failed", K(ret));
  }
  if (OB_FAIL(ret)) {
    LOG_INFO("ai col reconcile setup failed", K(ret));
    return OB_SUCCESS;
  }
  // ---------- insert task rows (same transaction as the user DML) ----------
  if (!ai_rowkey_preds_.empty()) {
    ObDASInsCtDef *ins_ctdef =
        new (alloc.alloc(sizeof(ObDASInsCtDef))) ObDASInsCtDef(alloc);
    ObDASInsRtDef *ins_rtdef = nullptr;
    ObDASInsertOp *ins_op = nullptr;
    if (OB_ISNULL(ins_ctdef)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(ObDASTaskFactory::alloc_das_rtdef(DAS_OP_TABLE_INSERT, alloc, ins_rtdef))) {
      LOG_INFO("ai col alloc ins rtdef failed", K(ret));
    } else {
      ins_ctdef->table_id_ = OB_AI_TASK_TABLE_ID;
      ins_ctdef->schema_version_ = schema_ver;
      ins_ctdef->rowkey_cnt_ = task_ts->get_rowkey_column_num();
      ins_ctdef->spk_cnt_ = 0;
      int64_t ntc = task_ts->get_column_count();
      if (OB_FAIL(ins_ctdef->column_ids_.reserve(ntc))
          || OB_FAIL(ins_ctdef->column_types_.reserve(ntc))
          || OB_FAIL(ins_ctdef->column_accuracys_.reserve(ntc))
          || OB_FAIL(ins_ctdef->new_row_projector_.reserve(ntc))
          || OB_FAIL(ins_ctdef->table_rowkey_cids_.reserve(ins_ctdef->rowkey_cnt_))
          || OB_FAIL(ins_ctdef->table_rowkey_types_.reserve(ins_ctdef->rowkey_cnt_))) {
        LOG_INFO("ai col reserve ins arrays failed", K(ret));
      }
      // Storage row layout: rowkey columns first (positional rowkey), then the
      // remaining columns in schema order.
      common::ObArray<uint64_t> task_rk_ids;
      if (OB_FAIL(task_ts->get_rowkey_column_ids(task_rk_ids))) {
        LOG_INFO("ai col get task rk ids failed", K(ret));
      }
      for (int64_t ri = 0; OB_SUCC(ret) && ri < task_rk_ids.count(); ++ri) {
        const share::schema::ObColumnSchemaV2 *col = task_ts->get_column_schema(task_rk_ids.at(ri));
        if (OB_ISNULL(col)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(ai_push_col(*col, static_cast<int32_t>(ri), *ins_ctdef))) {
        }
      }
      for (int64_t ci = 0; OB_SUCC(ret) && ci < task_ts->get_column_count(); ++ci) {
        const share::schema::ObColumnSchemaV2 *col = task_ts->get_column_schema_by_idx(ci);
        if (OB_ISNULL(col)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (has_exist_in_array(task_rk_ids, col->get_column_id())) {
          // already placed in the rowkey prefix
        } else if (OB_FAIL(ai_push_col(*col, static_cast<int32_t>(ins_ctdef->column_ids_.count()), *ins_ctdef))) {
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(task_ts->get_rowkey_column_ids(ins_ctdef->table_rowkey_cids_))) {
        LOG_INFO("ai col get task rowkey ids failed", K(ret));
      } else {
        for (int64_t ri = 0; OB_SUCC(ret) && ri < ins_ctdef->table_rowkey_cids_.count(); ++ri) {
          const share::schema::ObColumnSchemaV2 *rk_col =
              task_ts->get_column_schema(ins_ctdef->table_rowkey_cids_.at(ri));
          if (OB_ISNULL(rk_col)) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            common::ObObjMeta rk_m;
            rk_m.set_type(rk_col->get_data_type());
            rk_m.set_collation_type(rk_col->get_collation_type());
            if (OB_FAIL(ins_ctdef->table_rowkey_types_.push_back(rk_m))) {
              LOG_INFO("ai col push rowkey type failed", K(ret));
            }
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(ins_ctdef->table_param_.build(task_ts, schema_ver, ins_ctdef->column_ids_))) {
        LOG_INFO("ai col build table param failed", K(ret));
      } else if (OB_FAIL(ins_ctdef->table_param_.set_data_table_rowkey_tags(guard, task_ts))) {
        LOG_INFO("ai col set rowkey tags failed", K(ret));
      } else if (OB_FAIL(ObDMLService::init_das_dml_rtdef(dml_rtctx_, *ins_ctdef, *ins_rtdef, loc_meta))) {
        LOG_INFO("ai col init ins rtdef failed", K(ret));
      } else if (OB_FAIL(dml_rtctx_.das_ref_.prepare_das_task(tablet_loc, ins_op))) {
        LOG_INFO("ai col prepare ins task failed", K(ret));
      } else if (OB_FAIL(ins_op->init_task_info(ObDASWriteBuffer::DAS_ROW_DEFAULT_EXTEND_SIZE))) {
        LOG_INFO("ai col init task info failed", K(ret));
      } else {
        ins_op->set_das_ctdef(ins_ctdef);
        ins_op->set_das_rtdef(ins_rtdef);
        ins_rtdef->use_put_ = true;  // overlay semantics: a re-put replaces the stale task
        int64_t now_us = ObTimeUtility::current_time();
        int64_t pos_tid = ai_col_pos_in_ids(*task_ts, ins_ctdef->column_ids_, "table_id");
        int64_t pos_cid = ai_col_pos_in_ids(*task_ts, ins_ctdef->column_ids_, "column_id");
        int64_t pos_pred = ai_col_pos_in_ids(*task_ts, ins_ctdef->column_ids_, "rowkey_predicate");
        int64_t pos_req = ai_col_pos_in_ids(*task_ts, ins_ctdef->column_ids_, "request_id");
        int64_t pos_state = ai_col_pos_in_ids(*task_ts, ins_ctdef->column_ids_, "state");
        int64_t pos_retry = ai_col_pos_in_ids(*task_ts, ins_ctdef->column_ids_, "retry_count");
        int64_t pos_maxr = ai_col_pos_in_ids(*task_ts, ins_ctdef->column_ids_, "max_retries");
        int64_t pos_elig = ai_col_pos_in_ids(*task_ts, ins_ctdef->column_ids_, "eligible_ts");
        int64_t pos_gmtc = ai_col_pos_in_ids(*task_ts, ins_ctdef->column_ids_, "gmt_create");
        int64_t pos_gmtm = ai_col_pos_in_ids(*task_ts, ins_ctdef->column_ids_, "gmt_modified");
        if (pos_tid < 0 || pos_cid < 0 || pos_pred < 0 || pos_req < 0 || pos_state < 0
            || pos_retry < 0 || pos_maxr < 0 || pos_elig < 0 || pos_gmtc < 0 || pos_gmtm < 0) {
          LOG_INFO("ai col task column position missing", K(pos_tid), K(pos_cid), K(pos_pred));
          ret = OB_ERR_UNEXPECTED;
        }
        int64_t ntask_cols = task_ts->get_column_count();
        for (int64_t i = 0; OB_SUCC(ret) && i < ai_rowkey_preds_.count(); ++i) {
          const common::ObSqlString &pred = ai_rowkey_preds_.at(i);
          char rid_buf[96];
          int64_t rid_len = snprintf(rid_buf, sizeof(rid_buf), "%ld_%lu_%ld_%p",
              now_us, static_cast<unsigned long>(table_id),
              static_cast<long>(i), static_cast<const void *>(this));
          common::ObString rid(static_cast<int32_t>(rid_len), rid_buf);
          std::string apred(pred.ptr(), pred.length());
          std::string arid(rid.ptr(), rid.length());
          std::vector<uint64_t> acols;
          std::vector<std::string> ares;
          std::vector<std::string> aexpr;
          std::vector<int64_t> amr;
          for (int64_t j = 0; OB_SUCC(ret) && j < ai_meta_col_ids_.count(); ++j) {
            acols.push_back(ai_meta_col_ids_.at(j));
            ares.push_back(std::string(ai_meta_res_cols_.at(j).ptr(),
                                       ai_meta_res_cols_.at(j).length()));
            // Rebuild a valid, typed SQL expression for the worker UPDATE: the
            // persisted normalized_expr embeds an unquoted system prompt and is
            // not parseable. Use the prompt-object form with an optional
            // "system" field derived from the persisted result kind.
            const common::ObString &m_mdl = ai_meta_models_.at(j);
            const common::ObString &m_ft = ai_meta_ftypes_.at(j);
            const common::ObString &m_pexpr = ai_meta_prompt_exprs_.at(j);
            int64_t m_rt = ai_meta_rtypes_.at(j);
            const char *sys_txt = (1 == m_rt)
                ? "Return exactly one valid JSON value."
                : (2 == m_rt ? "Return exactly one base-10 integer." : NULL);
            common::ObSqlString ex;
            bool embed = (m_ft.length() == 5 && 0 == strncasecmp(m_ft.ptr(), "embed", 5));
            if (embed) {
              if (OB_SUCCESS != ex.assign_fmt("AI_EMBED('")) {
              } else {
                for (int64_t x = 0; OB_SUCCESS == ret && x < m_mdl.length(); ++x) {
                  ret = (m_mdl.ptr()[x] == '\'') ? ex.append("''") : ex.append(m_mdl.ptr() + x, 1);
                }
                if (OB_SUCCESS == ret) {
                  ret = ex.append("', ");
                }
              }
              if (OB_SUCCESS == ret) { ret = ex.append(m_pexpr.ptr(), m_pexpr.length()); }
              if (OB_SUCCESS == ret) { ret = ex.append(")"); }
            } else {
              if (OB_SUCCESS != ex.assign_fmt("AI_COMPLETE('")) {
              } else {
                for (int64_t x = 0; OB_SUCCESS == ret && x < m_mdl.length(); ++x) {
                  ret = (m_mdl.ptr()[x] == '\'') ? ex.append("''") : ex.append(m_mdl.ptr() + x, 1);
                }
                if (OB_SUCCESS == ret) {
                  ret = ex.append("', JSON_OBJECT('template', ");
                }
              }
              if (OB_SUCCESS == ret) { ret = ex.append(m_pexpr.ptr(), m_pexpr.length()); }
              if (OB_SUCCESS == ret) { ret = ex.append(", 'args', JSON_ARRAY()"); }
              if (OB_SUCCESS == ret && OB_NOT_NULL(sys_txt)) {
                ret = ex.append(", 'system', '");
                for (int64_t x = 0; OB_SUCCESS == ret && sys_txt[x] != 0; ++x) {
                  ret = (sys_txt[x] == '\'') ? ex.append("''") : ex.append(sys_txt + x, 1);
                }
                if (OB_SUCCESS == ret) {
                  ret = ex.append("')");
                }
              }
              if (OB_SUCCESS == ret) {
                // sys branch already closed JSON_OBJECT; without it we need
                // two closers (JSON_OBJECT + AI_COMPLETE)
                ret = ex.append(OB_NOT_NULL(sys_txt) ? ")" : "))");
              }
            }
            if (OB_SUCCESS != ret) {
              LOG_INFO("ai col worker expr build failed", K(ret));
              ret = OB_SUCCESS;  // keep the DML alive; skip this column
              continue;
            }
            LOG_INFO("AICOL_WORKER_EXPR", K(table_id), "expr", ex.ptr());
            aexpr.push_back(std::string(ex.ptr(), ex.length()));
            amr.push_back(ai_meta_max_retries_.at(j));
            blocksstable::ObDatumRow *row = nullptr;
            if (OB_FAIL(data_plane::create_datum_row(alloc, ntask_cols, row))) {
              LOG_INFO("ai col create datum row failed", K(ret));
            } else {
              for (int64_t ci = 0; OB_SUCC(ret) && ci < ntask_cols; ++ci) {
                if (ci == pos_tid) {
                  row->storage_datums_[ci].set_int(static_cast<int64_t>(table_id));
                } else if (ci == pos_cid) {
                  row->storage_datums_[ci].set_int(static_cast<int64_t>(ai_meta_col_ids_.at(j)));
                } else if (ci == pos_pred) {
                  row->storage_datums_[ci].set_string(pred.ptr(), static_cast<int32_t>(pred.length()));
                } else if (ci == pos_req) {
                  row->storage_datums_[ci].set_string(rid.ptr(), static_cast<int32_t>(rid.length()));
                } else if (ci == pos_state) {
                  row->storage_datums_[ci].set_int(0);
                } else if (ci == pos_retry) {
                  row->storage_datums_[ci].set_int(0);
                } else if (ci == pos_maxr) {
                  row->storage_datums_[ci].set_int(ai_meta_max_retries_.at(j));
                } else if (ci == pos_elig || ci == pos_gmtc || ci == pos_gmtm) {
                  row->storage_datums_[ci].set_int(now_us);
                } else {
                  row->storage_datums_[ci].set_null();
                }
              }
              if (OB_SUCC(ret)) {
                ObDASWriteBuffer::DmlShadowRow sr;
                int sret = sr.init(alloc, ins_ctdef->column_types_, true);
                if (OB_SUCCESS == sret) {
                  sret = sr.shadow_copy(*row);
                }
                if (OB_SUCCESS != sret) {
                  LOG_INFO("ai col build shadow row failed", K(sret));
                } else {
                  ObChunkDatumStore::StoredRow *stored = nullptr;
                  int aret = ins_op->get_insert_buffer().add_row(sr, &stored);
                  LOG_INFO("AICOL_TASK_ROW_ADD", K(aret), K(table_id),
                           "col", ai_meta_col_ids_.at(j), "pred", pred.ptr());
                }
              }
            }
          }
          if (OB_SUCC(ret)) {
            common::ObMySQLProxy *ai_proxy = ctx_.get_sql_proxy();
            ai_spawn_worker(ai_proxy, static_cast<int64_t>(table_id),
                            std::string(db_name.ptr(), db_name.length()),
                            std::string(tbl_name.ptr(), tbl_name.length()),
                            apred, arid, acols, ares, aexpr, amr);
          }
        }
      }
    }
    if (OB_FAIL(ret)) {
      LOG_INFO("ai col insert task rows failed", K(ret));
    }
    if (dml_rtctx_.das_ref_.has_task()) {
      int eret = dml_rtctx_.das_ref_.execute_all_task();
      int cret = dml_rtctx_.das_ref_.close_all_task();
      dml_rtctx_.das_ref_.reset();
      LOG_INFO("AICOL_DAS_EXECUTE_INS", K(eret), K(cret), K(table_id));
    }
  }
  // ---------- delete task rows of removed/renamed rows (same transaction) ----------
  if (OB_SUCC(ret) && !ai_del_preds_.empty()) {
    ObDASDelCtDef *del_ctdef =
        new (alloc.alloc(sizeof(ObDASDelCtDef))) ObDASDelCtDef(alloc);
    ObDASDelRtDef *del_rtdef = nullptr;
    ObDASDeleteOp *del_op = nullptr;
    if (OB_ISNULL(del_ctdef)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(ObDASTaskFactory::alloc_das_rtdef(DAS_OP_TABLE_DELETE, alloc, del_rtdef))) {
      LOG_INFO("ai col alloc del rtdef failed", K(ret));
    } else {
      del_ctdef->table_id_ = OB_AI_TASK_TABLE_ID;
      del_ctdef->schema_version_ = schema_ver;
      del_ctdef->rowkey_cnt_ = task_ts->get_rowkey_column_num();
      del_ctdef->spk_cnt_ = 0;
      int64_t nrk = task_ts->get_rowkey_column_num();
      if (OB_FAIL(del_ctdef->column_ids_.reserve(nrk))
          || OB_FAIL(del_ctdef->column_types_.reserve(nrk))
          || OB_FAIL(del_ctdef->column_accuracys_.reserve(nrk))
          || OB_FAIL(del_ctdef->old_row_projector_.reserve(nrk))) {
        LOG_INFO("ai col reserve del arrays failed", K(ret));
      }
      common::ObArray<uint64_t> rk_ids;
      if (OB_FAIL(task_ts->get_rowkey_column_ids(rk_ids))) {
        LOG_INFO("ai col get task rowkey ids failed", K(ret));
      }
      for (int64_t ri = 0; OB_SUCC(ret) && ri < rk_ids.count(); ++ri) {
        const share::schema::ObColumnSchemaV2 *col = task_ts->get_column_schema(rk_ids.at(ri));
        if (OB_ISNULL(col)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          common::ObObjMeta m;
          m.set_type(col->get_data_type());
          m.set_collation_type(col->get_collation_type());
          if (OB_FAIL(del_ctdef->column_ids_.push_back(col->get_column_id()))
              || OB_FAIL(del_ctdef->column_types_.push_back(m))
              || OB_FAIL(del_ctdef->column_accuracys_.push_back(col->get_accuracy()))
              || OB_FAIL(del_ctdef->old_row_projector_.push_back(static_cast<int32_t>(ri)))) {
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(del_ctdef->table_param_.build(task_ts, schema_ver, del_ctdef->column_ids_))) {
        LOG_INFO("ai col build del table param failed", K(ret));
      } else if (OB_FAIL(del_ctdef->table_param_.set_data_table_rowkey_tags(guard, task_ts))) {
        LOG_INFO("ai col set del rowkey tags failed", K(ret));
      } else if (OB_FAIL(ObDMLService::init_das_dml_rtdef(dml_rtctx_, *del_ctdef, *del_rtdef, loc_meta))) {
        LOG_INFO("ai col init del rtdef failed", K(ret));
      } else if (OB_FAIL(dml_rtctx_.das_ref_.prepare_das_task(tablet_loc, del_op))) {
        LOG_INFO("ai col prepare del task failed", K(ret));
      } else if (OB_FAIL(del_op->init_task_info(ObDASWriteBuffer::DAS_ROW_DEFAULT_EXTEND_SIZE))) {
        LOG_INFO("ai col init del task info failed", K(ret));
      } else {
        del_op->set_das_ctdef(del_ctdef);
        del_op->set_das_rtdef(del_rtdef);
        int64_t rk_cnt = del_ctdef->column_ids_.count();
        for (int64_t i = 0; OB_SUCC(ret) && i < ai_del_preds_.count(); ++i) {
          const common::ObSqlString &pred = ai_del_preds_.at(i);
          for (int64_t j = 0; OB_SUCC(ret) && j < ai_meta_col_ids_.count(); ++j) {
            blocksstable::ObDatumRow *row = nullptr;
            if (OB_FAIL(data_plane::create_datum_row(alloc, rk_cnt, row))) {
              LOG_INFO("ai col create del datum row failed", K(ret));
            } else {
              for (int64_t ci = 0; OB_SUCC(ret) && ci < rk_cnt; ++ci) {
                uint64_t cid = del_ctdef->column_ids_.at(ci);
                const share::schema::ObColumnSchemaV2 *col = task_ts->get_column_schema(cid);
                const ObString &cname = col->get_column_name_str();
                if (0 == strcasecmp(cname.ptr(), "table_id")) {
                  row->storage_datums_[ci].set_int(static_cast<int64_t>(table_id));
                } else if (0 == strcasecmp(cname.ptr(), "column_id")) {
                  row->storage_datums_[ci].set_int(static_cast<int64_t>(ai_meta_col_ids_.at(j)));
                } else if (0 == strcasecmp(cname.ptr(), "rowkey_predicate")) {
                  row->storage_datums_[ci].set_string(pred.ptr(), static_cast<int32_t>(pred.length()));
                } else {
                  row->storage_datums_[ci].set_null();
                }
              }
              if (OB_SUCC(ret)) {
                ObDASWriteBuffer::DmlShadowRow sr;
                int sret = sr.init(alloc, del_ctdef->column_types_, true);
                if (OB_SUCCESS == sret) {
                  sret = sr.shadow_copy(*row);
                }
                if (OB_SUCCESS != sret) {
                  LOG_INFO("ai col build del shadow row failed", K(sret));
                } else {
                  ObChunkDatumStore::StoredRow *stored = nullptr;
                  int aret = del_op->get_write_buffer().add_row(sr, &stored);
                  LOG_INFO("AICOL_TASK_ROW_DEL", K(aret), K(table_id),
                           "col", ai_meta_col_ids_.at(j), "pred", pred.ptr());
                }
              }
            }
          }
        }
      }
    }
    if (OB_FAIL(ret)) {
      LOG_INFO("ai col delete task rows failed", K(ret));
    }
    // Delete failures (e.g. the task row was already drained by the worker)
    // are tolerated: they must never break the user DML.
    if (dml_rtctx_.das_ref_.has_task()) {
      int eret = dml_rtctx_.das_ref_.execute_all_task();
      int cret = dml_rtctx_.das_ref_.close_all_task();
      dml_rtctx_.das_ref_.reset();
      LOG_INFO("AICOL_DAS_EXECUTE_DEL", K(eret), K(cret), K(table_id));
    }
  }
  return OB_SUCCESS;  // reconciliation failures must never break the user DML
}


void ObTableModifyOp::clear_dml_evaluated_flag(ObExpr *clear_expr)
{
  if (clear_expr->is_batch_result()) {
    clear_expr->get_evaluated_flags(eval_ctx_).unset(eval_ctx_.get_batch_idx());
  } else {
    clear_expr->get_eval_info(eval_ctx_).clear_evaluated_flag();
  }
  if (0 < clear_expr->parent_cnt_) {
    clear_dml_evaluated_flag(clear_expr->parent_cnt_, clear_expr->parents_);
  }
}

void ObTableModifyOp::clear_dml_evaluated_flag(int64_t parent_cnt, ObExpr **parent_exprs)
{
  for (int64_t i = 0; i < parent_cnt; ++i) {
    clear_dml_evaluated_flag(parent_exprs[i]);
  }
}

void ObTableModifyOp::clear_dml_evaluated_flag()
{
  // Clear the parent expr because the assignment expr may be changed
  for (int64_t i = 0; i < trigger_clear_exprs_.count(); ++i) {
    ObExpr *expr = trigger_clear_exprs_.at(i);
    clear_dml_evaluated_flag(expr);
  }
}

ObDasParallelType ObTableModifyOp::check_das_parallel_type()
{
  ObDasParallelType type = DAS_SERIALIZATION;
  // Local DAS modification runs serially.
  return type;
}

OB_INLINE int ObTableModifyOp::init_das_dml_ctx()
{
  ObSQLSessionInfo *session = GET_MY_SESSION(ctx_);
  const char *label = nullptr;
  switch(MY_SPEC.type_) {
  case PHY_INSERT:
    label = "InsDASCtx";
    break;
  case PHY_UPDATE:
    label = "UpdDASCtx";
    break;
  case PHY_DELETE:
    label = "DelDASCtx";
    break;
  case PHY_LOCK:
    label = "LockDASCtx";
    break;
  default:
    label = "DmlDASCtx";
    break;
  }
  ObMemAttr memattr(label, ObCtxIds::EXECUTE_CTX_ID);
  dml_rtctx_.das_ref_.set_expr_frame_info(nullptr != MY_SPEC.expr_frame_info_
                                        ? MY_SPEC.expr_frame_info_
                                        : &MY_SPEC.plan_->get_expr_frame_info());
  dml_rtctx_.das_ref_.set_mem_attr(memattr);
  dml_rtctx_.das_ref_.get_das_parallel_ctx().set_das_parallel_type(check_das_parallel_type());
  dml_rtctx_.das_ref_.get_das_parallel_ctx().set_das_dop(ctx_.get_das_ctx().get_real_das_dop());
  if (check_das_parallel_type() != DAS_SERIALIZATION) {
    LOG_TRACE("this sql use das parallel submit", K(check_das_parallel_type()));
  }
  return OB_SUCCESS;
}

int ObTableModifyOp::replace_implict_cursor(int64_t affected_rows,
                                            int64_t found_rows,
                                            int64_t matched_rows,
                                            int64_t duplicated_rows)
{
  int ret = OB_SUCCESS;
  bool is_ins_val_opt = ctx_.get_sql_ctx()->is_do_insert_batch_opt();
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
  if (MY_SPEC.ab_stmt_id_ != nullptr && !is_ins_val_opt) {
    ObDatum *stmt_id_datum = nullptr;
    ObImplicitCursorInfo implicit_cursor;
    if (OB_FAIL(prepare_implict_cursor(affected_rows,
                                       found_rows,
                                       matched_rows,
                                       duplicated_rows,
                                       implicit_cursor))) {
    } else if (OB_FAIL(plan_ctx->replace_implicit_cursor_info(implicit_cursor))) {
    }
  }
  return ret;
}

int ObTableModifyOp::prepare_implict_cursor(int64_t affected_rows,
                                            int64_t found_rows,
                                            int64_t matched_rows,
                                            int64_t duplicated_rows,
                                            ObImplicitCursorInfo &implicit_cursor)
{
  int ret = OB_SUCCESS;
  ObDatum *stmt_id_datum = nullptr;
  if (OB_FAIL(MY_SPEC.ab_stmt_id_->eval(eval_ctx_, stmt_id_datum))) {
  } else {
    ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
    int64_t stmt_id = stmt_id_datum->get_int();
    implicit_cursor.stmt_id_ = stmt_id;
    implicit_cursor.found_rows_ += found_rows;
    implicit_cursor.matched_rows_ += matched_rows;
    implicit_cursor.affected_rows_ += affected_rows;
    implicit_cursor.duplicated_rows_ += duplicated_rows;
    implicit_cursor.last_insert_id_ = plan_ctx->get_autoinc_col_value();
  }
  return ret;
}

int ObTableModifyOp::merge_implict_cursor(int64_t affected_rows,
                                          int64_t found_rows,
                                          int64_t matched_rows,
                                          int64_t duplicated_rows)
{
  int ret = OB_SUCCESS;
  bool is_ins_val_opt = ctx_.get_sql_ctx()->is_do_insert_batch_opt();
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
  if (MY_SPEC.ab_stmt_id_ != nullptr && !is_ins_val_opt) {
    ObDatum *stmt_id_datum = nullptr;
    ObImplicitCursorInfo implicit_cursor;
    if (OB_FAIL(prepare_implict_cursor(affected_rows,
                                       found_rows,
                                       matched_rows,
                                       duplicated_rows,
                                       implicit_cursor))) {
    } else if (OB_FAIL(plan_ctx->merge_implicit_cursor_info(implicit_cursor))) {
    }
  }
  return ret;
}

int ObTableModifyOp::inner_switch_iterator()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_switch_iterator())) {
  }

  return ret;
}

int ObTableModifyOp::inner_close()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = ctx_.get_physical_plan_ctx();
  // release cache_handle for auto-increment
  share::ObAutoincrementService &auto_service = share::ObAutoincrementService::get_instance();
  ObIArray<share::AutoincParam> &autoinc_params = plan_ctx->get_autoinc_params();
  for (int64_t i = 0; i < autoinc_params.count(); ++i) {
    if (NULL != autoinc_params.at(i).cache_handle_) {
      auto_service.release_handle(autoinc_params.at(i).cache_handle_);
    }
  }
  if (dml_rtctx_.das_ref_.has_task()) {
    if (OB_FAIL(dml_rtctx_.das_ref_.close_all_task())) {
    } else {
      dml_rtctx_.das_ref_.reset();
    }
  }
  // AI column task reconciliation inside the user transaction (nested session).
  if (OB_SUCC(ret)) {
    const ObDMLBaseCtDef *ai_ctdef = nullptr;
    int gret = MY_SPEC.get_single_dml_ctdef(ai_ctdef);
    if (OB_SUCCESS == gret && OB_NOT_NULL(ai_ctdef)) {
      uint64_t ai_table_id = ai_ctdef->das_base_ctdef_.table_param_.get_data_table().get_table_id();
      // gate: only tables that actually declare an AI column need reconciliation
      bool need_ai_reconcile = false;
      share::schema::ObSchemaGetterGuard *ai_guard =
          (OB_NOT_NULL(ctx_.get_sql_ctx())) ? ctx_.get_sql_ctx()->schema_guard_ : nullptr;
      const share::schema::ObTableSchema *ai_table_schema = nullptr;
      if (OB_NOT_NULL(ai_guard)
          && OB_SUCCESS == ai_guard->get_table_schema(ai_table_id, ai_table_schema)
          && OB_NOT_NULL(ai_table_schema)) {
        for (int64_t ci = 0; ci < ai_table_schema->get_column_count(); ++ci) {
          const share::schema::ObColumnSchemaV2 *ai_col =
              ai_table_schema->get_column_schema_by_idx(ci);
          if (OB_NOT_NULL(ai_col)
              && ai_col->get_column_name_str().prefix_match(OB_AI_STATE_COL_PREFIX)) {
            need_ai_reconcile = true;
            break;
          }
        }
        LOG_INFO("AICOL_GATE_DBG", K(ai_table_id), "col_cnt", ai_table_schema->get_column_count(),
                 "has_ai", ai_table_schema->has_ai_column(), K(need_ai_reconcile));
      }
      if (need_ai_reconcile) {
        int ai_ret = ai_reconcile_das(ai_table_id);
        LOG_INFO("AICOL_RECONCILE_DONE", K(ai_ret), K(ai_table_id), "npred", ai_rowkey_preds_.count(), "ndel", ai_del_preds_.count());
      } else {
        LOG_INFO("AICOL_GATE_SKIP", K(ai_table_id), "sql_ctx_null", OB_ISNULL(ctx_.get_sql_ctx()),
                 "guard_null", OB_ISNULL(ai_guard), "schema_null", OB_ISNULL(ai_table_schema));
      }
    }
  }
  close_inner_conn();
  dml_modify_rows_.clear();
  return ret;
}

// Build the rowkey predicate string ("`id` = 1") from a stored row. Returns an
// empty pred when the row or a rowkey value is unusable.
namespace {
int ai_build_rowkey_pred(const share::schema::ObTableSchema &ts,
                         const ObDMLBaseCtDef &ctdef,
                         const ObChunkDatumStore::StoredRow *row,
                         common::ObSqlString &pred)
{
  int ret = OB_SUCCESS;
  common::ObArray<uint64_t> rk_ids;
  bool any = false;
  bool ok = true;
  if (OB_ISNULL(row)) {
    // no row for this event
  } else if (OB_FAIL(ts.get_rowkey_column_ids(rk_ids))) {
    LOG_WARN("ai col get rowkey ids failed", K(ret));
  } else {
    for (int64_t i = 0; ok && OB_SUCC(ret) && i < rk_ids.count(); ++i) {
      int64_t pos = -1;
      for (int64_t j = 0; j < ctdef.column_ids_.count(); ++j) {
        if (ctdef.column_ids_.at(j) == rk_ids.at(i)) { pos = j; break; }
      }
      if (pos < 0 || pos >= row->cnt_) { continue; }
      const common::ObDatum *d = row->cells() + pos;
      const share::schema::ObColumnSchemaV2 *col = ts.get_column_schema(rk_ids.at(i));
      if (OB_ISNULL(col)) { continue; }
      if (any && OB_FAIL(pred.append_fmt(" AND "))) { }
      any = true;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(pred.append_fmt("`%.*s` = ",
              static_cast<int>(col->get_column_name_str().length()),
              col->get_column_name_str().ptr()))) {
      } else if (d->is_null()) {
        ok = false;
      } else if (ob_is_int_tc(col->get_data_type())) {
        if (OB_FAIL(pred.append_fmt("%ld", d->get_int()))) { }
      } else if (col->get_data_type() == ObVarcharType || col->get_data_type() == ObCharType) {
        common::ObString sv = d->get_string();
        if (OB_FAIL(pred.append_fmt("'%.*s'",
                static_cast<int>(sv.length()), sv.ptr()))) { }
      } else {
        LOG_INFO("AICOL_CAPTURE unsupported rowkey type");
        ok = false;
      }
    }
  }
  if (OB_FAIL(ret) || !ok || !any) {
    pred.reset();
    ret = OB_SUCCESS;
  }
  return ret;
}
}  // anonymous namespace

int ObTableModifyOp::ai_capture_rowkey(const ObDMLModifyRowNode &node)
{
  int ret = OB_SUCCESS;
  if (!ai_capture_on_) {
    return ret;
  }
  LOG_INFO("AICOL_CAPTURE_ENTER", "event", static_cast<int>(node.dml_event_),
           "has_new", OB_NOT_NULL(node.new_row_), "has_old", OB_NOT_NULL(node.old_row_),
           "gate", ai_dep_gate_);
  const ObDMLBaseCtDef &ctdef = *node.dml_ctdef_;
  ObDmlEventType event = node.dml_event_;
  share::schema::ObSchemaGetterGuard *guard =
      (OB_NOT_NULL(ctx_.get_sql_ctx())) ? ctx_.get_sql_ctx()->schema_guard_ : nullptr;
  uint64_t tid = ctdef.das_base_ctdef_.table_param_.get_data_table().get_table_id();
  const share::schema::ObTableSchema *ts = nullptr;
  if (OB_ISNULL(guard)) {
    LOG_WARN("AICOL_CAPTURE guard null");
  } else if (OB_SUCCESS != guard->get_table_schema(tid, ts) || OB_ISNULL(ts)) {
    LOG_WARN("AICOL_CAPTURE get table schema failed", K(tid));
  } else {
    common::ObSqlString new_pred;
    common::ObSqlString old_pred;
    common::ObSqlString frozen;
    bool has_new = false;
    bool has_old = false;
    // the plain UPDATE path fills only full_row_ (new values for the updated
    // columns); the rowkey columns in it are the new rowkey
    const ObChunkDatumStore::StoredRow *new_row_src =
        (OB_NOT_NULL(node.new_row_)) ? node.new_row_
        : (ObDmlEventType::DE_UPDATING == event ? node.full_row_ : nullptr);
    if (ObDmlEventType::DE_DELETING != event) {
      if (OB_SUCCESS == ai_build_rowkey_pred(*ts, ctdef, new_row_src, new_pred)
          && new_pred.length() > 0) {
        has_new = true;
      }
    }
    if (ObDmlEventType::DE_DELETING == event || ObDmlEventType::DE_UPDATING == event) {
      if (OB_SUCCESS == ai_build_rowkey_pred(*ts, ctdef, node.old_row_, old_pred)
          && old_pred.length() > 0) {
        has_old = true;
      }
    }
    if (ObDmlEventType::DE_DELETING == event) {
      if (has_old) {
        frozen.assign(old_pred.ptr(), old_pred.length());
        if (OB_SUCCESS != ai_del_preds_.push_back(frozen)) { }
      }
    } else if (ObDmlEventType::DE_UPDATING == event) {
      if (ai_dep_gate_ && has_new) {
        frozen.assign(new_pred.ptr(), new_pred.length());
        if (OB_SUCCESS != ai_rowkey_preds_.push_back(frozen)) { }
      }
      if (has_old && (!has_new || old_pred.length() != new_pred.length()
                      || 0 != memcmp(old_pred.ptr(), new_pred.ptr(), old_pred.length()))) {
        // rowkey changed: the task keyed by the old predicate must go away
        frozen.assign(old_pred.ptr(), old_pred.length());
        if (OB_SUCCESS != ai_del_preds_.push_back(frozen)) { }
      }
    } else {  // inserting
      if (has_new) {
        frozen.assign(new_pred.ptr(), new_pred.length());
        if (OB_SUCCESS != ai_rowkey_preds_.push_back(frozen)) { }
      }
    }
    if (ai_rowkey_preds_.count() + ai_del_preds_.count() > 0) {
      LOG_INFO("AICOL_CAPTURE_OK", "npred", ai_rowkey_preds_.count(),
               "ndel", ai_del_preds_.count(), "event", static_cast<int>(event));
    }
  }
  return ret;
}

OB_INLINE int ObTableModifyOp::init_foreign_key_operation()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *phy_plan_ctx = GET_PHY_PLAN_CTX(ctx_);
  if (OB_ISNULL(phy_plan_ctx)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!phy_plan_ctx->need_foreign_key_checks()) {
    // set fk check even if ret != OB_SUCCESS. see ObTableModifyOp::inner_close()
    need_foreign_key_check_ = false;
  } else {
    need_foreign_key_check_ = MY_SPEC.need_foreign_key_check_;
  }
  return ret;
}

int ObTableModifyOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    iter_end_ = false;
    if (dml_rtctx_.das_ref_.has_task()) {
      if (OB_FAIL(dml_rtctx_.das_ref_.close_all_task())) {
      } else {
        dml_rtctx_.reuse();
      }
    }
  }
  if (OB_SUCC(ret)) {
    dml_modify_rows_.clear();
    if (OB_FAIL(calc_single_table_loc())) {
    }
  }
  return ret;
}

int ObTableModifyOp::check_need_exec_single_row() {
  int ret = OB_SUCCESS;
  if (MY_SPEC.is_returning_ && (need_foreign_key_checks() || MY_SPEC.need_trigger_fire_)) {
    execute_single_row_ = true;
  }
  return ret;
}

int ObTableModifyOp::get_gi_task()
{
  int ret = OB_SUCCESS;
  ObGranuleTaskInfo gi_task_info;
  GIPrepareTaskMap *gi_prepare_map = nullptr;
  ObDASCtx &das_ctx = ctx_.get_das_ctx();
  ObTableID table_loc_id = OB_INVALID_ID;
  ObTableID ref_table_id = OB_INVALID_ID;
  ObTabletID tablet_id;
  if (OB_FAIL(MY_SPEC.get_single_table_loc_id(table_loc_id, ref_table_id))) {
  } else if (OB_FAIL(ctx_.get_gi_task_map(gi_prepare_map))) {
  } else if (OB_FAIL(gi_prepare_map->get_refactored(MY_SPEC.id_, gi_task_info))) {
    if (ret != OB_HASH_NOT_EXIST) {
    } else {
      LOG_DEBUG("no prepared task info, set table modify to end",
        K(MY_SPEC.get_id()), K(this), K(lbt()));
      // Current DML operator cannot obtain task from GI, indicating current DML operator iter end
      iter_end_ = true;
      ret = OB_SUCCESS;
    }
  } else if (OB_ISNULL(get_input()->table_loc_) &&
      OB_ISNULL(get_input()->table_loc_ = das_ctx.get_table_loc_by_id(table_loc_id, ref_table_id))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    get_input()->tablet_loc_ = gi_task_info.tablet_loc_;
    LOG_DEBUG("DML operator consume a task", K(ret),
              K(gi_task_info), KPC(get_input()), K(MY_SPEC.id_), K(get_input()));
  }
  return ret;
}

int ObTableModifyOp::calc_single_table_loc()
{
  int ret = OB_SUCCESS;
  if (!MY_SPEC.use_dist_das_) {
    ObTableID table_loc_id = OB_INVALID_ID;
    ObTableID ref_table_id = OB_INVALID_ID;
    ObDASCtx &das_ctx = ctx_.get_das_ctx();
    ObDASTableLoc *table_loc = nullptr;
    if (OB_UNLIKELY(MY_SPEC.gi_above_)) {
      if (OB_FAIL(get_gi_task())) {
      }
    } else if (OB_FAIL(MY_SPEC.get_single_table_loc_id(table_loc_id, ref_table_id))) {
    } else if (OB_ISNULL(table_loc = das_ctx.get_table_loc_by_id(table_loc_id, ref_table_id))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      get_input()->table_loc_ = table_loc;
      get_input()->tablet_loc_ = table_loc->get_first_tablet_loc();
    }
  }
  return ret;
}

int ObTableModifyOp::open_inner_conn()
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = NULL;
  if (OB_ISNULL(sql_proxy_ = ctx_.get_sql_proxy())) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(session = ctx_.get_my_session())) {
    ret = OB_NOT_INIT;
  } else if (NULL != session->get_inner_conn()) {
    // do nothing.
  } else if (OB_FAIL(
                 query::ObInnerSQLConnectionAccess::
                     create_connection_with_external_session(
                         session, inner_conn_guard_))) {
  } else if (OB_ISNULL(inner_conn_guard_.get_ptr())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    /**
     * session is the only data struct which can pass through multi layer nested sql,
     * so we put inner conn in session to share it within multi layer nested sql.
     */
    session->set_inner_conn(inner_conn_guard_.get_ptr());
    need_close_conn_ = true;
  }
  if (OB_SUCC(ret)) {
    inner_conn_ = as_inner_sql_connection(session->get_inner_conn());
    if (OB_ISNULL(inner_conn_)) {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

int ObTableModifyOp::close_inner_conn()
{
  /**
   * we can call it even if open_inner_conn() failed, because only the one who call
   * open_inner_conn() succeed first will do close job by "if (need_close_conn_)".
   */
  int ret = OB_SUCCESS;
  if (need_close_conn_) {
    ObSQLSessionInfo *session = ctx_.get_my_session();
    if (OB_ISNULL(sql_proxy_) || OB_ISNULL(session)) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      OX(session->set_inner_conn(NULL));
      OX(inner_conn_guard_.reset());
    }
    need_close_conn_ = false;
  }
  sql_proxy_ = NULL;
  inner_conn_guard_.reset();
  inner_conn_ = NULL;
  return ret;
}

int ObTableModifyOp::begin_nested_session(bool skip_cur_stmt_tables)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(inner_conn_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(inner_conn_->begin_nested_session(get_saved_session(), saved_conn_,
                                                       skip_cur_stmt_tables))) {
  }
  return ret;
}

int ObTableModifyOp::end_nested_session()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(inner_conn_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(inner_conn_->end_nested_session(get_saved_session(), saved_conn_))) {
  }
  return ret;
}
int ObTableModifyOp::execute_write(const char *sql)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  if (OB_ISNULL(inner_conn_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(sql)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(inner_conn_->execute_write(sql, affected_rows))) {
  }
  return ret;
}

int ObTableModifyOp::execute_read(const char *sql, ObMySQLProxy::MySQLResult &res)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(inner_conn_) || OB_ISNULL(sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(sql)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(inner_conn_->execute_read(sql, res))) {
  }
 return ret;
}

int ObTableModifyOp::check_stack()
{
  int ret = OB_SUCCESS;
  const int max_stack_deep = 16;
  bool is_stack_overflow = false;
  ObSQLSessionInfo *session = ctx_.get_my_session();
  if (OB_ISNULL(session)) {
    ret = OB_ERR_NULL_VALUE;
  } else if (session->get_nested_count() > max_stack_deep) {
    ret = OB_SIZE_OVERFLOW;
  } else if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
  }
  return ret;
}
OperatorOpenOrder ObTableModifyOp::get_operator_open_order() const
{
  OperatorOpenOrder open_order = OPEN_CHILDREN_FIRST;
  if (spec_.plan_->is_use_px()) {
    // If PX is enabled in DML, there are two scenarios:
    // 1. PDML: in pdml, pdml-op completely does not need its corresponding input to provide runtime parameters, so it will directly return
    // open_order = OPEN_CHILDREN_FIRST
    // 2. DML+PX: In this case, the parameters of the dml run are completely fed by the GI operator above it, for example, such a plan:
    //   PX COORD
    //    PX TRANSMIT
    //      GI (FULL PARTITION WISE)
    //       DELETE
    //        TSC
    //  Such a plan requires GI to drive the deletion, GI rescan delete each time a new Task is iterated,
    //  Pass the information of the corresponding Task to the delete operator.
    open_order = OPEN_CHILDREN_FIRST;
  }
  return open_order;
}

int ObTableModifyOp::submit_all_dml_task()
{
  int ret = OB_SUCCESS;
  if (dml_rtctx_.das_ref_.has_task()) {
    if (dml_rtctx_.need_pick_del_task_first() &&
                OB_FAIL(dml_rtctx_.das_ref_.pick_del_task_to_first())) {
    } else if (OB_FAIL(dml_rtctx_.das_ref_.execute_all_task())) {
    } else if (OB_FAIL(dml_rtctx_.das_ref_.close_all_task())) {
    } else if (OB_FAIL(ObDMLService::handle_after_row_processing(this, &get_dml_modify_row_list()))) {
    } else {
      dml_modify_rows_.clear();
      dml_rtctx_.reuse();
    }
  }
  return ret;
}

//The data to be written by DML will be buffered in the DAS Write Buffer
//When the buffer data exceeds 6M,
//needs to be written to the storage to release the memory.
int ObTableModifyOp::discharge_das_write_buffer()
{
  int ret = OB_SUCCESS;
  if (dml_rtctx_.need_submit_all_tasks()) {
    LOG_INFO("DASWriteBuffer full, now to write storage",
             "buffer memory", dml_rtctx_.das_ref_.get_das_alloc().used(),
             K(dml_rtctx_.get_das_task_memory_size()), K(dml_rtctx_.get_das_parallel_task_size()));
    ret = submit_all_dml_task();
  } else if (execute_single_row_) {
    if (REACH_COUNT_INTERVAL(100)) { // print log per 100 times.
    }
    ret = submit_all_dml_task();
  }
  return ret;
}

int ObTableModifyOp::get_next_row_from_child()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (OB_FAIL(child_->get_next_row())) {
    if (OB_ITER_END != ret) {
    }
  } else {
    LOG_DEBUG("child output row", "row", ROWEXPR2STR(eval_ctx_, child_->get_spec().output_));
  }
  return ret;
}

int ObTableModifyOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  if (iter_end_) {
    ret = OB_ITER_END;
  } else {
    int64_t row_count = 0;
    while (OB_SUCC(ret)) {
      if (OB_FAIL(try_check_status())) {
      } else if (OB_FAIL(get_next_row_from_child())) {
        if (OB_ITER_END != ret) {
        } else {
          iter_end_ = true;
          ret = OB_SUCCESS;
          break;
        }
      } else if (OB_FAIL(write_row_to_das_buffer())) {
      } else if (OB_FAIL(discharge_das_write_buffer())) {
      } else if (MY_SPEC.is_returning_) {
        break;
      }
      row_count ++;
    }

    if (OB_FAIL(ret)) {
      record_err_for_load_data(ret, row_count);
    }

    if (OB_SUCC(ret) && iter_end_ && dml_rtctx_.das_ref_.has_task()) {
      //DML operator reach iter end,
      //now submit the remaining rows in the DAS Write Buffer to the storage
      if (OB_FAIL(submit_all_dml_task())) {
      }
    }
    //to post process the DML info after writing all data to the storage or returning one row
    ret = write_rows_post_proc(ret);
    if (OB_SUCC(ret) && iter_end_) {
      ret = OB_ITER_END;
    }
  }
  return ret;
}

int ObTableModifyOp::perform_batch_fk_check()
{
  DEBUG_SYNC(BEFORE_FOREIGN_KEY_CONSTRAINT_CHECK);
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < fk_checkers_.count(); ++i) {
    bool all_has_result = false;
    ObForeignKeyChecker *fk_checker = fk_checkers_.at(i);
    if (OB_ISNULL(fk_checker)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(fk_checker->do_fk_check_batch(all_has_result))) {
    } else if (!all_has_result) {
      ret = OB_ERR_NO_REFERENCED_ROW;
    }
  }
  return ret;
}
}  // namespace sql
}  // namespace oceanbase
