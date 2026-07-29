/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#ifndef SQL_AI_AI_HTTP_TRANSPORT_INCLUDED
#define SQL_AI_AI_HTTP_TRANSPORT_INCLUDED

#include <cstdint>
#include <string>
#include <vector>

#include "sql/ai/ai_types.h"

namespace alisql::ai {

constexpr size_t k_ai_max_http_response_bytes = 1024 * 1024;

struct Ai_http_request {
  std::string endpoint;
  std::string authorization;
  std::string body;
  uint32_t connect_timeout_ms{5000};
  uint32_t timeout_ms{30000};
};

struct Ai_http_response {
  unsigned int status_code{0};
  std::string body;
  std::string request_id;
};

class Ai_http_transport {
 public:
  virtual ~Ai_http_transport() = default;
  virtual Ai_error PostJson(const Ai_http_request &request,
                            Ai_http_response *response) = 0;
};

class Curl_ai_http_transport final : public Ai_http_transport {
 public:
  explicit Curl_ai_http_transport(std::vector<std::string> allowed_hosts);
  Ai_error PostJson(const Ai_http_request &request,
                    Ai_http_response *response) override;

 private:
  bool IsAllowedEndpoint(const std::string &endpoint) const;
  std::vector<std::string> allowed_hosts_;
};

}  // namespace alisql::ai

#endif  // SQL_AI_AI_HTTP_TRANSPORT_INCLUDED
