/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include "sql/ai/ai_audit.h"

#include <string>

#include "sql/ai/ai_file_audit.h"
#include "sql/mysqld.h"

namespace alisql::ai {

Ai_audit_sink *Get_ai_invoke_audit_sink() {
  if (!opt_ai_invoke_audit) return nullptr;

  // The option is startup-only, so the process-wide sink has one immutable
  // destination. The default remains under datadir and is never supplied by
  // a SQL session.
  static Ai_file_audit_sink sink(
      opt_ai_invoke_audit_log_file != nullptr
          ? std::string(opt_ai_invoke_audit_log_file)
          : std::string(mysql_real_data_home) + "ai_invoke_audit.jsonl");
  return &sink;
}

}  // namespace alisql::ai
