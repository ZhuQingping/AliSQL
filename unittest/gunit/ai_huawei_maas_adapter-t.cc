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
    last_request.authorization = {};
    last_authorization.assign(request.authorization.data(),
                              request.authorization.size());
    *response = next_response;
    return next_error;
  }

  Ai_http_request last_request;
  std::string last_authorization;
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
  request.model.endpoint = "https://maas.example.invalid/v1/embeddings";
  return request;
}

Ai_canonical_request ChatRequest() {
  Ai_canonical_request request;
  request.capability = Ai_capability::k_text_generation;
  request.system_prompt = "Server-owned system instructions";
  request.input = "Task:\nCaller task text\n\nInput:\ncaller evidence";
  request.model.model_name = "huawei/glm-5.2";
  request.model.provider = "huawei";
  request.model.provider_model_name = "glm-5.2";
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

constexpr char k_test_token[] = "test-token";

}  // namespace

TEST(HuaweiMaaSTest, EmbeddingRequestUsesResolvedProviderModel) {
  Fake_transport transport;
  transport.next_response.status_code = 200;
  transport.next_response.body = EmbeddingBody(1024);
  Huawei_maas_adapter adapter(&transport);
  Ai_canonical_response response;

  ASSERT_EQ(Ai_error::k_ok,
            adapter.Execute(EmbeddingRequest(), k_test_token, &response));
  EXPECT_EQ(1024U, response.embeddings.front().size());
  EXPECT_NE(std::string::npos, transport.last_request.body.find("bge-m3"));
  EXPECT_EQ("Bearer test-token", transport.last_authorization);
}

TEST(HuaweiMaaSTest, ChatReasoningOnlyIsIncompleteOutput) {
  Fake_transport transport;
  transport.next_response.status_code = 200;
  transport.next_response.body =
      "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"reasoning_content\":\"hidden\"}}]}";
  Huawei_maas_adapter adapter(&transport);
  Ai_canonical_response response;

  EXPECT_EQ(Ai_error::k_incomplete_output,
            adapter.Execute(ChatRequest(), k_test_token, &response));
}

TEST(HuaweiMaaSTest, ChatReturnsFinalContentButNeverReasoning) {
  Fake_transport transport;
  transport.next_response.status_code = 200;
  transport.next_response.request_id = "provider-request-17";
  transport.next_response.body =
      "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"final\",\"reasoning_content\":\"hidden\"}}],\"usage\":{\"completion_tokens\":2}}";
  Huawei_maas_adapter adapter(&transport);
  Ai_canonical_response response;

  EXPECT_EQ(Ai_error::k_ok,
            adapter.Execute(ChatRequest(), k_test_token, &response));
  EXPECT_EQ("final", response.final_content);
  EXPECT_TRUE(response.reasoning_present);
  EXPECT_EQ(2U, response.usage.completion_tokens);
  EXPECT_EQ("provider-request-17", response.provider_request_id);
  EXPECT_EQ(200U, response.http_status);
}

TEST(HuaweiMaaSTest, ChatSerializesControlledSystemAndUserMessages) {
  Fake_transport transport;
  transport.next_response.status_code = 200;
  transport.next_response.body =
      "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"final\"}}]}";
  Huawei_maas_adapter adapter(&transport);
  Ai_canonical_response response;

  ASSERT_EQ(Ai_error::k_ok,
            adapter.Execute(ChatRequest(), k_test_token, &response));
  EXPECT_NE(std::string::npos,
            transport.last_request.body.find(
                "\"role\":\"system\",\"content\":\"Server-owned system instructions\""));
  EXPECT_NE(std::string::npos,
            transport.last_request.body.find(
                "\"role\":\"user\",\"content\":\"Task:\\nCaller task text\\n\\nInput:\\ncaller evidence\""));
}

TEST(HuaweiMaaSTest, ChatMapsStableOutputAndTimeoutLimits) {
  Fake_transport transport;
  transport.next_response.status_code = 200;
  transport.next_response.body =
      "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"final\"}}]}";
  Huawei_maas_adapter adapter(&transport);
  Ai_canonical_request request = ChatRequest();
  request.max_output_tokens = 128;
  request.timeout_ms = 2345;
  Ai_canonical_response response;

  ASSERT_EQ(Ai_error::k_ok, adapter.Execute(request, k_test_token, &response));
  EXPECT_NE(std::string::npos, transport.last_request.body.find("\"max_tokens\":128"));
  EXPECT_EQ(2345U, transport.last_request.timeout_ms);
}

TEST(HuaweiMaaSTest, RejectsNonSuccessWithoutExposingProviderBody) {
  Fake_transport transport;
  transport.next_response.status_code = 500;
  transport.next_response.body = "secret provider error";
  Huawei_maas_adapter adapter(&transport);
  Ai_canonical_response response;

  EXPECT_EQ(Ai_error::k_provider_error,
            adapter.Execute(ChatRequest(), k_test_token, &response));
  EXPECT_TRUE(response.final_content.empty());
}

TEST(HuaweiMaaSTest, ClassifiesProviderAuthorizationAndRateLimitFailures) {
  Fake_transport transport;
  Huawei_maas_adapter adapter(&transport);
  Ai_canonical_response response;

  transport.next_response.status_code = 403;
  transport.next_response.body = "provider details must not escape";
  EXPECT_EQ(Ai_error::k_access_denied,
            adapter.Execute(ChatRequest(), k_test_token, &response));
  EXPECT_TRUE(response.final_content.empty());

  transport.next_response.status_code = 429;
  EXPECT_EQ(Ai_error::k_rate_limited,
            adapter.Execute(ChatRequest(), k_test_token, &response));
}

TEST(HuaweiMaaSTest, RejectsEndpointProtocolMismatchBeforeEgress) {
  Fake_transport transport;
  Huawei_maas_adapter adapter(&transport);
  Ai_canonical_request request = ChatRequest();
  request.model.endpoint = "https://maas.example.invalid/v1/embeddings";
  Ai_canonical_response response;

  EXPECT_EQ(Ai_error::k_protocol_mismatch,
            adapter.Execute(request, k_test_token, &response));
  EXPECT_TRUE(transport.last_request.body.empty());
}

TEST(HuaweiMaaSTest, RejectsMalformedEmbeddingEntriesWithoutCrashing) {
  Fake_transport transport;
  transport.next_response.status_code = 200;
  transport.next_response.body = "{\"data\":[7]}";
  Huawei_maas_adapter adapter(&transport);
  Ai_canonical_response response;

  EXPECT_EQ(Ai_error::k_provider_error,
            adapter.Execute(EmbeddingRequest(), k_test_token, &response));
}

TEST(HuaweiMaaSTest, RejectsWrongBgeM3DimensionBeforeReturningVector) {
  Fake_transport transport;
  transport.next_response.status_code = 200;
  transport.next_response.body = EmbeddingBody(512);
  Huawei_maas_adapter adapter(&transport);
  Ai_canonical_response response;

  EXPECT_EQ(Ai_error::k_dimension_mismatch,
            adapter.Execute(EmbeddingRequest(), k_test_token, &response));
  EXPECT_TRUE(response.embeddings.empty());
}

}  // namespace alisql::ai
