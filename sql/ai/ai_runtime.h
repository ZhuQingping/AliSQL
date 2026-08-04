/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */
#ifndef SQL_AI_AI_RUNTIME_INCLUDED
#define SQL_AI_AI_RUNTIME_INCLUDED

#include <string>
#include "sql/ai/ai_audit.h"
#include "sql/ai/ai_provider_adapter.h"

class THD;

namespace alisql::ai {
// Fixed P0 request ceilings.  They are intentionally not model-table fields:
// customer SQL gets a small, predictable safety envelope while an omitted
// option preserves the adapter's existing default behavior.
inline constexpr uint32_t k_ai_analyze_max_output_tokens = 32768;
inline constexpr uint32_t k_ai_analyze_max_timeout_ms = 60000;

struct Ai_analyze_options {
  uint32_t max_output_tokens{0};
  uint32_t timeout_ms{0};
};

// Validates the canonical Runtime contract. Zero for either numeric option is
// the internal sentinel for the adapter default; SQL parsing separately
// rejects an explicitly supplied zero.
Ai_error ValidateAnalyzeOptions(const Ai_analyze_options &options);

class Ai_runtime {
 public:
  Ai_runtime(Ai_provider_adapter *adapter, Ai_audit_sink *audit)
      : adapter_(adapter), audit_(audit) {}
  Ai_error ParseAnalyzeOptions(const std::string &json, Ai_analyze_options *out) const;
  Ai_error Embed(THD *thd, const std::string &text,
                 const std::string &model_name, uint32_t dimension,
                 std::string *encoded_vector) const;
  Ai_error Analyze(THD *thd, const std::string &model_name,
                   const std::string &prompt,
                   const Ai_analyze_options &options,
                   std::string *final_content) const;
 private:
  Ai_provider_adapter *adapter_;
  Ai_audit_sink *audit_;
};
}  // namespace alisql::ai
#endif
