/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include "sql/ai/ai_types.h"

namespace alisql::ai {

Ai_error Ai_canonical_response::ValidateChatCompletion() const {
  if (!response_complete || final_content.empty() || finish_reason == "length") {
    return Ai_error::k_incomplete_output;
  }
  return Ai_error::k_ok;
}

}  // namespace alisql::ai
