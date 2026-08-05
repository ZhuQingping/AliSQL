/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include "sql/ai/ai_provider_endpoint_policy.h"

namespace alisql::ai {

Ai_error Provider_endpoint_policy::Validate(
    std::string_view provider, Ai_capability capability,
    std::string_view endpoint_url, std::string_view provider_options) {
  if (provider_options.empty())
    return Ai_error::k_invalid_options;

  // P0 has one real egress provider.  It deliberately accepts only the two
  // documented HTTPS endpoints: no custom port, credential-bearing userinfo,
  // query/fragment, redirects, or IP-literal bypass can enter the transport.
  if (provider != "huawei") return Ai_error::k_protocol_mismatch;
  const std::string_view expected =
      capability == Ai_capability::k_text_embedding
          ? "https://api.modelarts-maas.com/v1/embeddings"
          : "https://api.modelarts-maas.com/v2/chat/completions";
  return endpoint_url == expected ? Ai_error::k_ok
                                  : Ai_error::k_protocol_mismatch;
}

}  // namespace alisql::ai
