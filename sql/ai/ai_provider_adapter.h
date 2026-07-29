#ifndef SQL_AI_AI_PROVIDER_ADAPTER_H
#define SQL_AI_AI_PROVIDER_ADAPTER_H

#include "sql/ai/ai_types.h"

namespace alisql::ai {

/**
  Provider-neutral execution boundary for DB4AI model adapters.

  Callers construct and validate a canonical request before reaching this
  interface. Implementations translate it to a provider protocol and return a
  canonical response, without exposing provider-specific payloads upstream.
*/
class Ai_provider_adapter {
 public:
  virtual ~Ai_provider_adapter() = default;

  virtual Ai_error Execute(const Ai_canonical_request &request,
                           Ai_canonical_response *response) = 0;
};

}  // namespace alisql::ai

#endif  // SQL_AI_AI_PROVIDER_ADAPTER_H
