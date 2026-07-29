/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include "sql/ai/ai_model_registry.h"

#include <algorithm>
#include <utility>

#ifndef EXTRA_CODE_FOR_UNIT_TESTING
#include "keyring_operations_helper.h"
#include "sql/server_component/mysql_server_keyring_lockable_imp.h"
#endif

namespace alisql::ai {

Secure_string::Secure_string(Secure_string &&other) noexcept
    : value_(std::move(other.value_)) {
  other.Clear();
}

Secure_string &Secure_string::operator=(Secure_string &&other) noexcept {
  if (this != &other) {
    Clear();
    value_ = std::move(other.value_);
    other.Clear();
  }
  return *this;
}

Secure_string::~Secure_string() { Clear(); }

void Secure_string::Assign(std::string value) {
  Clear();
  value_ = std::move(value);
}

void Secure_string::Clear() {
  std::fill(value_.begin(), value_.end(), '\0');
  value_.clear();
}

Ai_error Ai_model_registry::Resolve(THD *, std::string_view,
                                    Ai_capability,
                                    Ai_resolved_model *) const {
  // Callers must never substitute an untrusted SQL argument for a configured
  // model. Until the protected-table loader supplies a profile, fail closed.
  return Ai_error::k_model_not_found;
}

void Ai_model_registry::AddProfileForTest(const Ai_model_profile &profile) {
  test_profiles_.push_back(profile);
}

Ai_error Ai_model_registry::ResolveForTest(uint64_t tenant_id,
                                           std::string_view model_name,
                                           Ai_capability capability,
                                           Ai_resolved_model *out) const {
  return ResolveProfiles(tenant_id, model_name, capability, test_profiles_, out);
}

Ai_error Ai_model_registry::ResolveProfiles(
    uint64_t tenant_id, std::string_view model_name, Ai_capability capability,
    const std::vector<Ai_model_profile> &profiles, Ai_resolved_model *out) const {
  if (out == nullptr) return Ai_error::k_provider_error;

  const Ai_model_profile *fallback = nullptr;
  for (const auto &profile : profiles) {
    if (!profile.active || profile.model_name != model_name ||
        profile.capability != capability || profile.endpoint_type != "HTTPS_JSON" ||
        profile.endpoint.rfind("https://", 0) != 0)
      continue;
    if (profile.tenant_id == tenant_id) {
      out->config_id = profile.config_id;
      out->config_version = profile.config_version;
      out->capability = profile.capability;
      out->model_name = profile.model_name;
      out->provider = profile.provider;
      out->provider_model_name = profile.provider_model_name;
      out->model_revision = profile.model_revision;
      out->endpoint_type = profile.endpoint_type;
      out->endpoint = profile.endpoint;
      out->credential_kind = profile.credential_kind;
      out->credential_ref = profile.credential_ref;
      return Ai_error::k_ok;
    }
    if (profile.tenant_id == 0) fallback = &profile;
  }
  if (fallback == nullptr) return Ai_error::k_model_not_found;

  out->config_id = fallback->config_id;
  out->config_version = fallback->config_version;
  out->capability = fallback->capability;
  out->model_name = fallback->model_name;
  out->provider = fallback->provider;
  out->provider_model_name = fallback->provider_model_name;
  out->model_revision = fallback->model_revision;
  out->endpoint_type = fallback->endpoint_type;
  out->endpoint = fallback->endpoint;
  out->credential_kind = fallback->credential_kind;
  out->credential_ref = fallback->credential_ref;
  return Ai_error::k_ok;
}

Ai_error Ai_model_registry::ValidateDimension(
    const Ai_model_profile &profile, uint32_t requested_dimension) const {
  if (profile.model_name == "huawei/bge-m3" && requested_dimension != 1024)
    return Ai_error::k_dimension_mismatch;
  if (profile.dimension != 0 && requested_dimension != 0 &&
      profile.dimension != requested_dimension)
    return Ai_error::k_dimension_mismatch;
  return Ai_error::k_ok;
}

Ai_error Ai_credential_resolver::ReadSecret(const Ai_resolved_model &model,
                                            Secure_string *out) const {
  if (out == nullptr || model.credential_kind != "SECRET_REF" ||
      model.credential_ref.empty())
    return Ai_error::k_credential_unavailable;

#ifdef EXTRA_CODE_FOR_UNIT_TESTING
  // Unit targets must remain offline and do not link a keyring component.
  return Ai_error::k_credential_unavailable;
#else
  if (srv_keyring_reader == nullptr) return Ai_error::k_credential_unavailable;

  unsigned char *secret = nullptr;
  size_t secret_length = 0;
  char *secret_type = nullptr;
  const int result = keyring_operations_helper::read_secret(
      srv_keyring_reader, model.credential_ref.c_str(), nullptr, &secret,
      &secret_length, &secret_type, PSI_INSTRUMENT_ME);
  if (secret_type != nullptr) my_free(secret_type);
  if (result != 1 || secret == nullptr) return Ai_error::k_credential_unavailable;

  out->Assign(std::string(reinterpret_cast<const char *>(secret), secret_length));
  my_free(secret);
  return Ai_error::k_ok;
#endif
}

}  // namespace alisql::ai
