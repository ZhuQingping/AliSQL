/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include <gtest/gtest.h>

#include "sql/ai/ai_provider_adapter.h"
#include "sql/ai/ai_types.h"

namespace alisql::ai {

TEST(AiTypesTest, RejectsReasoningOnlyCompletion) {
  Ai_canonical_response response;
  response.reasoning_present = true;

  EXPECT_EQ(Ai_error::k_incomplete_output, response.ValidateChatCompletion());
}

TEST(AiTypesTest, RejectsLengthFinishedCompletion) {
  Ai_canonical_response response;
  response.final_content = "partial";
  response.finish_reason = "length";

  EXPECT_EQ(Ai_error::k_incomplete_output, response.ValidateChatCompletion());
}

TEST(AiTypesTest, AcceptsNonEmptyFinalCompletion) {
  Ai_canonical_response response;
  response.final_content = "answer";
  response.finish_reason = "stop";
  response.response_complete = true;

  EXPECT_EQ(Ai_error::k_ok, response.ValidateChatCompletion());
}

class EchoAdapter final : public Ai_provider_adapter {
 public:
  Ai_error Execute(const Ai_canonical_request &request,
                   Ai_canonical_response *response) override {
    response->final_content = request.input;
    response->finish_reason = "stop";
    response->response_complete = true;
    return Ai_error::k_ok;
  }
};

TEST(AiTypesTest, AdapterUsesCanonicalRequestAndResponse) {
  Ai_canonical_request request;
  request.input = "canonical input";
  Ai_canonical_response response;
  EchoAdapter adapter;

  EXPECT_EQ(Ai_error::k_ok, adapter.Execute(request, &response));
  EXPECT_EQ("canonical input", response.final_content);
  EXPECT_EQ(Ai_error::k_ok, response.ValidateChatCompletion());
}

}  // namespace alisql::ai
