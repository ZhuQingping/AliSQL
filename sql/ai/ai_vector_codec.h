/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */
#ifndef SQL_AI_AI_VECTOR_CODEC_INCLUDED
#define SQL_AI_AI_VECTOR_CODEC_INCLUDED

#include <cstdint>
#include <string>
#include <vector>
#include "sql/ai/ai_types.h"

namespace alisql::ai {
class Ai_vector_codec {
 public:
  static Ai_error Encode(const std::vector<float> &values, uint32_t dimension,
                         std::string *encoded);
  static float ReadFloatForTest(const std::string &encoded, uint32_t index);
};
}  // namespace alisql::ai
#endif
