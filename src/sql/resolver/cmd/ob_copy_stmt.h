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

#ifndef OB_COPY_STMT_H_
#define OB_COPY_STMT_H_

#include "sql/resolver/cmd/ob_cmd_stmt.h"
#include "sql/resolver/ob_stmt_resolver.h"

namespace oceanbase
{
namespace sql
{

class ObSelectStmt;

class ObCopyStmt : public ObCMDStmt
{
public:
  ObCopyStmt()
      : ObCMDStmt(stmt::T_COPY),
        select_stmt_(NULL),
        header_(true)
  {}
  virtual ~ObCopyStmt() {}
  OB_INLINE void set_select_stmt(ObSelectStmt *stmt) { select_stmt_ = stmt; }
  OB_INLINE ObSelectStmt *get_select_stmt() { return select_stmt_; }
  OB_INLINE void set_path(const common::ObString &path) { path_ = path; }
  OB_INLINE const common::ObString &get_path() const { return path_; }
  OB_INLINE void set_header(bool v) { header_ = v; }
  OB_INLINE bool get_header() const { return header_; }
  OB_INLINE void set_nullstr(const common::ObString &s) { nullstr_ = s; }
  OB_INLINE const common::ObString &get_nullstr() const { return nullstr_; }
  TO_STRING_KV(K_(select_stmt), K_(path), K_(header), K_(nullstr));
private:
  ObSelectStmt *select_stmt_;
  common::ObString path_;
  bool header_;
  common::ObString nullstr_;
};

class ObCopyResolver : public ObStmtResolver
{
public:
  explicit ObCopyResolver(ObResolverParams &params) : ObStmtResolver(params) {}
  virtual ~ObCopyResolver() {}
  virtual int resolve(const ParseNode &parse_tree);
};

} // end namespace sql
} // end namespace oceanbase

#endif /* OB_COPY_STMT_H_ */
