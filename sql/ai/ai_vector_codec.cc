/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */
#include "sql/ai/ai_vector_codec.h"

#include <cmath>
#include <cstring>

namespace alisql::ai {
Ai_error Ai_vector_codec::Encode(const std::vector<float> &values,
                                 uint32_t dimension, std::string *encoded) {
  if (encoded == nullptr || values.empty() || dimension == 0 ||
      values.size() != dimension)
    return Ai_error::k_dimension_mismatch;
  for (float value : values)
    if (!std::isfinite(value)) return Ai_error::k_provider_error;
  encoded->assign(values.size() * sizeof(float), '\0');
  std::memcpy(encoded->data(), values.data(), encoded->size());
  return Ai_error::k_ok;
}

float Ai_vector_codec::ReadFloatForTest(const std::string &encoded,
                                        uint32_t index) {
  float value = 0;
  std::memcpy(&value, encoded.data() + index * sizeof(float), sizeof(value));
  return value;
}
}  // namespace alisql::ai
