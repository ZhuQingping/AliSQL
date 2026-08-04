/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include <gtest/gtest.h>

#include "sql/ai/ai_runtime.h"

namespace alisql::ai {

TEST(AiRuntimeTest, RejectsProviderPrivateOptions) {
  Ai_runtime runtime(nullptr, nullptr);
  Ai_analyze_options options;
  EXPECT_EQ(Ai_error::k_invalid_options,
            runtime.ParseAnalyzeOptions("{\"temperature\":0}", &options));
}

TEST(AiRuntimeTest, ParsesRagJsonSourceOptions) {
  Ai_runtime runtime(nullptr, nullptr);
  Ai_analyze_options options;
  EXPECT_EQ(Ai_error::k_ok,
            runtime.ParseAnalyzeOptions(
                "{\"model_name\":\"huawei/glm-5.2\",\"mode\":\"rag\","
                "\"output_format\":\"json\",\"return_sources\":true,"
                "\"max_output_tokens\":128,\"timeout_ms\":5000}",
                &options));
  EXPECT_EQ("huawei/glm-5.2", options.model_name);
  EXPECT_EQ("rag", options.mode);
  EXPECT_TRUE(options.return_sources);
  EXPECT_EQ(128U, options.max_output_tokens);
}

TEST(AiRuntimeTest, RejectsRagWithoutCanonicalJsonSourcesContract) {
  Ai_runtime runtime(nullptr, nullptr);
  Ai_analyze_options options;

  EXPECT_EQ(Ai_error::k_invalid_options,
            runtime.ParseAnalyzeOptions(
                "{\"model_name\":\"huawei/glm-5.2\",\"mode\":\"rag\"}",
                &options));
  EXPECT_EQ(Ai_error::k_invalid_options,
            runtime.ParseAnalyzeOptions(
                "{\"model_name\":\"huawei/glm-5.2\",\"mode\":\"rag\","
                "\"output_format\":\"json\",\"return_sources\":false}",
                &options));
  EXPECT_EQ(Ai_error::k_invalid_options,
            runtime.ParseAnalyzeOptions(
                "{\"model_name\":\"huawei/glm-5.2\",\"mode\":\"rag\","
                "\"output_format\":\"text\",\"return_sources\":true}",
                &options));
}

TEST(AiRuntimeTest, EnforcesAnalyzeParameterUpperBounds) {
  Ai_runtime runtime(nullptr, nullptr);
  Ai_analyze_options options;

  EXPECT_EQ(Ai_error::k_ok,
            runtime.ParseAnalyzeOptions(
                "{\"model_name\":\"huawei/glm-5.2\","
                "\"max_output_tokens\":25600,\"timeout_ms\":60000}",
                &options));
  EXPECT_EQ(Ai_error::k_invalid_options,
            runtime.ParseAnalyzeOptions(
                "{\"model_name\":\"huawei/glm-5.2\","
                "\"max_output_tokens\":0}", &options));
  EXPECT_EQ(Ai_error::k_invalid_options,
            runtime.ParseAnalyzeOptions(
                "{\"model_name\":\"huawei/glm-5.2\","
                "\"max_output_tokens\":32769}", &options));
  EXPECT_EQ(Ai_error::k_invalid_options,
            runtime.ParseAnalyzeOptions(
                "{\"model_name\":\"huawei/glm-5.2\",\"timeout_ms\":0}",
                &options));
  EXPECT_EQ(Ai_error::k_invalid_options,
            runtime.ParseAnalyzeOptions(
                "{\"model_name\":\"huawei/glm-5.2\","
                "\"timeout_ms\":60001}", &options));
}

TEST(AiRuntimeTest, RuntimeValidationRejectsConstructedUnsafeOptions) {
  Ai_analyze_options rag_options;
  rag_options.model_name = "unconfigured/rag";
  rag_options.mode = "rag";
  EXPECT_EQ(Ai_error::k_invalid_options,
            ValidateAnalyzeOptions(rag_options));

  Ai_analyze_options limit_options;
  limit_options.model_name = "unconfigured/limits";
  limit_options.max_output_tokens = k_ai_analyze_max_output_tokens + 1;
  EXPECT_EQ(Ai_error::k_invalid_options,
            ValidateAnalyzeOptions(limit_options));

  limit_options.max_output_tokens = 0;
  limit_options.timeout_ms = k_ai_analyze_max_timeout_ms + 1;
  EXPECT_EQ(Ai_error::k_invalid_options,
            ValidateAnalyzeOptions(limit_options));

  limit_options.timeout_ms = 0;
  EXPECT_EQ(Ai_error::k_ok, ValidateAnalyzeOptions(limit_options));
}

TEST(AiRuntimeTest, RecordsReasoningTokensButNeverReasoningText) {
  Ai_memory_audit_sink audit;
  Ai_audit_record record;
  uint64_t call_id = 0;
  record.usage.reasoning_tokens = 17;
  record.status = Ai_audit_status::k_succeeded;
  ASSERT_EQ(Ai_error::k_ok, audit.Start(nullptr, record, &call_id));
  ASSERT_EQ(Ai_error::k_ok, audit.Complete(nullptr, call_id, record));

  ASSERT_EQ(1U, audit.records().size());
  EXPECT_EQ(call_id, audit.records().front().call_id);
  EXPECT_EQ(17U, audit.records().front().usage.reasoning_tokens);
  EXPECT_TRUE(audit.records().front().provider_request_id.empty());
}

TEST(AiRuntimeAuditTest, StartAllocatesCallIdBeforeCompletion) {
  Ai_memory_audit_sink audit;
  Ai_audit_record record;
  uint64_t call_id = 0;

  ASSERT_EQ(Ai_error::k_ok, audit.Start(nullptr, record, &call_id));
  EXPECT_NE(0U, call_id);
  record.status = Ai_audit_status::k_succeeded;
  record.usage.total_tokens = 9;
  ASSERT_EQ(Ai_error::k_ok, audit.Complete(nullptr, call_id, record));
  ASSERT_EQ(1U, audit.records().size());
  EXPECT_EQ(call_id, audit.records().front().call_id);
  EXPECT_EQ(9U, audit.records().front().usage.total_tokens);
}

}  // namespace alisql::ai
