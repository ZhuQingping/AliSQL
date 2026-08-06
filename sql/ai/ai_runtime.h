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
inline constexpr size_t k_ai_max_input_bytes = 1024 * 1024;
inline constexpr uint32_t k_ai_max_invocations_per_statement = 32;
inline constexpr uint32_t k_ai_max_concurrent_invocations = 32;

struct Ai_analyze_options {
  uint32_t max_output_tokens{0};
  uint32_t timeout_ms{0};
};

// Embedding options intentionally contain only the output dimension in P0.
// Keeping this as a JSON object, rather than adding positional arguments,
// leaves the public SQL contract extensible without accepting provider-specific
// knobs silently.
struct Ai_embedding_options {
  uint32_t dimension{0};
};

// Validates the canonical Runtime contract. Zero for either numeric option is
// the internal sentinel for the adapter default; SQL parsing separately
// rejects an explicitly supplied zero.
Ai_error ValidateAnalyzeOptions(const Ai_analyze_options &options);

/** Instance-level MaaS feature gate shared by SQL and control-plane entry points. */
bool IsAiMaaSEnabled();

class Ai_runtime {
 public:
  Ai_runtime(Ai_provider_adapter *adapter, Ai_audit_sink *audit)
      : adapter_(adapter), audit_(audit) {}
  Ai_error ParseAnalyzeOptions(const std::string &json, Ai_analyze_options *out) const;
  Ai_error ParseEmbeddingOptions(const std::string &json,
                                 Ai_embedding_options *out) const;
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
