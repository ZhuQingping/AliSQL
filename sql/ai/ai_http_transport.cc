/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include "sql/ai/ai_http_transport.h"

#include <algorithm>
#include <cstring>

#include <curl/curl.h>

namespace alisql::ai {

namespace {

struct Response_collector {
  std::string *body;
  bool exceeded_limit{false};
};

size_t CollectResponse(char *data, size_t size, size_t count, void *context) {
  const size_t bytes = size * count;
  auto *collector = static_cast<Response_collector *>(context);
  if (bytes > k_ai_max_http_response_bytes - collector->body->size()) {
    collector->exceeded_limit = true;
    return 0;
  }
  collector->body->append(data, bytes);
  return bytes;
}

size_t CollectHeaders(char *data, size_t size, size_t count, void *context) {
  const size_t bytes = size * count;
  auto *request_id = static_cast<std::string *>(context);
  constexpr char k_header[] = "x-request-id:";
  if (bytes > sizeof(k_header) - 1 &&
      std::equal(k_header, k_header + sizeof(k_header) - 1, data,
                 [](char lhs, char rhs) {
                   return std::tolower(static_cast<unsigned char>(lhs)) == rhs;
                 })) {
    const char *begin = data + sizeof(k_header) - 1;
    const char *end = data + bytes;
    while (begin != end && (*begin == ' ' || *begin == '\t')) ++begin;
    while (end != begin && (end[-1] == '\r' || end[-1] == '\n')) --end;
    request_id->assign(begin, end);
  }
  return bytes;
}

}  // namespace

Curl_ai_http_transport::Curl_ai_http_transport(
    std::vector<std::string> allowed_hosts)
    : allowed_hosts_(std::move(allowed_hosts)) {}

bool Curl_ai_http_transport::IsAllowedEndpoint(
    const std::string &endpoint) const {
  constexpr char k_scheme[] = "https://";
  if (endpoint.rfind(k_scheme, 0) != 0) return false;
  const size_t begin = sizeof(k_scheme) - 1;
  const size_t end = endpoint.find_first_of("/?#", begin);
  const std::string authority = endpoint.substr(begin, end - begin);
  return std::find(allowed_hosts_.begin(), allowed_hosts_.end(), authority) !=
         allowed_hosts_.end();
}

Ai_error Curl_ai_http_transport::PostJson(const Ai_http_request &request,
                                          Ai_http_response *response) {
  if (response == nullptr || !IsAllowedEndpoint(request.endpoint))
    return Ai_error::k_provider_error;
  response->body.clear();
  response->request_id.clear();
  response->status_code = 0;

  CURL *curl = curl_easy_init();
  if (curl == nullptr) return Ai_error::k_provider_error;
  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  if (!request.authorization.empty()) {
    const std::string header = "Authorization: " + request.authorization;
    headers = curl_slist_append(headers, header.c_str());
  }
  Response_collector collector{&response->body};
  curl_easy_setopt(curl, CURLOPT_URL, request.endpoint.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(request.body.size()));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                   static_cast<long>(request.connect_timeout_ms));
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout_ms));
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CollectResponse);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &collector);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CollectHeaders);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response->request_id);
  const CURLcode status = curl_easy_perform(curl);
  if (status == CURLE_OK)
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status_code);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (collector.exceeded_limit) return Ai_error::k_response_too_large;
  if (status == CURLE_OPERATION_TIMEDOUT) return Ai_error::k_timeout;
  return status == CURLE_OK ? Ai_error::k_ok : Ai_error::k_provider_error;
}

}  // namespace alisql::ai
