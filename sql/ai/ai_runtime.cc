/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */
#include "sql/ai/ai_runtime.h"

#include <set>

#include <my_rapidjson_size_t.h>
#include <rapidjson/document.h>

namespace alisql::ai {
Ai_error ValidateAnalyzeOptions(const Ai_analyze_options &options) {
  if (options.max_output_tokens > k_ai_analyze_max_output_tokens ||
      options.timeout_ms > k_ai_analyze_max_timeout_ms)
    return Ai_error::k_invalid_options;
  return Ai_error::k_ok;
}

Ai_error Ai_runtime::ParseAnalyzeOptions(const std::string &json,
                                         Ai_analyze_options *out) const {
  if (out == nullptr) return Ai_error::k_invalid_options;
  *out = Ai_analyze_options{};
  if (json.empty()) return Ai_error::k_invalid_options;
  rapidjson::Document doc;
  if (doc.Parse(json.c_str()).HasParseError() || !doc.IsObject())
    return Ai_error::k_invalid_options;
  std::set<std::string> seen_keys;
  for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
    const std::string key(it->name.GetString(), it->name.GetStringLength());
    if (!seen_keys.insert(key).second) return Ai_error::k_invalid_options;
    const auto &value = it->value;
    if (key == "max_output_tokens" && value.IsUint() &&
             value.GetUint() > 0)
      out->max_output_tokens = value.GetUint();
    else if (key == "timeout_ms" && value.IsUint() && value.GetUint() > 0)
      out->timeout_ms = value.GetUint();
    else return Ai_error::k_invalid_options;
  }
  return ValidateAnalyzeOptions(*out);
}

Ai_error Ai_runtime::ParseEmbeddingOptions(
    const std::string &json, Ai_embedding_options *out) const {
  if (out == nullptr || json.empty()) return Ai_error::k_invalid_options;
  *out = Ai_embedding_options{};
  rapidjson::Document doc;
  if (doc.Parse(json.c_str()).HasParseError() || !doc.IsObject())
    return Ai_error::k_invalid_options;
  std::set<std::string> seen_keys;
  for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
    const std::string key(it->name.GetString(), it->name.GetStringLength());
    if (!seen_keys.insert(key).second) return Ai_error::k_invalid_options;
    if (key != "dimension" || !it->value.IsUint() ||
        it->value.GetUint() == 0)
      return Ai_error::k_invalid_options;
    out->dimension = it->value.GetUint();
  }
  return Ai_error::k_ok;
}
}  // namespace alisql::ai
