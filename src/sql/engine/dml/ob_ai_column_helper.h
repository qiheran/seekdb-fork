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

#ifndef OCEANBASE_SQL_ENGINE_DML_OB_AI_COLUMN_HELPER_H_
#define OCEANBASE_SQL_ENGINE_DML_OB_AI_COLUMN_HELPER_H_

#include "lib/string/ob_sql_string.h"
#include "share/ob_errno.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_ai_column_meta.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "query/session/ob_session_inner_sql.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "sql/session/ob_inner_sql_connection.h"

namespace oceanbase
{
namespace sql
{

// Escape a string as a single-quoted SQL literal (without surrounding quotes are added by caller).
inline int ob_ai_sql_quote(common::ObSqlString &out, const common::ObString &s)
{
  int ret = common::OB_SUCCESS;
  if (OB_FAIL(out.append("'"))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < s.length(); ++i) {
      char c = s[i];
      if (c == '\'') {
        ret = out.append("''");
      } else if (c == '\\') {
        ret = out.append("\\\\");
      } else {
        ret = out.append(&c, 1);
      }
    }
    if (OB_SUCC(ret)) {
      ret = out.append("'");
    }
  }
  return ret;
}

// Build the SQL expression that computes the rowkey predicate string of the
// current row, e.g. CONCAT('`id` = ', CAST(`id` AS CHAR))
inline int ob_ai_build_rowkey_pred_expr(const share::schema::ObTableSchema &table_schema,
                                        common::ObSqlString &pred)
{
  int ret = common::OB_SUCCESS;
  common::ObArray<uint64_t> rk_ids;
  common::ObArray<common::ObString> rk_names;
  if (OB_FAIL(table_schema.get_rowkey_column_ids(rk_ids))) {
    LOG_WARN("get rowkey column ids failed", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rk_ids.count(); ++i) {
    const share::schema::ObColumnSchemaV2 *col = table_schema.get_column_schema(rk_ids.at(i));
    if (OB_ISNULL(col)) {
      ret = common::OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(rk_names.push_back(col->get_column_name_str()))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(pred.append_fmt("CONCAT("))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rk_names.count(); ++i) {
    if (i > 0 && OB_FAIL(pred.append_fmt(" ' AND '"))) {
    }
    if (OB_SUCC(ret) && OB_FAIL(pred.append_fmt("'`%.*s` = ', CAST(`%.*s` AS CHAR)",
            static_cast<int>(rk_names.at(i).length()), rk_names.at(i).ptr(),
            static_cast<int>(rk_names.at(i).length()), rk_names.at(i).ptr()))) {
    }
  }
  if (OB_SUCC(ret) && rk_names.count() > 0) {
    if (OB_FAIL(pred.append_fmt(")"))) {
    }
  }
  return ret;
}

inline int ob_ai_reconcile_tasks(common::sqlclient::ObISQLConnection *conn, uint64_t table_id)
{
  int ret = common::OB_SUCCESS;
  if (OB_ISNULL(conn)) {
    return common::OB_SUCCESS;
  }
  common::ObSqlString sql;
  common::ObSqlString db_name;
  common::ObSqlString table_name;
  int64_t affected = 0;
  // 0. AI column metadata first: without registered AI columns this DML has
  //    nothing to reconcile (also true for every inner/core table).
  common::ObISQLClient::ReadResult ai_res;
  common::ObArray<uint64_t> ai_col_ids;
  common::ObArray<common::ObSqlString> state_names;
  common::ObArray<common::ObSqlString> req_names;
  common::ObArray<int64_t> max_retries;
  if (OB_FAIL(sql.assign_fmt(
          "SELECT column_id AS column_id, state_column_name AS state_column_name, "
          "request_column_name AS request_column_name, max_retries AS max_retries "
          "FROM `oceanbase`.`__all_ai_column` WHERE table_id = %lu",
          static_cast<unsigned long>(table_id)))) {
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(conn->execute_read(sql.ptr(), ai_res))) {
    // metadata not ready (e.g. bootstrap phase): treat as nothing to do
    LOG_WARN("ai reconcile meta lookup failed", K(ret), K(sql));
    return common::OB_SUCCESS;
  } else if (OB_ISNULL(ai_res.mysql_result())) {
    return common::OB_SUCCESS;
  } else {
    sqlclient::ObMySQLResult *result = ai_res.mysql_result();
    int sql_ret = common::OB_SUCCESS;
    while (OB_SUCC(sql_ret)) {
      sql_ret = result->next();
      if (OB_SUCCESS == sql_ret) {
        uint64_t cid = 0;
        common::ObString state_name;
        common::ObString req_name;
        int64_t mr = 0;
        if (OB_FAIL(result->get_uint("column_id", cid))) {
        } else if (OB_FAIL(result->get_varchar("state_column_name", state_name))) {
        } else if (OB_FAIL(result->get_varchar("request_column_name", req_name))) {
        } else if (OB_FAIL(result->get_int("max_retries", mr))) {
        } else {
          common::ObSqlString st_buf;
          common::ObSqlString rq_buf;
          if (OB_FAIL(st_buf.append(state_name.ptr(), state_name.length()))) {
          } else if (OB_FAIL(rq_buf.append(req_name.ptr(), req_name.length()))) {
          } else if (OB_FAIL(state_names.push_back(st_buf))
              || OB_FAIL(req_names.push_back(rq_buf))
              || OB_FAIL(ai_col_ids.push_back(cid))
              || OB_FAIL(max_retries.push_back(mr))) {
          }
        }
      }
    }
    if (OB_FAIL(sql_ret) && sql_ret != common::OB_ITER_END) {
      LOG_WARN("ai reconcile meta iterate failed", K(ret));
      return common::OB_SUCCESS;
    }
  }
  if (ai_col_ids.count() == 0) {
    return common::OB_SUCCESS;
  }
  // 1. locate the table name and database name via SQL metadata
  sql.reset();
  if (OB_FAIL(sql.assign_fmt(
          "SELECT t.table_name AS table_name, d.database_name AS database_name "
          "FROM `oceanbase`.`__all_table` t "
          "JOIN `oceanbase`.`__all_database` d ON d.database_id = t.database_id "
          "WHERE t.table_id = %lu",
          static_cast<unsigned long>(table_id)))) {
  }
  common::ObISQLClient::ReadResult res;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(conn->execute_read(sql.ptr(), res))) {
    LOG_WARN("ai reconcile table lookup failed", K(ret), K(sql));
    return common::OB_SUCCESS;
  } else if (OB_ISNULL(res.mysql_result())) {
    return common::OB_SUCCESS;
  } else {
    sqlclient::ObMySQLResult *result = res.mysql_result();
    if (OB_SUCC(result->next())) {
      ObString tmp_db;
      ObString tmp_tbl;
      if (OB_FAIL(result->get_varchar("database_name", tmp_db))) {
      } else if (OB_FAIL(result->get_varchar("table_name", tmp_tbl))) {
      } else if (OB_FAIL(db_name.append(tmp_db.ptr(), tmp_db.length()))) {
      } else if (OB_FAIL(table_name.append(tmp_tbl.ptr(), tmp_tbl.length()))) {
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (table_name.empty()) {
    // table no longer exists: drop all its tasks
    sql.reset();
    if (OB_FAIL(sql.assign_fmt(
            "DELETE FROM `oceanbase`.`__all_ai_column_task` WHERE table_id = %lu",
            static_cast<unsigned long>(table_id)))) {
    } else if (OB_FAIL(conn->execute_write(sql.ptr(), affected))) {
      LOG_WARN("ai reconcile cleanup failed", K(ret), K(sql));
    }
    return ret;
  }
  // 2. build the rowkey predicate expression from rowkey column names
  common::ObSqlString pred;
  common::ObArray<common::ObSqlString> rk_names;
  if (OB_FAIL(ret)) {
  } else {
    sql.reset();
    if (OB_FAIL(sql.assign_fmt(
            "SELECT column_name AS column_name FROM `oceanbase`.`__all_column` "
            "WHERE table_id = %lu AND rowkey_position > 0 ORDER BY column_id",
            static_cast<unsigned long>(table_id)))) {
    }
    common::ObISQLClient::ReadResult rk_res;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(conn->execute_read(sql.ptr(), rk_res))) {
      LOG_WARN("ai reconcile rowkey lookup failed", K(ret), K(sql));
      return common::OB_SUCCESS;
    } else if (OB_ISNULL(rk_res.mysql_result())) {
      return common::OB_SUCCESS;
    } else {
      sqlclient::ObMySQLResult *result = rk_res.mysql_result();
      int sql_ret = common::OB_SUCCESS;
      while (OB_SUCC(sql_ret)) {
        sql_ret = result->next();
        if (OB_SUCCESS == sql_ret) {
          common::ObString name;
          common::ObSqlString name_buf;
          if (OB_FAIL(result->get_varchar("column_name", name))) {
          } else if (OB_FAIL(name_buf.append(name.ptr(), name.length()))) {
          } else if (OB_FAIL(rk_names.push_back(name_buf))) {
          }
        }
      }
      if (OB_FAIL(sql_ret) && sql_ret != common::OB_ITER_END) {
        LOG_WARN("ai reconcile rowkey iterate failed", K(ret));
        return common::OB_SUCCESS;
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(pred.append_fmt("CONCAT("))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rk_names.count(); ++i) {
    if (i > 0 && OB_FAIL(pred.append_fmt(" ' AND '"))) {
    }
    if (OB_FAIL(pred.append_fmt("'`%.*s` = ', CAST(`%.*s` AS CHAR)",
            static_cast<int>(rk_names.at(i).length()), rk_names.at(i).ptr(),
            static_cast<int>(rk_names.at(i).length()), rk_names.at(i).ptr()))) {
    }
  }
  if (OB_SUCC(ret) && rk_names.count() > 0) {
    if (OB_FAIL(pred.append_fmt(")"))) {
    }
  }
  // 3. remove task rows whose user row no longer exists
  if (OB_FAIL(ret)) {
  } else if (rk_names.count() > 0) {
    sql.reset();
    if (OB_FAIL(sql.assign_fmt(
            "DELETE FROM `oceanbase`.`__all_ai_column_task` WHERE table_id = %lu AND "
            "rowkey_predicate NOT IN (SELECT ",
            static_cast<unsigned long>(table_id)))) {
    } else if (OB_FAIL(sql.append(pred.ptr(), pred.length()))) {
    } else if (OB_FAIL(sql.append_fmt(" FROM `%.*s`.`%.*s`)",
            static_cast<int>(db_name.length()), db_name.ptr(),
            static_cast<int>(table_name.length()), table_name.ptr()))) {
    } else if (OB_FAIL(conn->execute_write(sql.ptr(), affected))) {
      LOG_WARN("ai reconcile delete failed", K(ret), K(sql));
    }
  }
  // 4. upsert task rows for pending AI columns
  for (int64_t i = 0; OB_SUCC(ret) && i < ai_col_ids.count(); ++i) {
    sql.reset();
    if (OB_FAIL(sql.assign_fmt(
            "INSERT INTO `oceanbase`.`__all_ai_column_task` (table_id, column_id, "
            "rowkey_predicate, request_id, state, retry_count, max_retries, eligible_ts) "
            "SELECT %lu, %lu, ",
            static_cast<unsigned long>(table_id),
            static_cast<unsigned long>(ai_col_ids.at(i))))) {
    } else if (OB_FAIL(sql.append(pred.ptr(), pred.length()))) {
    } else if (OB_FAIL(sql.append_fmt(
            ", `%.*s`, 0, 0, %ld, now(6) FROM `%.*s`.`%.*s` WHERE `%.*s` = 'PENDING' "
            "ON DUPLICATE KEY UPDATE request_id = VALUES(request_id), state = 0, "
            "retry_count = 0, max_retries = VALUES(max_retries), eligible_ts = now(6), "
            "gmt_modified = now(6)",
            static_cast<int>(req_names.at(i).length()), req_names.at(i).ptr(),
            static_cast<long>(max_retries.at(i)),
            static_cast<int>(db_name.length()), db_name.ptr(),
            static_cast<int>(table_name.length()), table_name.ptr(),
            static_cast<int>(state_names.at(i).length()), state_names.at(i).ptr()))) {
    } else if (OB_FAIL(conn->execute_write(sql.ptr(), affected))) {
      LOG_WARN("ai reconcile upsert failed", K(ret), K(sql));
    }
  }
  return ret;
}

// Strict read check: returns OB_ERR_AI_COLUMN_HAS_UNFINISHED_TASKS when the
// AI column has rows that are not READY.
inline int ob_ai_check_strict_read(common::sqlclient::ObISQLConnection *conn,
                                   const share::schema::ObTableSchema &table_schema,
                                   const common::ObString &db_name,
                                   uint64_t column_id)
{
  int ret = common::OB_SUCCESS;
  const share::schema::ObColumnSchemaV2 *col = table_schema.get_column_schema(column_id);
  share::schema::ObAiColumnMeta meta;
  if (OB_ISNULL(col)) {
    ret = common::OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(share::schema::parse_ai_meta(col->get_ai_meta(), meta))) {
    LOG_WARN("parse ai meta failed", K(ret));
  } else {
    common::ObSqlString sql;
    sql.assign_fmt("SELECT COUNT(*) AS cnt FROM `%.*s`.`%.*s` WHERE `%.*s` <> 'READY'",
        static_cast<int>(db_name.length()), db_name.ptr(),
        static_cast<int>(table_schema.get_table_name_str().length()),
        table_schema.get_table_name_str().ptr(),
        static_cast<int>(meta.state_col_name_.length()), meta.state_col_name_.ptr());
    common::ObISQLClient::ReadResult res;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(conn->execute_read(sql.ptr(), res))) {
      LOG_WARN("strict read count failed", K(ret), K(sql));
    } else if (OB_ISNULL(res.mysql_result())) {
      ret = common::OB_ERR_UNEXPECTED;
    } else {
      sqlclient::ObMySQLResult *result = res.mysql_result();
      int64_t cnt = 0;
      if (OB_SUCC(result->next())) {
        if (OB_FAIL(result->get_int("cnt", cnt))) {
          LOG_WARN("get count failed", K(ret));
        } else if (cnt > 0) {
          ret = OB_ERR_AI_COLUMN_HAS_UNFINISHED_TASKS;
          LOG_USER_ERROR(OB_ERR_AI_COLUMN_HAS_UNFINISHED_TASKS,
              static_cast<long>(table_schema.get_table_id()),
              static_cast<long>(column_id));
        }
      } else {
        ret = common::OB_SUCCESS; // no rows: treat as ready
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_DML_OB_AI_COLUMN_HELPER_H_
