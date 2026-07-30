/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */
#ifndef SQL_AI_AI_AUDIT_INCLUDED
#define SQL_AI_AI_AUDIT_INCLUDED

#include <vector>
#include "sql/ai/ai_types.h"

namespace alisql::ai {
enum class Ai_audit_status { k_started, k_succeeded, k_failed };
struct Ai_audit_record {
  uint64_t call_id{0};
  uint64_t tenant_id{0};
  uint64_t config_id{0};
  uint64_t config_version{0};
  Ai_capability capability{Ai_capability::k_text_generation};
  Ai_audit_status status{Ai_audit_status::k_started};
  Ai_error error{Ai_error::k_ok};
  Ai_usage usage;
  std::string provider_request_id;
  unsigned int http_status{0};
};
class Ai_audit_sink {
 public:
  virtual ~Ai_audit_sink() = default;
  virtual Ai_error Start(const Ai_audit_record &record, uint64_t *call_id) = 0;
  virtual Ai_error Complete(uint64_t call_id, const Ai_audit_record &record) = 0;
};
class Ai_memory_audit_sink final : public Ai_audit_sink {
 public:
  Ai_error Start(const Ai_audit_record &, uint64_t *call_id) override {
    if (call_id == nullptr) return Ai_error::k_provider_error;
    *call_id = next_call_id_++;
    return Ai_error::k_ok;
  }
  Ai_error Complete(uint64_t call_id, const Ai_audit_record &record) override {
    if (call_id == 0) return Ai_error::k_provider_error;
    Ai_audit_record stored = record;
    stored.call_id = call_id;
    records_.push_back(std::move(stored));
    return Ai_error::k_ok;
  }
  const std::vector<Ai_audit_record> &records() const { return records_; }
 private:
  uint64_t next_call_id_{1};
  std::vector<Ai_audit_record> records_;
};
}  // namespace alisql::ai
#endif
