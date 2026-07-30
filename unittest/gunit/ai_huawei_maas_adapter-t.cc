/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include <gtest/gtest.h>

#include <string>

#include "sql/ai/ai_huawei_maas_adapter.h"

namespace alisql::ai {

namespace {

class Fake_transport final : public Ai_http_transport {
 public:
  Ai_error PostJson(const Ai_http_request &request,
                    Ai_http_response *response) override {
    last_request = request;
    *response = next_response;
    return next_error;
  }

  Ai_http_request last_request;
  Ai_http_response next_response;
  Ai_error next_error{Ai_error::k_ok};
};

Ai_canonical_request EmbeddingRequest() {
  Ai_canonical_request request;
  request.capability = Ai_capability::k_text_embedding;
  request.input = "text";
  request.model.model_name = "huawei/bge-m3";
  request.model.provider = "huawei";
  request.model.provider_model_name = "bge-m3";
  request.model.endpoint_type = "HTTPS_JSON";
  request.model.endpoint = "https://maas.example.invalid/v1/embeddings";
  return request;
}

Ai_canonical_request ChatRequest() {
  Ai_canonical_request request;
  request.capability = Ai_capability::k_text_generation;
  request.task = "Summarize";
  request.input = "input";
  request.model.model_name = "huawei/glm-5.2";
  request.model.provider = "huawei";
  request.model.provider_model_name = "glm-5.2";
  request.model.endpoint_type = "HTTPS_JSON";
  request.model.endpoint = "https://maas.example.invalid/v2/chat/completions";
  return request;
}

std::string EmbeddingBody(size_t dimensions) {
  std::string values;
  for (size_t i = 0; i < dimensions; ++i) {
    if (i != 0) values += ',';
    values += "0.125";
  }
  return "{\"data\":[{\"embedding\":[" + values + "]}],\"usage\":{\"prompt_tokens\":3,\"total_tokens\":3}}";
}

}  // namespace

TEST(HuaweiMaaSTest, EmbeddingRequestUsesResolvedProviderModel) {
  Fake_transport transport;
  transport.next_response.status_code = 200;
  transport.next_response.body = EmbeddingBody(1024);
  Huawei_maas_adapter adapter(&transport, "test-token");
  Ai_canonical_response response;

  ASSERT_EQ(Ai_error::k_ok, adapter.Execute(EmbeddingRequest(), &response));
  EXPECT_EQ(1024U, response.embeddings.front().size());
  EXPECT_NE(std::string::npos, transport.last_request.body.find("bge-m3"));
  EXPECT_EQ("Bearer test-token", transport.last_request.authorization);
}

TEST(HuaweiMaaSTest, ChatReasoningOnlyIsIncompleteOutput) {
  Fake_transport transport;
  transport.next_response.status_code = 200;
  transport.next_response.body =
      "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"reasoning_content\":\"hidden\"}}]}";
  Huawei_maas_adapter adapter(&transport, "test-token");
  Ai_canonical_response response;

  EXPECT_EQ(Ai_error::k_incomplete_output,
            adapter.Execute(ChatRequest(), &response));
}

TEST(HuaweiMaaSTest, ChatReturnsFinalContentButNeverReasoning) {
  Fake_transport transport;
  transport.next_response.status_code = 200;
  transport.next_response.body =
      "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"final\",\"reasoning_content\":\"hidden\"}}],\"usage\":{\"completion_tokens\":2}}";
  Huawei_maas_adapter adapter(&transport, "test-token");
  Ai_canonical_response response;

  EXPECT_EQ(Ai_error::k_ok, adapter.Execute(ChatRequest(), &response));
  EXPECT_EQ("final", response.final_content);
  EXPECT_TRUE(response.reasoning_present);
  EXPECT_EQ(2U, response.usage.completion_tokens);
}

TEST(HuaweiMaaSTest, ChatMapsStableOutputAndTimeoutLimits) {
  Fake_transport transport;
  transport.next_response.status_code = 200;
  transport.next_response.body =
      "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"final\"}}]}";
  Huawei_maas_adapter adapter(&transport, "test-token");
  Ai_canonical_request request = ChatRequest();
  request.max_output_tokens = 128;
  request.timeout_ms = 2345;
  Ai_canonical_response response;

  ASSERT_EQ(Ai_error::k_ok, adapter.Execute(request, &response));
  EXPECT_NE(std::string::npos, transport.last_request.body.find("\"max_tokens\":128"));
  EXPECT_EQ(2345U, transport.last_request.timeout_ms);
}

TEST(HuaweiMaaSTest, RejectsNonSuccessWithoutExposingProviderBody) {
  Fake_transport transport;
  transport.next_response.status_code = 429;
  transport.next_response.body = "secret provider error";
  Huawei_maas_adapter adapter(&transport, "test-token");
  Ai_canonical_response response;

  EXPECT_EQ(Ai_error::k_provider_error,
            adapter.Execute(ChatRequest(), &response));
  EXPECT_TRUE(response.final_content.empty());
}

TEST(HuaweiMaaSTest, RejectsMalformedEmbeddingEntriesWithoutCrashing) {
  Fake_transport transport;
  transport.next_response.status_code = 200;
  transport.next_response.body = "{\"data\":[7]}";
  Huawei_maas_adapter adapter(&transport, "test-token");
  Ai_canonical_response response;

  EXPECT_EQ(Ai_error::k_provider_error,
            adapter.Execute(EmbeddingRequest(), &response));
}

TEST(HuaweiMaaSTest, RejectsWrongBgeM3DimensionBeforeReturningVector) {
  Fake_transport transport;
  transport.next_response.status_code = 200;
  transport.next_response.body = EmbeddingBody(512);
  Huawei_maas_adapter adapter(&transport, "test-token");
  Ai_canonical_response response;

  EXPECT_EQ(Ai_error::k_dimension_mismatch,
            adapter.Execute(EmbeddingRequest(), &response));
  EXPECT_TRUE(response.embeddings.empty());
}

}  // namespace alisql::ai
