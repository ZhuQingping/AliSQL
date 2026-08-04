/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include <gtest/gtest.h>

#include "sql/ai/ai_runtime.h"

namespace alisql::ai {

TEST(AiRuntimeTest, ParsesOnlyPublicAnalyzeOptions) {
  Ai_runtime runtime(nullptr, nullptr);
  Ai_analyze_options options;

  EXPECT_EQ(Ai_error::k_invalid_options,
            runtime.ParseAnalyzeOptions("", &options));
  EXPECT_EQ(Ai_error::k_ok, runtime.ParseAnalyzeOptions("{}", &options));
  EXPECT_EQ(0U, options.max_output_tokens);
  EXPECT_EQ(0U, options.timeout_ms);
  EXPECT_EQ(Ai_error::k_ok,
            runtime.ParseAnalyzeOptions(
                "{\"max_output_tokens\":256,\"timeout_ms\":60000}",
                &options));
  EXPECT_EQ(256U, options.max_output_tokens);
  EXPECT_EQ(60000U, options.timeout_ms);
}

TEST(AiRuntimeTest, RejectsLegacyAndProviderPrivateAnalyzeOptions) {
  Ai_runtime runtime(nullptr, nullptr);
  Ai_analyze_options options;

  for (const char *json : {
           "{\"model_name\":\"huawei/glm-5.2\"}",
           "{\"mode\":\"rag\"}",
           "{\"output_format\":\"json\"}",
           "{\"return_sources\":true}",
           "{\"temperature\":0}",
           "{\"timeout_ms\":1000,\"timeout_ms\":2000}",
       }) {
    EXPECT_EQ(Ai_error::k_invalid_options,
              runtime.ParseAnalyzeOptions(json, &options));
  }
}

TEST(AiRuntimeTest, EnforcesAnalyzeParameterUpperBounds) {
  Ai_runtime runtime(nullptr, nullptr);
  Ai_analyze_options options;

  EXPECT_EQ(Ai_error::k_invalid_options,
            runtime.ParseAnalyzeOptions("{\"max_output_tokens\":0}",
                                        &options));
  EXPECT_EQ(Ai_error::k_invalid_options,
            runtime.ParseAnalyzeOptions("{\"max_output_tokens\":32769}",
                                        &options));
  EXPECT_EQ(Ai_error::k_invalid_options,
            runtime.ParseAnalyzeOptions("{\"timeout_ms\":0}", &options));
  EXPECT_EQ(Ai_error::k_invalid_options,
            runtime.ParseAnalyzeOptions("{\"timeout_ms\":60001}",
                                        &options));
}

TEST(AiRuntimeTest, RuntimeValidationAcceptsDefaultOptions) {
  Ai_analyze_options options;
  EXPECT_EQ(Ai_error::k_ok, ValidateAnalyzeOptions(options));

  options.max_output_tokens = k_ai_analyze_max_output_tokens + 1;
  EXPECT_EQ(Ai_error::k_invalid_options, ValidateAnalyzeOptions(options));

  options.max_output_tokens = 0;
  options.timeout_ms = k_ai_analyze_max_timeout_ms + 1;
  EXPECT_EQ(Ai_error::k_invalid_options, ValidateAnalyzeOptions(options));
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
