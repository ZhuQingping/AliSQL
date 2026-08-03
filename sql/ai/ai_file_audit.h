/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */
#ifndef SQL_AI_AI_FILE_AUDIT_INCLUDED
#define SQL_AI_AI_FILE_AUDIT_INCLUDED

#include <cstdint>
#include <mutex>
#include <string>

#include "sql/ai/ai_audit.h"

namespace alisql::ai {

class Ai_file_audit_sink final : public Ai_audit_sink {
 public:
  explicit Ai_file_audit_sink(std::string path);

  Ai_error Start(THD *caller, const Ai_audit_record &record,
                 uint64_t *call_id) override;
  Ai_error Complete(THD *caller, uint64_t call_id,
                    const Ai_audit_record &record) override;

 private:
  Ai_error Append(const char *event_type, uint64_t call_id,
                  const Ai_audit_record &record, bool durable);

  std::string path_;
  std::mutex mutex_;
  uint64_t next_call_id_{0};
};

}  // namespace alisql::ai
#endif  // SQL_AI_AI_FILE_AUDIT_INCLUDED
