/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include "sql/ai/ai_file_audit.h"

#include <chrono>
#include <cstring>
#include <fcntl.h>

#include <my_rapidjson_size_t.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "my_io.h"
#include "my_systime.h"
#include "my_sys.h"

namespace alisql::ai {
namespace {

const char *CapabilityName(Ai_capability capability) {
  return capability == Ai_capability::k_text_embedding ? "TEXT_EMBEDDING"
                                                       : "TEXT_GENERATION";
}

const char *StatusName(Ai_audit_status status) {
  switch (status) {
    case Ai_audit_status::k_started:
      return "STARTED";
    case Ai_audit_status::k_succeeded:
      return "SUCCEEDED";
    case Ai_audit_status::k_failed:
      return "FAILED";
  }
  return "FAILED";
}

const char *ErrorName(Ai_error error) {
  switch (error) {
    case Ai_error::k_ok: return "OK";
    case Ai_error::k_invalid_options: return "INVALID_OPTIONS";
    case Ai_error::k_incomplete_output: return "INCOMPLETE_OUTPUT";
    case Ai_error::k_unsafe_output: return "UNSAFE_OUTPUT";
    case Ai_error::k_dimension_mismatch: return "DIMENSION_MISMATCH";
    case Ai_error::k_timeout: return "TIMEOUT";
    case Ai_error::k_provider_error: return "PROVIDER_ERROR";
    case Ai_error::k_model_not_found: return "MODEL_NOT_FOUND";
    case Ai_error::k_credential_unavailable: return "CREDENTIAL_UNAVAILABLE";
    case Ai_error::k_access_denied: return "ACCESS_DENIED";
    case Ai_error::k_response_too_large: return "RESPONSE_TOO_LARGE";
    case Ai_error::k_rate_limited: return "RATE_LIMITED";
    case Ai_error::k_protocol_mismatch: return "PROTOCOL_MISMATCH";
    case Ai_error::k_audit_unavailable: return "AUDIT_UNAVAILABLE";
  }
  return "PROVIDER_ERROR";
}

uint64_t TimestampMillis() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

}  // namespace

Ai_file_audit_sink::Ai_file_audit_sink(std::string path)
    : path_(std::move(path)), next_call_id_(my_micro_time()) {}

Ai_error Ai_file_audit_sink::Start(THD *, const Ai_audit_record &record,
                                    uint64_t *call_id) {
  if (call_id == nullptr || path_.empty()) return Ai_error::k_audit_unavailable;
  std::lock_guard<std::mutex> guard(mutex_);
  const uint64_t assigned_call_id = next_call_id_++;
  const Ai_error result =
      Append("AI_CALL_STARTED", assigned_call_id, record, true);
  if (result != Ai_error::k_ok) return result;
  *call_id = assigned_call_id;
  return Ai_error::k_ok;
}

Ai_error Ai_file_audit_sink::Complete(THD *, uint64_t call_id,
                                       const Ai_audit_record &record) {
  if (call_id == 0 || path_.empty()) return Ai_error::k_audit_unavailable;
  std::lock_guard<std::mutex> guard(mutex_);
  const char *event_type = record.status == Ai_audit_status::k_succeeded
                               ? "AI_CALL_SUCCEEDED"
                               : "AI_CALL_FAILED";
  return Append(event_type, call_id, record, true);
}

Ai_error Ai_file_audit_sink::Append(const char *event_type, uint64_t call_id,
                                    const Ai_audit_record &record,
                                    bool durable) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.StartObject();
  writer.Key("timestamp_ms"); writer.Uint64(TimestampMillis());
  writer.Key("event_type"); writer.String(event_type);
  writer.Key("call_id"); writer.Uint64(call_id);
  writer.Key("instance_id");
  writer.String(record.instance_id.data(), record.instance_id.size());
  writer.Key("account"); writer.String(record.account.data(), record.account.size());
  writer.Key("client_ip");
  writer.String(record.client_ip.data(), record.client_ip.size());
  writer.Key("capability"); writer.String(CapabilityName(record.capability));
  writer.Key("model_name");
  writer.String(record.model_name.data(), record.model_name.size());
  writer.Key("endpoint_fingerprint");
  writer.String(record.endpoint_fingerprint.data(),
                record.endpoint_fingerprint.size());
  writer.Key("config_id"); writer.Uint64(record.config_id);
  writer.Key("config_version"); writer.Uint64(record.config_version);
  writer.Key("status"); writer.String(StatusName(record.status));
  writer.Key("error_category"); writer.String(ErrorName(record.error));
  if (strcmp(event_type, "AI_CALL_STARTED") != 0) {
    writer.Key("provider_request_id");
    writer.String(record.provider_request_id.data(), record.provider_request_id.size());
    writer.Key("http_status"); writer.Uint(record.http_status);
    writer.Key("latency_ms"); writer.Uint64(record.latency_ms);
    writer.Key("prompt_tokens"); writer.Uint64(record.usage.prompt_tokens);
    writer.Key("completion_tokens"); writer.Uint64(record.usage.completion_tokens);
    writer.Key("reasoning_tokens"); writer.Uint64(record.usage.reasoning_tokens);
    writer.Key("cached_tokens"); writer.Uint64(record.usage.cached_tokens);
    writer.Key("total_tokens"); writer.Uint64(record.usage.total_tokens);
  }
  writer.EndObject();
  std::string line(buffer.GetString(), buffer.GetSize());
  line.push_back('\n');

  const File file = my_open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND,
                            MYF(MY_WME));
  if (file < 0) return Ai_error::k_audit_unavailable;
  // Restrict the file before the first event is written. A permission failure
  // means the event is not safely persisted, so pre-egress callers fail
  // closed instead of dispatching an unaudited request.
  if (my_chmod(path_.c_str(), USER_READ | USER_WRITE, MYF(0))) {
    (void)my_close(file, MYF(0));
    return Ai_error::k_audit_unavailable;
  }
  const bool write_failed =
      my_write(file, reinterpret_cast<const uchar *>(line.data()), line.size(),
               MYF(MY_WME)) != line.size();
  const bool sync_failed = !write_failed && durable && my_sync(file, MYF(MY_WME));
  const bool close_failed = my_close(file, MYF(MY_WME)) != 0;
  return write_failed || sync_failed || close_failed ? Ai_error::k_audit_unavailable
                                                      : Ai_error::k_ok;
}

}  // namespace alisql::ai
