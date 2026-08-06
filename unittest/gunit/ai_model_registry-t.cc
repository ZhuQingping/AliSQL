/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include <gtest/gtest.h>

#include "sql/ai/ai_model_registry.h"

namespace alisql::ai {

namespace {

Ai_model_profile MakeProfile(const char *model_name, Ai_capability capability,
                             uint64_t config_version) {
  Ai_model_profile profile;
  profile.config_id = 100 + config_version;
  profile.config_version = config_version;
  profile.model_name = model_name;
  profile.capability = capability;
  profile.provider = "huawei";
  profile.provider_model_name = "provider-model";
  profile.endpoint = "https://maas.example.invalid";
  profile.active = true;
  return profile;
}

}  // namespace

TEST(AiModelRegistryTest, ResolvesActiveInstanceProfileAndFreezesVersion) {
  Ai_model_registry registry;
  auto profile = MakeProfile("huawei/bge-m3",
                             Ai_capability::k_text_embedding, 7);
  registry.AddProfileForTest(profile);
  Ai_resolved_model out;

  EXPECT_EQ(Ai_error::k_ok,
            registry.ResolveForTest(profile.model_name,
                                    Ai_capability::k_text_embedding, &out));
  EXPECT_EQ(7U, out.config_version);
  EXPECT_EQ(107U, out.config_id);
  EXPECT_EQ("huawei", out.provider);
}

TEST(AiModelRegistryTest, ResolvesLatestActiveExplicitProfileVersion) {
  Ai_model_registry registry;
  registry.AddProfileForTest(
      MakeProfile("mtr/versioned", Ai_capability::k_text_embedding, 1));
  registry.AddProfileForTest(
      MakeProfile("mtr/versioned", Ai_capability::k_text_embedding, 2));
  Ai_resolved_model out;

  EXPECT_EQ(Ai_error::k_ok,
            registry.ResolveForTest("mtr/versioned",
                                    Ai_capability::k_text_embedding, &out));
  EXPECT_EQ(2U, out.config_version);
}

TEST(AiModelRegistryTest, RejectsAnOmittedModelName) {
  Ai_model_registry registry;
  auto profile = MakeProfile("huawei/bge-m3",
                             Ai_capability::k_text_embedding, 9);
  registry.AddProfileForTest(profile);
  Ai_resolved_model out;

  EXPECT_EQ(Ai_error::k_model_not_found,
            registry.ResolveForTest("", Ai_capability::k_text_embedding,
                                    &out));
}

TEST(AiModelRegistryTest, DoesNotUseInactiveOrWrongCapabilityProfile) {
  Ai_model_registry registry;
  auto inactive = MakeProfile("huawei/bge-m3",
                              Ai_capability::k_text_embedding, 7);
  inactive.active = false;
  registry.AddProfileForTest(inactive);
  Ai_resolved_model out;

  EXPECT_NE(Ai_error::k_ok,
            registry.ResolveForTest(inactive.model_name,
                                    Ai_capability::k_text_embedding, &out));
}

TEST(AiModelRegistryTest, RejectsBgeM3DimensionOtherThan1024) {
  Ai_model_registry registry;
  auto profile = MakeProfile("huawei/bge-m3",
                             Ai_capability::k_text_embedding, 7);

  EXPECT_EQ(Ai_error::k_dimension_mismatch,
            registry.ValidateDimension(profile, 512));
  EXPECT_EQ(Ai_error::k_ok, registry.ValidateDimension(profile, 1024));
}

TEST(AiModelRegistryTest, RejectsUnsupportedEndpointBeforeDispatch) {
  Ai_model_registry registry;
  auto profile = MakeProfile("huawei/bge-m3",
                             Ai_capability::k_text_embedding, 7);
  profile.endpoint = "http://maas.example.invalid";
  registry.AddProfileForTest(profile);
  Ai_resolved_model out;

  EXPECT_EQ(Ai_error::k_model_not_found,
            registry.ResolveForTest(profile.model_name,
                                    Ai_capability::k_text_embedding, &out));
}

TEST(AiModelRegistryTest, CredentialResolverRejectsUnconfiguredSecret) {
  Ai_credential_resolver resolver;
  Ai_resolved_model model;
  Secure_string secret;

  EXPECT_EQ(Ai_error::k_credential_unavailable,
            resolver.ReadSecret(nullptr, model, &secret));
  EXPECT_TRUE(secret.empty());
}

}  // namespace alisql::ai
