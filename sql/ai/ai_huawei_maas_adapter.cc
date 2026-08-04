/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include "my_cleanse.h"

#include "sql/ai/ai_huawei_maas_adapter.h"

#include <my_rapidjson_size_t.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace alisql::ai {

namespace {

void ClearSensitiveString(std::string *value) {
  if (!value->empty()) my_cleanse(value->data(), value->size());
  value->clear();
}

void AddUsage(const rapidjson::Value &usage, Ai_usage *out) {
  if (!usage.IsObject()) return;
  const auto read = [&usage](const char *name) -> uint64_t {
    const auto it = usage.FindMember(name);
    return it != usage.MemberEnd() && it->value.IsUint64() ? it->value.GetUint64() : 0;
  };
  out->prompt_tokens = read("prompt_tokens");
  out->completion_tokens = read("completion_tokens");
  out->reasoning_tokens = read("reasoning_tokens");
  out->cached_tokens = read("cached_tokens");
  out->total_tokens = read("total_tokens");
}

Ai_error Post(Ai_http_transport *transport, const Ai_canonical_request &request,
              std::string_view token, const std::string &body,
              Ai_http_response *http_response) {
  if (transport == nullptr || token.empty() || request.model.provider != "huawei" ||
      request.model.endpoint_type != "HTTPS_JSON")
    return Ai_error::k_provider_error;
  Ai_http_request http_request;
  http_request.endpoint = request.model.endpoint;
  std::string authorization = "Bearer ";
  authorization.append(token.data(), token.size());
  http_request.authorization = authorization;
  http_request.body = body;
  if (request.timeout_ms != 0) http_request.timeout_ms = request.timeout_ms;
  const Ai_error error = transport->PostJson(http_request, http_response);
  ClearSensitiveString(&authorization);
  if (error != Ai_error::k_ok) return error;
  if (http_response->status_code >= 200 && http_response->status_code < 300)
    return Ai_error::k_ok;
  if (http_response->status_code == 401 || http_response->status_code == 403)
    return Ai_error::k_access_denied;
  if (http_response->status_code == 429) return Ai_error::k_rate_limited;
  if (http_response->status_code == 404) return Ai_error::k_model_not_found;
  return Ai_error::k_provider_error;
}

bool HasEndpointPath(const std::string &endpoint, const char *path) {
  return endpoint.find(path) != std::string::npos;
}

}  // namespace

Ai_error Huawei_maas_adapter::Execute(const Ai_canonical_request &request,
                                      std::string_view credential,
                                      Ai_canonical_response *response) {
  if (response == nullptr) return Ai_error::k_provider_error;
  *response = Ai_canonical_response{};
  if (request.capability == Ai_capability::k_text_embedding)
    return ExecuteEmbedding(request, credential, response);
  return ExecuteChat(request, credential, response);
}

Ai_error Huawei_maas_adapter::ExecuteEmbedding(const Ai_canonical_request &request,
                                               std::string_view credential,
                                               Ai_canonical_response *response) {
  if (!HasEndpointPath(request.model.endpoint, "/v1/embeddings"))
    return Ai_error::k_protocol_mismatch;
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.StartObject();
  writer.Key("model");
  writer.String(request.model.provider_model_name.c_str());
  writer.Key("input");
  writer.String(request.input.c_str());
  writer.Key("encoding_format");
  writer.String("float");
  writer.EndObject();
  Ai_http_response http_response;
  const Ai_error error = Post(transport_, request, credential, buffer.GetString(),
                              &http_response);
  response->provider_request_id = http_response.request_id;
  response->http_status = http_response.status_code;
  if (error != Ai_error::k_ok) return error;

  rapidjson::Document doc;
  if (doc.Parse(http_response.body.c_str()).HasParseError() || !doc.IsObject())
    return Ai_error::k_provider_error;
  const auto data = doc.FindMember("data");
  if (data == doc.MemberEnd() || !data->value.IsArray() || data->value.Empty())
    return Ai_error::k_provider_error;
  for (const auto &entry : data->value.GetArray()) {
    if (!entry.IsObject()) return Ai_error::k_provider_error;
    const auto embedding = entry.FindMember("embedding");
    if (embedding == entry.MemberEnd() || !embedding->value.IsArray())
      return Ai_error::k_provider_error;
    std::vector<float> vector;
    vector.reserve(embedding->value.Size());
    for (const auto &value : embedding->value.GetArray()) {
      if (!value.IsNumber()) return Ai_error::k_provider_error;
      vector.push_back(value.GetFloat());
    }
    if (request.model.model_name == "huawei/bge-m3" && vector.size() != 1024)
      return Ai_error::k_dimension_mismatch;
    response->embeddings.push_back(std::move(vector));
  }
  const auto usage = doc.FindMember("usage");
  if (usage != doc.MemberEnd()) AddUsage(usage->value, &response->usage);
  response->response_complete = true;
  return Ai_error::k_ok;
}

Ai_error Huawei_maas_adapter::ExecuteChat(const Ai_canonical_request &request,
                                          std::string_view credential,
                                          Ai_canonical_response *response) {
  if (!HasEndpointPath(request.model.endpoint, "/v2/chat/completions"))
    return Ai_error::k_protocol_mismatch;
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.StartObject();
  writer.Key("model");
  writer.String(request.model.provider_model_name.c_str());
  writer.Key("messages");
  writer.StartArray();
  writer.StartObject(); writer.Key("role"); writer.String("system");
  writer.Key("content"); writer.String(request.system_prompt.c_str()); writer.EndObject();
  writer.StartObject(); writer.Key("role"); writer.String("user");
  writer.Key("content"); writer.String(request.input.c_str()); writer.EndObject();
  writer.EndArray();
  if (request.max_output_tokens != 0) {
    writer.Key("max_tokens");
    writer.Uint(request.max_output_tokens);
  }
  writer.EndObject();
  Ai_http_response http_response;
  const Ai_error error = Post(transport_, request, credential, buffer.GetString(),
                              &http_response);
  response->provider_request_id = http_response.request_id;
  response->http_status = http_response.status_code;
  if (error != Ai_error::k_ok) return error;

  rapidjson::Document doc;
  if (doc.Parse(http_response.body.c_str()).HasParseError() || !doc.IsObject())
    return Ai_error::k_provider_error;
  const auto choices = doc.FindMember("choices");
  if (choices == doc.MemberEnd() || !choices->value.IsArray() || choices->value.Empty())
    return Ai_error::k_incomplete_output;
  const auto &choice = choices->value[0];
  if (!choice.IsObject()) return Ai_error::k_incomplete_output;
  const auto message = choice.FindMember("message");
  if (message == choice.MemberEnd() || !message->value.IsObject())
    return Ai_error::k_incomplete_output;
  const auto content = message->value.FindMember("content");
  if (content != message->value.MemberEnd() && content->value.IsString())
    response->final_content.assign(content->value.GetString(), content->value.GetStringLength());
  const auto reasoning = message->value.FindMember("reasoning_content");
  response->reasoning_present = reasoning != message->value.MemberEnd() &&
                               reasoning->value.IsString() &&
                               reasoning->value.GetStringLength() != 0;
  const auto finish = choice.FindMember("finish_reason");
  if (finish != choice.MemberEnd() && finish->value.IsString())
    response->finish_reason.assign(finish->value.GetString(), finish->value.GetStringLength());
  const auto usage = doc.FindMember("usage");
  if (usage != doc.MemberEnd()) AddUsage(usage->value, &response->usage);
  response->response_complete = true;
  return response->ValidateChatCompletion();
}

}  // namespace alisql::ai
