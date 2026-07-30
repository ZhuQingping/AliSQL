/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include <gtest/gtest.h>

#include <limits>

#include "sql/ai/ai_vector_codec.h"

namespace alisql::ai {

TEST(AiVectorCodecTest, EncodesNativeFloatVector) {
  std::string encoded;
  EXPECT_EQ(Ai_error::k_ok,
            Ai_vector_codec::Encode({1.0F, -2.5F}, 2, &encoded));
  EXPECT_EQ(2U * sizeof(float), encoded.size());
  EXPECT_FLOAT_EQ(1.0F, Ai_vector_codec::ReadFloatForTest(encoded, 0));
  EXPECT_FLOAT_EQ(-2.5F, Ai_vector_codec::ReadFloatForTest(encoded, 1));
}

TEST(AiVectorCodecTest, RejectsDimensionMismatchAndNonFiniteValues) {
  std::string encoded;
  EXPECT_EQ(Ai_error::k_dimension_mismatch,
            Ai_vector_codec::Encode({1.0F}, 2, &encoded));
  EXPECT_EQ(Ai_error::k_provider_error,
            Ai_vector_codec::Encode({std::numeric_limits<float>::infinity()}, 1,
                                   &encoded));
}

}  // namespace alisql::ai
