/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#ifndef SQL_AI_AI_MODEL_REGISTRY_INCLUDED
#define SQL_AI_AI_MODEL_REGISTRY_INCLUDED

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sql/ai/ai_types.h"

class THD;

namespace alisql::ai {

/** A non-secret projection of one protected model configuration row. */
struct Ai_model_profile {
  // tenant_id is retained in the in-memory type for source compatibility with
  // existing offline tests. Production resolution is instance scoped and
  // never reads an account or tenant mapping.
  uint64_t tenant_id{0};
  uint64_t config_id{0};
  uint64_t config_version{0};
  uint32_t dimension{0};
  Ai_capability capability{Ai_capability::k_text_generation};
  bool active{false};
  bool is_default{false};
  std::string model_name;
  std::string provider;
  std::string provider_model_name;
  std::string model_revision;
  std::string endpoint_type;
  std::string endpoint;
  std::string embedding_space_id;
  std::string distance_metric;
  std::string credential_kind;
  std::string credential_ref;
};

/**
  Owner of a secret returned by a credential provider.

  The value is deliberately non-copyable and clears its owned storage when
  destroyed. It must never be sent to diagnostics, audit or SQL output.
*/
class Secure_string {
 public:
  Secure_string() = default;
  explicit Secure_string(std::string value) : value_(std::move(value)) {}
  Secure_string(const Secure_string &) = delete;
  Secure_string &operator=(const Secure_string &) = delete;
  Secure_string(Secure_string &&other) noexcept;
  Secure_string &operator=(Secure_string &&other) noexcept;
  ~Secure_string();

  bool empty() const { return value_.empty(); }
  std::string_view view() const { return value_; }
  void Assign(std::string value);
  void Clear();

 private:
  std::string value_;
};

class Ai_model_registry {
 public:
  /** Production entry point. Database-backed resolution is fail-closed. */
  Ai_error Resolve(THD *thd, std::string_view model_name,
                   Ai_capability capability, Ai_resolved_model *out) const;
  /** Test seam used by offline unit and MTR fixtures. */
  void AddProfileForTest(const Ai_model_profile &profile);
  Ai_error ResolveForTest(uint64_t tenant_id, std::string_view model_name,
                          Ai_capability capability,
                          Ai_resolved_model *out) const;
  Ai_error ValidateDimension(const Ai_model_profile &profile,
                             uint32_t requested_dimension) const;

 private:
  Ai_error ResolveProfiles(uint64_t tenant_id, bool populate_legacy_fields,
                           std::string_view model_name, Ai_capability capability,
                           const std::vector<Ai_model_profile> &profiles,
                           Ai_resolved_model *out) const;
  Ai_error LoadProfiles(THD *thd, std::vector<Ai_model_profile> *profiles) const;
  std::vector<Ai_model_profile> test_profiles_;
};

class Ai_credential_resolver {
 public:
  /**
    Read a credential after a model profile has been resolved. This keeps
    credential plumbing separate from SQL argument parsing; SECRET_REF is the
    only production-safe credential kind.
  */
  Ai_error ReadSecret(THD *thd, const Ai_resolved_model &model,
                      Secure_string *out) const;

#ifdef EXTRA_CODE_FOR_UNIT_TESTING
  /** Offline seam for the Debug-only plaintext credential policy. */
  Ai_error ReadPlaintextDevForTest(bool allow_plaintext_dev,
                                   std::string_view credential_kind,
                                   std::string_view plaintext_value,
                                   Secure_string *out) const;
  bool IsPlaintextDevConfigForTest(uint64_t expected_config_id,
                                   uint64_t expected_config_version,
                                   uint64_t actual_config_id,
                                   uint64_t actual_config_version, bool active,
                                   std::string_view credential_kind) const;
#endif
};

}  // namespace alisql::ai

#endif  // SQL_AI_AI_MODEL_REGISTRY_INCLUDED
