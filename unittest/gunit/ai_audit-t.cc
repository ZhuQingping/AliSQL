/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include <gtest/gtest.h>

#include "sql/ai/ai_audit.h"

namespace alisql::ai {

TEST(AiAuditTest, StartAllocatesCallIdBeforeDispatch) {
  Ai_memory_audit_sink audit;
  Ai_audit_record record;
  uint64_t call_id = 0;

  EXPECT_EQ(Ai_error::k_ok, audit.Start(nullptr, record, &call_id));
  EXPECT_NE(0U, call_id);
  EXPECT_TRUE(audit.records().empty());
}

TEST(AiAuditTest, CompletionStoresOnlyTelemetry) {
  Ai_memory_audit_sink audit;
  Ai_audit_record record;
  uint64_t call_id = 0;
  record.status = Ai_audit_status::k_succeeded;
  record.provider_request_id = "request-id";
  record.http_status = 200;
  record.latency_ms = 42;
  record.usage.total_tokens = 9;

  ASSERT_EQ(Ai_error::k_ok, audit.Start(nullptr, record, &call_id));
  ASSERT_EQ(Ai_error::k_ok, audit.Complete(nullptr, call_id, record));
  ASSERT_EQ(1U, audit.records().size());
  EXPECT_EQ(call_id, audit.records().front().call_id);
  EXPECT_EQ(42U, audit.records().front().latency_ms);
  EXPECT_EQ(9U, audit.records().front().usage.total_tokens);
}

}  // namespace alisql::ai
