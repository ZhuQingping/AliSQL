/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#ifndef SQL_AI_AI_TYPES_INCLUDED
#define SQL_AI_AI_TYPES_INCLUDED

#include <cstdint>
#include <string>
#include <vector>

namespace alisql::ai {

enum class Ai_capability { k_text_embedding, k_text_generation };

enum class Ai_error {
  k_ok,
  k_invalid_options,
  k_incomplete_output,
  k_dimension_mismatch,
  k_timeout,
  k_provider_error,
  k_model_not_found,
  k_credential_unavailable,
  k_access_denied,
};

struct Ai_usage {
  uint64_t prompt_tokens{0};
  uint64_t completion_tokens{0};
  uint64_t reasoning_tokens{0};
  uint64_t cached_tokens{0};
  uint64_t total_tokens{0};
};

struct Ai_resolved_model {
  uint64_t config_id{0};
  uint64_t config_version{0};
  Ai_capability capability{Ai_capability::k_text_generation};
  std::string model_name;
  std::string provider_model_name;
  std::string model_revision;
  std::string provider;
  std::string endpoint_type;
  std::string endpoint;
  std::string credential_kind;
  std::string credential_ref;
};

struct Ai_canonical_request {
  Ai_capability capability{Ai_capability::k_text_generation};
  Ai_resolved_model model;
  std::string task;
  std::string input;
};

struct Ai_canonical_response {
  std::string final_content;
  std::string finish_reason;
  std::vector<std::vector<float>> embeddings;
  Ai_usage usage;
  bool response_complete{false};
  bool reasoning_present{false};

  Ai_error ValidateChatCompletion() const;
};

}  // namespace alisql::ai

#endif  // SQL_AI_AI_TYPES_INCLUDED
