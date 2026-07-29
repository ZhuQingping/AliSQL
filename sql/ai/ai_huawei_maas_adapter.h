/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#ifndef SQL_AI_AI_HUAWEI_MAAS_ADAPTER_INCLUDED
#define SQL_AI_AI_HUAWEI_MAAS_ADAPTER_INCLUDED

#include <string>

#include "sql/ai/ai_http_transport.h"
#include "sql/ai/ai_provider_adapter.h"

namespace alisql::ai {

class Huawei_maas_adapter final : public Ai_provider_adapter {
 public:
  Huawei_maas_adapter(Ai_http_transport *transport, std::string bearer_token)
      : transport_(transport), bearer_token_(std::move(bearer_token)) {}

  Ai_error Execute(const Ai_canonical_request &request,
                   Ai_canonical_response *response) override;

 private:
  Ai_error ExecuteEmbedding(const Ai_canonical_request &request,
                            Ai_canonical_response *response);
  Ai_error ExecuteChat(const Ai_canonical_request &request,
                       Ai_canonical_response *response);

  Ai_http_transport *transport_;
  std::string bearer_token_;
};

}  // namespace alisql::ai

#endif  // SQL_AI_AI_HUAWEI_MAAS_ADAPTER_INCLUDED
