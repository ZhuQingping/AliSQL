/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include <cstdio>
#include <fstream>
#include <string>

#include <sys/stat.h>

#include <gtest/gtest.h>

#include "sql/ai/ai_file_audit.h"

namespace alisql::ai {

Ai_error CompleteAiInvocationAudit(THD *caller, Ai_audit_sink *sink,
                                   uint64_t call_id,
                                   const Ai_audit_record &record);

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

class StartSucceedsCompleteFailsAuditSink final : public Ai_audit_sink {
 public:
  Ai_error Start(THD *, const Ai_audit_record &, uint64_t *call_id) override {
    if (call_id == nullptr) return Ai_error::k_audit_unavailable;
    *call_id = k_call_id;
    started_ = true;
    return Ai_error::k_ok;
  }

  Ai_error Complete(THD *, uint64_t call_id,
                    const Ai_audit_record &record) override {
    completed_call_id_ = call_id;
    completion_model_name_ = record.model_name;
    return Ai_error::k_audit_unavailable;
  }

  static constexpr uint64_t k_call_id = 97;
  bool started_{false};
  uint64_t completed_call_id_{0};
  std::string completion_model_name_;
};

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

TEST(AiFileAuditSinkTest,
     TerminalWriteFailureLeavesOnlyTheStartedEventAndKeepsItsCallId) {
  const std::string path = TestAuditPath("ai-file-audit-terminal-failure");
  std::remove(path.c_str());
  Ai_file_audit_sink audit(path);
  Ai_audit_record record = TestRecord();
  uint64_t call_id = 0;

  ASSERT_EQ(Ai_error::k_ok, audit.Start(nullptr, record, &call_id));
  ASSERT_NE(0U, call_id);
  // Make the second open fail only after the durable STARTED event exists.
  ASSERT_EQ(0, chmod(path.c_str(), S_IRUSR));
  record.status = Ai_audit_status::k_succeeded;
  record.provider_request_id = "provider-request-that-must-not-leak";
  EXPECT_EQ(Ai_error::k_audit_unavailable,
            audit.Complete(nullptr, call_id, record));
  ASSERT_EQ(0, chmod(path.c_str(), S_IRUSR | S_IWUSR));

  const std::string events = ReadFile(path);
  EXPECT_NE(std::string::npos, events.find("AI_CALL_STARTED"));
  EXPECT_NE(std::string::npos,
            events.find("\"call_id\":" + std::to_string(call_id)));
  EXPECT_EQ(std::string::npos, events.find("AI_CALL_SUCCEEDED"));
  EXPECT_EQ(std::string::npos, events.find("AI_CALL_FAILED"));
  EXPECT_EQ(std::string::npos,
            events.find("provider-request-that-must-not-leak"));
  std::remove(path.c_str());
}

TEST(AiFileAuditSinkTest,
     RuntimeClassifiesTerminalWriteFailureWithoutChangingTheStartedCallId) {
  StartSucceedsCompleteFailsAuditSink audit;
  Ai_audit_record record = TestRecord();
  record.model_name = "huawei/logical-profile";
  uint64_t call_id = 0;
  ASSERT_EQ(Ai_error::k_ok, audit.Start(nullptr, TestRecord(), &call_id));
  ASSERT_TRUE(audit.started_);

  EXPECT_EQ(Ai_error::k_audit_unavailable,
            CompleteAiInvocationAudit(nullptr, &audit, call_id, record));
  EXPECT_EQ(StartSucceedsCompleteFailsAuditSink::k_call_id,
            audit.completed_call_id_);
  EXPECT_EQ("huawei/logical-profile", audit.completion_model_name_);
}

}  // namespace alisql::ai
