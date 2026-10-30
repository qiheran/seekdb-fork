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

#ifndef OCEANBASE_SQL_ENGINE_CMD_OB_COPY_EXECUTOR_H_
#define OCEANBASE_SQL_ENGINE_CMD_OB_COPY_EXECUTOR_H_

#include "sql/resolver/cmd/ob_copy_stmt.h"

namespace oceanbase
{
namespace sql
{

class ObCopyExecutor
{
public:
  ObCopyExecutor() {}
  virtual ~ObCopyExecutor() {}
  int execute(ObExecContext &ctx, ObCopyStmt &stmt);
private:
  int write_csv(ObExecContext &ctx, ObCopyStmt &stmt, const char *sql);
  DISALLOW_COPY_AND_ASSIGN(ObCopyExecutor);
};

} // end namespace sql
} // end namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_CMD_OB_COPY_EXECUTOR_H_
