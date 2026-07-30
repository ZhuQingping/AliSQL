/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include "sql/ai/ai_runtime.h"

#include <chrono>

#include "sql/ai/ai_huawei_maas_adapter.h"
#include "sql/ai/ai_model_registry.h"
#include "sql/ai/ai_vector_codec.h"

namespace alisql::ai {
namespace {

std::string EndpointAuthority(const std::string &endpoint) {
  constexpr char k_scheme[] = "https://";
  if (endpoint.rfind(k_scheme, 0) != 0) return {};
  const size_t start = sizeof(k_scheme) - 1;
  const size_t end = endpoint.find_first_of("/?#", start);
  return endpoint.substr(start, end - start);
}

Ai_audit_record NewAuditRecord(const Ai_resolved_model &model,
                               Ai_capability capability) {
  Ai_audit_record record;
  record.tenant_id = model.tenant_id;
  record.config_id = model.config_id;
  record.config_version = model.config_version;
  record.capability = capability;
  return record;
}

Ai_error StartInvocation(THD *thd, Ai_audit_sink *sink,
                         const Ai_resolved_model &model,
                         Ai_capability capability, uint64_t *call_id) {
  if (call_id == nullptr) return Ai_error::k_audit_unavailable;
  *call_id = 0;
  if (sink == nullptr) return Ai_error::k_ok;
  const Ai_error result =
      sink->Start(thd, NewAuditRecord(model, capability), call_id);
  return result == Ai_error::k_ok ? result : Ai_error::k_audit_unavailable;
}

Ai_error CompleteInvocation(THD *thd, Ai_audit_sink *sink, uint64_t call_id,
                            const Ai_resolved_model &model,
                            Ai_capability capability,
                            const Ai_canonical_response *response,
                            uint64_t latency_ms, Ai_error result) {
  Ai_audit_record record = NewAuditRecord(model, capability);
  record.status = result == Ai_error::k_ok ? Ai_audit_status::k_succeeded
                                            : Ai_audit_status::k_failed;
  record.error = result;
  record.latency_ms = latency_ms;
  if (response != nullptr) {
    record.usage = response->usage;
    record.provider_request_id = response->provider_request_id;
    record.http_status = response->http_status;
  }
  if (sink != nullptr) {
    const Ai_error complete = sink->Complete(thd, call_id, record);
    if (complete != Ai_error::k_ok) return Ai_error::k_audit_unavailable;
  }
  return result;
}

}  // namespace

Ai_error Ai_runtime::Embed(THD *thd, const std::string &text,
                           const std::string &model_name, uint32_t dimension,
                           std::string *encoded_vector) const {
  if (encoded_vector == nullptr || text.empty()) return Ai_error::k_provider_error;
  Ai_model_registry registry;
  Ai_resolved_model model;
  const Ai_error resolve = registry.Resolve(
      thd, model_name.empty() ? "huawei/bge-m3" : model_name,
      Ai_capability::k_text_embedding, &model);
  if (resolve != Ai_error::k_ok) return resolve;

  uint64_t call_id = 0;
  const Ai_error audit_start = StartInvocation(
      thd, audit_, model, Ai_capability::k_text_embedding, &call_id);
  if (audit_start != Ai_error::k_ok) return audit_start;
  const auto invocation_started = std::chrono::steady_clock::now();
  const auto complete = [&](const Ai_canonical_response *response,
                            Ai_error result) {
    const auto elapsed = std::chrono::steady_clock::now() - invocation_started;
    return CompleteInvocation(
        thd, audit_, call_id, model, Ai_capability::k_text_embedding, response,
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count()),
        result);
  };

  const uint32_t expected_dimension = dimension == 0 ? model.dimension : dimension;
  if (expected_dimension == 0 ||
      (model.dimension != 0 && expected_dimension != model.dimension) ||
      (model.model_name == "huawei/bge-m3" && expected_dimension != 1024))
    return complete(nullptr, Ai_error::k_dimension_mismatch);

  Ai_credential_resolver credential_resolver;
  Secure_string credential;
  const Ai_error credential_error =
      credential_resolver.ReadSecret(thd, model, &credential);
  if (credential_error != Ai_error::k_ok)
    return complete(nullptr, credential_error);

  const std::string authority = EndpointAuthority(model.endpoint);
  if (authority.empty())
    return complete(nullptr, Ai_error::k_provider_error);
  Curl_ai_http_transport transport({authority});
  Huawei_maas_adapter adapter(&transport);
  Ai_canonical_request request;
  request.capability = Ai_capability::k_text_embedding;
  request.model = model;
  request.input = text;
  Ai_canonical_response response;
  const Ai_error execute = adapter.Execute(request, credential.view(), &response);
  if (execute != Ai_error::k_ok || response.embeddings.size() != 1)
    return complete(&response,
                    execute == Ai_error::k_ok ? Ai_error::k_provider_error
                                               : execute);
  const Ai_error encode = Ai_vector_codec::Encode(response.embeddings.front(),
                                                  expected_dimension,
                                                  encoded_vector);
  return complete(&response, encode);
}

Ai_error Ai_runtime::Analyze(THD *thd, const std::string &task,
                             const std::string &input,
                             const Ai_analyze_options &options,
                             std::string *final_content) const {
  if (final_content == nullptr || task.empty() || input.empty())
    return Ai_error::k_provider_error;
  Ai_model_registry registry;
  Ai_resolved_model model;
  const Ai_error resolve = registry.Resolve(
      thd, options.model_name.empty() ? "huawei/glm-5.2" : options.model_name,
      Ai_capability::k_text_generation, &model);
  if (resolve != Ai_error::k_ok) return resolve;

  uint64_t call_id = 0;
  const Ai_error audit_start = StartInvocation(
      thd, audit_, model, Ai_capability::k_text_generation, &call_id);
  if (audit_start != Ai_error::k_ok) return audit_start;
  const auto invocation_started = std::chrono::steady_clock::now();
  const auto complete = [&](const Ai_canonical_response *response,
                            Ai_error result) {
    const auto elapsed = std::chrono::steady_clock::now() - invocation_started;
    return CompleteInvocation(
        thd, audit_, call_id, model, Ai_capability::k_text_generation, response,
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count()),
        result);
  };

  Ai_credential_resolver credential_resolver;
  Secure_string credential;
  const Ai_error credential_error =
      credential_resolver.ReadSecret(thd, model, &credential);
  if (credential_error != Ai_error::k_ok)
    return complete(nullptr, credential_error);

  const std::string authority = EndpointAuthority(model.endpoint);
  if (authority.empty())
    return complete(nullptr, Ai_error::k_provider_error);
  Curl_ai_http_transport transport({authority});
  Huawei_maas_adapter adapter(&transport);
  Ai_canonical_request request;
  request.capability = Ai_capability::k_text_generation;
  request.model = model;
  request.task = task;
  request.input = input;
  request.max_output_tokens = options.max_output_tokens;
  request.timeout_ms = options.timeout_ms;
  Ai_canonical_response response;
  const Ai_error execute = adapter.Execute(request, credential.view(), &response);
  if (execute != Ai_error::k_ok)
    return complete(&response, execute);
  *final_content = std::move(response.final_content);
  return complete(&response, Ai_error::k_ok);
}

}  // namespace alisql::ai
