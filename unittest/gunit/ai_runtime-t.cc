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

TEST(AiRuntimeTest, ParsesOnlyStableAnalyzeOptions) {
  Ai_runtime runtime(nullptr, nullptr);
  Ai_analyze_options options;
  EXPECT_EQ(Ai_error::k_ok,
            runtime.ParseAnalyzeOptions(
                "{\"model_name\":\"huawei/glm-5.2\",\"mode\":\"summarize\","
                "\"output_format\":\"json\",\"return_sources\":true,"
                "\"max_output_tokens\":128,\"timeout_ms\":5000}",
                &options));
  EXPECT_EQ("huawei/glm-5.2", options.model_name);
  EXPECT_EQ("summarize", options.mode);
  EXPECT_TRUE(options.return_sources);
  EXPECT_EQ(128U, options.max_output_tokens);
}

TEST(AiRuntimeTest, RecordsReasoningTokensButNeverReasoningText) {
  Ai_memory_audit_sink audit;
  Ai_audit_record record;
  record.usage.reasoning_tokens = 17;
  record.status = Ai_audit_status::k_succeeded;
  audit.Complete(record);

  ASSERT_EQ(1U, audit.records().size());
  EXPECT_EQ(17U, audit.records().front().usage.reasoning_tokens);
  EXPECT_TRUE(audit.records().front().provider_request_id.empty());
}

}  // namespace alisql::ai
