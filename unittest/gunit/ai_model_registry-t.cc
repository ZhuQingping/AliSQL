/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include <gtest/gtest.h>

#include "sql/ai/ai_model_registry.h"

namespace alisql::ai {

namespace {

Ai_model_profile MakeProfile(uint64_t tenant_id, const char *model_name,
                             Ai_capability capability,
                             uint64_t config_version) {
  Ai_model_profile profile;
  profile.tenant_id = tenant_id;
  profile.config_id = 100 + tenant_id;
  profile.config_version = config_version;
  profile.model_name = model_name;
  profile.capability = capability;
  profile.provider = "huawei";
  profile.provider_model_name = "provider-model";
  profile.model_revision = "2026-07";
  profile.endpoint_type = "HTTPS_JSON";
  profile.endpoint = "https://maas.example.invalid";
  profile.active = true;
  return profile;
}

}  // namespace

TEST(AiModelRegistryTest, ResolvesActiveTenantProfileAndFreezesVersion) {
  Ai_model_registry registry;
  auto profile = MakeProfile(42, "huawei/bge-m3",
                             Ai_capability::k_text_embedding, 7);
  registry.AddProfileForTest(profile);
  Ai_resolved_model out;

  EXPECT_EQ(Ai_error::k_ok,
            registry.ResolveForTest(42, profile.model_name,
                                    Ai_capability::k_text_embedding, &out));
  EXPECT_EQ(7U, out.config_version);
  EXPECT_EQ(142U, out.config_id);
  EXPECT_EQ("huawei", out.provider);
}

TEST(AiModelRegistryTest, TenantProfileWinsOverDefaultProfile) {
  Ai_model_registry registry;
  auto fallback = MakeProfile(0, "huawei/bge-m3",
                              Ai_capability::k_text_embedding, 1);
  fallback.config_id = 1;
  auto tenant = MakeProfile(42, "huawei/bge-m3",
                            Ai_capability::k_text_embedding, 9);
  registry.AddProfileForTest(fallback);
  registry.AddProfileForTest(tenant);
  Ai_resolved_model out;

  EXPECT_EQ(Ai_error::k_ok,
            registry.ResolveForTest(42, "huawei/bge-m3",
                                    Ai_capability::k_text_embedding, &out));
  EXPECT_EQ(142U, out.config_id);
  EXPECT_EQ(9U, out.config_version);
}

TEST(AiModelRegistryTest, DoesNotUseInactiveOrWrongCapabilityProfile) {
  Ai_model_registry registry;
  auto inactive = MakeProfile(42, "huawei/bge-m3",
                              Ai_capability::k_text_embedding, 7);
  inactive.active = false;
  registry.AddProfileForTest(inactive);
  Ai_resolved_model out;

  EXPECT_NE(Ai_error::k_ok,
            registry.ResolveForTest(42, inactive.model_name,
                                    Ai_capability::k_text_embedding, &out));
}

TEST(AiModelRegistryTest, RejectsBgeM3DimensionOtherThan1024) {
  Ai_model_registry registry;
  auto profile = MakeProfile(42, "huawei/bge-m3",
                             Ai_capability::k_text_embedding, 7);

  EXPECT_EQ(Ai_error::k_dimension_mismatch,
            registry.ValidateDimension(profile, 512));
  EXPECT_EQ(Ai_error::k_ok, registry.ValidateDimension(profile, 1024));
}

TEST(AiModelRegistryTest, RejectsUnsupportedEndpointBeforeDispatch) {
  Ai_model_registry registry;
  auto profile = MakeProfile(42, "huawei/bge-m3",
                             Ai_capability::k_text_embedding, 7);
  profile.endpoint = "http://maas.example.invalid";
  registry.AddProfileForTest(profile);
  Ai_resolved_model out;

  EXPECT_EQ(Ai_error::k_model_not_found,
            registry.ResolveForTest(42, profile.model_name,
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

TEST(AiCredentialResolverTest, PlaintextDevRequiresDebugPolicy) {
  Ai_credential_resolver resolver;
  Secure_string credential;

  EXPECT_EQ(Ai_error::k_credential_unavailable,
            resolver.ReadPlaintextDevForTest(false, "PLAINTEXT_DEV",
                                             "unit-token", &credential));
  EXPECT_TRUE(credential.empty());
}

TEST(AiCredentialResolverTest, PlaintextDevReturnsOnlyNonemptyValue) {
  Ai_credential_resolver resolver;
  Secure_string credential;

  EXPECT_EQ(Ai_error::k_ok,
            resolver.ReadPlaintextDevForTest(true, "PLAINTEXT_DEV",
                                             "unit-token", &credential));
  EXPECT_EQ("unit-token", credential.view());
}

TEST(AiCredentialResolverTest, PlaintextDevRequiresExactActiveConfig) {
  Ai_credential_resolver resolver;

  EXPECT_TRUE(resolver.IsPlaintextDevConfigForTest(
      9, 3, 9, 3, true, "PLAINTEXT_DEV"));
  EXPECT_FALSE(resolver.IsPlaintextDevConfigForTest(
      9, 3, 9, 4, true, "PLAINTEXT_DEV"));
  EXPECT_FALSE(resolver.IsPlaintextDevConfigForTest(
      9, 3, 9, 3, false, "PLAINTEXT_DEV"));
  EXPECT_FALSE(resolver.IsPlaintextDevConfigForTest(
      9, 3, 9, 3, true, "SECRET_REF"));
}

}  // namespace alisql::ai
