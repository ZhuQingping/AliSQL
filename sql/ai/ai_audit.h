/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */
#ifndef SQL_AI_AI_AUDIT_INCLUDED
#define SQL_AI_AI_AUDIT_INCLUDED

#include <vector>
#include "sql/ai/ai_types.h"

namespace alisql::ai {
enum class Ai_audit_status { k_started, k_succeeded, k_failed };
struct Ai_audit_record {
  uint64_t tenant_id{0};
  uint64_t config_id{0};
  uint64_t config_version{0};
  Ai_capability capability{Ai_capability::k_text_generation};
  Ai_audit_status status{Ai_audit_status::k_started};
  Ai_usage usage;
  std::string provider_request_id;
};
class Ai_audit_sink {
 public:
  virtual ~Ai_audit_sink() = default;
  virtual void Complete(const Ai_audit_record &record) = 0;
};
class Ai_memory_audit_sink final : public Ai_audit_sink {
 public:
  void Complete(const Ai_audit_record &record) override { records_.push_back(record); }
  const std::vector<Ai_audit_record> &records() const { return records_; }
 private:
  std::vector<Ai_audit_record> records_;
};
}  // namespace alisql::ai
#endif
