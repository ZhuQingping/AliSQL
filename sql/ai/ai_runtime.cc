/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */
#include "sql/ai/ai_runtime.h"
#include <my_rapidjson_size_t.h>
#include <rapidjson/document.h>

namespace alisql::ai {
Ai_error ValidateAnalyzeOptions(const Ai_analyze_options &options) {
  if (options.model_name.empty() ||
      (options.mode != "analyze" && options.mode != "rag" &&
       options.mode != "diagnose" && options.mode != "summarize" &&
       options.mode != "classify" && options.mode != "extract") ||
      (options.output_format != "text" && options.output_format != "json") ||
      (options.mode == "rag" &&
       (options.output_format != "json" || !options.return_sources)) ||
      (options.return_sources &&
       (options.mode != "rag" || options.output_format != "json")) ||
      options.max_output_tokens > k_ai_analyze_max_output_tokens ||
      options.timeout_ms > k_ai_analyze_max_timeout_ms)
    return Ai_error::k_invalid_options;
  return Ai_error::k_ok;
}

Ai_error Ai_runtime::ParseAnalyzeOptions(const std::string &json,
                                         Ai_analyze_options *out) const {
  if (out == nullptr) return Ai_error::k_invalid_options;
  *out = Ai_analyze_options{};
  if (json.empty()) return ValidateAnalyzeOptions(*out);
  rapidjson::Document doc;
  if (doc.Parse(json.c_str()).HasParseError() || !doc.IsObject())
    return Ai_error::k_invalid_options;
  for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
    const std::string key(it->name.GetString(), it->name.GetStringLength());
    const auto &value = it->value;
    if (key == "model_name" && value.IsString()) out->model_name = value.GetString();
    else if (key == "mode" && value.IsString()) out->mode = value.GetString();
    else if (key == "output_format" && value.IsString()) out->output_format = value.GetString();
    else if (key == "return_sources" && value.IsBool()) out->return_sources = value.GetBool();
    else if (key == "max_output_tokens" && value.IsUint() &&
             value.GetUint() > 0)
      out->max_output_tokens = value.GetUint();
    else if (key == "timeout_ms" && value.IsUint() && value.GetUint() > 0)
      out->timeout_ms = value.GetUint();
    else return Ai_error::k_invalid_options;
  }
  return ValidateAnalyzeOptions(*out);
}
}  // namespace alisql::ai
