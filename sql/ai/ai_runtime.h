/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */
#ifndef SQL_AI_AI_RUNTIME_INCLUDED
#define SQL_AI_AI_RUNTIME_INCLUDED

#include <string>
#include "sql/ai/ai_audit.h"
#include "sql/ai/ai_provider_adapter.h"

class THD;

namespace alisql::ai {
struct Ai_analyze_options {
  std::string model_name;
  std::string mode{"analyze"};
  std::string output_format{"text"};
  bool return_sources{false};
  uint32_t max_output_tokens{0};
  uint32_t timeout_ms{0};
};
class Ai_runtime {
 public:
  Ai_runtime(Ai_provider_adapter *adapter, Ai_audit_sink *audit)
      : adapter_(adapter), audit_(audit) {}
  Ai_error ParseAnalyzeOptions(const std::string &json, Ai_analyze_options *out) const;
  Ai_error Embed(THD *thd, const std::string &text,
                 const std::string &model_name, uint32_t dimension,
                 std::string *encoded_vector) const;
  Ai_error Analyze(THD *thd, const std::string &task, const std::string &input,
                   const Ai_analyze_options &options,
                   std::string *final_content) const;
 private:
  Ai_provider_adapter *adapter_;
  Ai_audit_sink *audit_;
};
}  // namespace alisql::ai
#endif
