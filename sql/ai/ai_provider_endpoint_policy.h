/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#ifndef SQL_AI_AI_PROVIDER_ENDPOINT_POLICY_INCLUDED
#define SQL_AI_AI_PROVIDER_ENDPOINT_POLICY_INCLUDED

#include <string_view>

#include "sql/ai/ai_types.h"

namespace alisql::ai {

/**
  Validates the governed egress destination stored in a model Profile.

  Provider options deliberately remain opaque to the generic registry, but
  must at least be a JSON object.  A concrete Provider is responsible for any
  additional option validation before it is enabled for dispatch.
*/
class Provider_endpoint_policy {
 public:
  static Ai_error Validate(std::string_view provider,
                           Ai_capability capability,
                           std::string_view endpoint_url,
                           std::string_view provider_options);
};

}  // namespace alisql::ai

#endif  // SQL_AI_AI_PROVIDER_ENDPOINT_POLICY_INCLUDED
