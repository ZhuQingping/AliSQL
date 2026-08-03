/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include <cstdio>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "sql/ai/ai_file_audit.h"

namespace alisql::ai {
namespace {

std::string TestAuditPath(const char *name) {
  return std::string(DATA_DIR) + "/" + name + ".jsonl";
}

std::string ReadFile(const std::string &path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

Ai_audit_record TestRecord() {
  Ai_audit_record record;
  record.config_id = 11;
  record.config_version = 3;
  record.capability = Ai_capability::k_text_generation;
  record.provider_request_id = "provider-request-17";
  record.http_status = 200;
  record.latency_ms = 42;
  record.usage.total_tokens = 9;
  return record;
}

}  // namespace

TEST(AiFileAuditSinkTest, StartPersistsEventBeforeCompletion) {
  const std::string path = TestAuditPath("ai-file-audit-start");
  std::remove(path.c_str());
  Ai_file_audit_sink audit(path);
  uint64_t call_id = 0;

  ASSERT_EQ(Ai_error::k_ok, audit.Start(nullptr, TestRecord(), &call_id));
  ASSERT_NE(0U, call_id);

  const std::string started = ReadFile(path);
  EXPECT_NE(std::string::npos, started.find("AI_CALL_STARTED"));
  EXPECT_NE(std::string::npos, started.find("\"call_id\":" +
                                             std::to_string(call_id)));
  EXPECT_EQ(std::string::npos, started.find("Authorization"));
  std::remove(path.c_str());
}

TEST(AiFileAuditSinkTest, CompletionUsesStartedCallId) {
  const std::string path = TestAuditPath("ai-file-audit-complete");
  std::remove(path.c_str());
  Ai_file_audit_sink audit(path);
  Ai_audit_record record = TestRecord();
  uint64_t call_id = 0;

  ASSERT_EQ(Ai_error::k_ok, audit.Start(nullptr, record, &call_id));
  record.status = Ai_audit_status::k_succeeded;
  ASSERT_EQ(Ai_error::k_ok, audit.Complete(nullptr, call_id, record));

  const std::string events = ReadFile(path);
  EXPECT_NE(std::string::npos, events.find("AI_CALL_SUCCEEDED"));
  const std::string id = "\"call_id\":" + std::to_string(call_id);
  EXPECT_NE(std::string::npos, events.find(id));
  EXPECT_NE(std::string::npos, events.find(id, events.find(id) + 1));
  std::remove(path.c_str());
}

TEST(AiFileAuditSinkTest, StartFailsWhenAuditDestinationIsUnavailable) {
  Ai_file_audit_sink audit("/db4ai-mtr-fixture.invalid/ai-audit.jsonl");
  uint64_t call_id = 0;

  EXPECT_EQ(Ai_error::k_audit_unavailable,
            audit.Start(nullptr, TestRecord(), &call_id));
  EXPECT_EQ(0U, call_id);
}

}  // namespace alisql::ai
