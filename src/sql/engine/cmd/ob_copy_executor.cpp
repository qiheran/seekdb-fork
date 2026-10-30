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

#define USING_LOG_PREFIX SQL_EXE

#include "sql/engine/cmd/ob_copy_executor.h"
#include <stdio.h>
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/printer/ob_select_stmt_printer.h"
#include "query/api/query/session/ob_inner_sql_connection_access.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/mysqlclient/ob_isql_client.h"
#include "lib/string/ob_sql_string.h"
#include "lib/file/ob_file.h"

namespace oceanbase
{
using namespace common;
using namespace common::sqlclient;
namespace sql
{

static int csv_append_field(ObSqlString &line, const ObObj &obj, const ObString &nullstr)
{
  int ret = OB_SUCCESS;
  if (obj.is_null()) {
    OZ (line.append(nullstr));
  } else if (obj.is_varchar_or_char() || obj.is_lob_storage()) {
    ObString s = obj.get_string();
    // minimal quoting: quote when field contains , " \n \r or equals nullstr
    bool need_quote = false;
    for (int32_t i = 0; !need_quote && i < s.length(); ++i) {
      if (s.ptr()[i] == ',' || s.ptr()[i] == '"' || s.ptr()[i] == '\n' || s.ptr()[i] == '\r') {
        need_quote = true;
      }
    }
    if (!need_quote && nullstr.length() > 0 && s.length() == nullstr.length() &&
        0 == s.case_compare(nullstr)) {
      need_quote = true;
    }
    if (need_quote) {
      OZ (line.append('"'));
      for (int32_t i = 0; OB_SUCC(ret) && i < s.length(); ++i) {
        if (s.ptr()[i] == '"') {
          OZ (line.append('"'));
        }
        char c[2] = {s.ptr()[i], '\0'};
        OZ (line.append(c));
      }
      if (OB_SUCC(ret)) {
        OZ (line.append('"'));
      }
    } else {
      OZ (line.append(s));
    }
  } else {
    char buf[64];
    int64_t pos = 0;
    if (obj.is_int()) {
      databuff_printf(buf, sizeof(buf), pos, "%ld", obj.get_int());
    } else if (obj.is_uint64()) {
      databuff_printf(buf, sizeof(buf), pos, "%lu", obj.get_uint64());
    } else if (obj.is_double()) {
      databuff_printf(buf, sizeof(buf), pos, "%.15g", obj.get_double());
    } else if (obj.is_float()) {
      databuff_printf(buf, sizeof(buf), pos, "%.9g", static_cast<double>(obj.get_float()));
    } else if (obj.is_tinyint() || obj.is_smallint() || obj.is_int32()) {
      databuff_printf(buf, sizeof(buf), pos, "%ld", obj.get_int());
    } else {
      // fall back to obj to string
      int64_t len = obj.to_string(buf, sizeof(buf));
      pos = len;
    }
    OZ (line.append(ObString(static_cast<int32_t>(pos), buf)));
  }
  return ret;
}

int ObCopyExecutor::execute(ObExecContext &ctx, ObCopyStmt &stmt)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *my_session = ctx.get_my_session();
  ObPhysicalPlanCtx *plan_ctx = ctx.get_physical_plan_ctx();
  CK (OB_NOT_NULL(my_session));
  CK (OB_NOT_NULL(plan_ctx));
  CK (OB_NOT_NULL(stmt.get_select_stmt()));
  CK (OB_NOT_NULL(ctx.get_sql_ctx()));
  // print inner select sql
  ObArenaAllocator printer_alloc("CopyPrint");
  char *buf = NULL;
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(buf = static_cast<char *>(printer_alloc.alloc(OB_MAX_SQL_LENGTH)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    int64_t pos = 0;
    ObObjPrintParams print_params(my_session->get_timezone_info(),
                                  my_session->get_actual_collation_connection());
    ObSelectStmtPrinter printer(buf, OB_MAX_SQL_LENGTH, &pos, stmt.get_select_stmt(),
                                ctx.get_sql_ctx()->schema_guard_,
                                print_params,
                                &plan_ctx->get_param_store(),
                                false);
    OZ (printer.do_print());
    if (OB_SUCC(ret)) {
      buf[pos] = '\0';
    }
  }
  // execute via inner connection
  ObISQLConnectionGuard conn_guard;
  ObISQLConnection *conn = NULL;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObInnerSQLConnectionAccess::create_connection_with_external_session(my_session, conn_guard))) {
    LOG_WARN("create inner connection failed", K(ret));
  } else if (OB_ISNULL(conn = conn_guard.get_ptr())) {
    ret = OB_INNER_STAT_ERROR;
  } else {
    OZ (write_csv(ctx, stmt, buf));
  }
  return ret;
}

int ObCopyExecutor::write_csv(ObExecContext &ctx, ObCopyStmt &stmt, const char *sql)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *my_session = ctx.get_my_session();
  ObISQLConnectionGuard conn_guard;
  ObISQLConnection *conn = NULL;
  ObISQLClient::ReadResult read_res;
  ObMySQLResult *result = NULL;
  ObSelectStmt *select_stmt = stmt.get_select_stmt();
  CK (OB_NOT_NULL(my_session));
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObInnerSQLConnectionAccess::create_connection_with_external_session(my_session, conn_guard))) {
    LOG_WARN("create inner connection failed", K(ret));
  } else if (OB_ISNULL(conn = conn_guard.get_ptr())) {
    ret = OB_INNER_STAT_ERROR;
  } else if (OB_FAIL(conn->execute_read(ObString(strlen(sql), sql), read_res))) {
    LOG_WARN("execute copy source query failed", K(ret), K(sql));
  } else if (OB_ISNULL(result = read_res.get_result())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("no result", K(ret));
  }
  // open target file
  FILE *fp = NULL;
  if (OB_SUCC(ret)) {
    ObString npath;
    ObArenaAllocator alloc("CopyFile");
    if (OB_FAIL(ob_write_string(alloc, stmt.get_path(), npath))) {
    } else if (npath.length() >= OB_MAX_FILE_NAME_LENGTH) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("copy target path too long", K(ret));
    } else {
      char cpath[OB_MAX_FILE_NAME_LENGTH];
      memcpy(cpath, npath.ptr(), npath.length());
      cpath[npath.length()] = '\0';
      if (NULL == (fp = fopen(cpath, "wb"))) {
        ret = OB_FILE_NOT_OPENED;
        LOG_USER_ERROR(OB_FILE_NOT_OPENED, cpath);
        LOG_WARN("cannot open copy target file", K(ret), K(cpath));
      }
    }
  }
  // header
  if (OB_SUCC(ret) && stmt.get_header()) {
    ObSqlString header_line;
    for (int64_t i = 0; OB_SUCC(ret) && i < select_stmt->get_select_item_size(); ++i) {
      const SelectItem &item = select_stmt->get_select_item(i);
      const ObString &name = item.alias_name_.empty() ? item.expr_name_ : item.alias_name_;
      if (i > 0) {
        OZ (header_line.append(','));
      }
      OZ (header_line.append(name));
    }
    if (OB_SUCC(ret)) {
      OZ (header_line.append('\n'));
      int64_t len = header_line.length();
      if (len != fwrite(header_line.ptr(), 1, len, fp)) {
        ret = OB_IO_ERROR;
        LOG_WARN("write csv header failed", K(ret));
      }
    }
  }
  // rows
  int64_t written_rows = 0;
  while (OB_SUCC(ret) && OB_SUCCESS == (ret = result->next())) {
    ObSqlString line;
    int64_t col_cnt = result->get_column_count();
    for (int64_t c = 0; OB_SUCC(ret) && c < col_cnt; ++c) {
      ObObj obj;
      if (c > 0) {
        OZ (line.append(','));
      }
      if (OB_FAIL(result->get_obj(c, obj))) {
        LOG_WARN("get copy row obj failed", K(ret), K(c));
      } else {
        OZ (csv_append_field(line, obj, stmt.get_nullstr()));
      }
    }
    if (OB_SUCC(ret)) {
      OZ (line.append('\n'));
      int64_t len = line.length();
      if (len != fwrite(line.ptr(), 1, len, fp)) {
        ret = OB_IO_ERROR;
        LOG_WARN("write csv row failed", K(ret));
      } else {
        written_rows++;
      }
    }
  }
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
  }
  if (NULL != fp) {
    fclose(fp);
    fp = NULL;
  }
  if (OB_SUCC(ret)) {
    ObPhysicalPlanCtx *plan_ctx = ctx.get_physical_plan_ctx();
    if (OB_NOT_NULL(plan_ctx)) {
      plan_ctx->set_affected_rows(written_rows);
    }
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
