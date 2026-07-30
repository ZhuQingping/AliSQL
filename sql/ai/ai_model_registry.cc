/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include "sql/ai/ai_model_registry.h"

#include <algorithm>
#include <utility>

#ifndef EXTRA_CODE_FOR_UNIT_TESTING
#include "sql/handler.h"
#include "sql/field.h"
#include "keyring_operations_helper.h"
#include "sql/rpl_table_access.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/server_component/mysql_server_keyring_lockable_imp.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/table.h"
#endif

namespace alisql::ai {

namespace {

#ifndef EXTRA_CODE_FOR_UNIT_TESTING
class Ai_system_table_access final : public System_table_access {
 public:
  void before_open(THD *) override {
    m_flags = MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK | MYSQL_OPEN_IGNORE_FLUSH |
              MYSQL_LOCK_IGNORE_TIMEOUT;
  }
};

const LEX_CSTRING k_mysql_schema = {STRING_WITH_LEN("mysql")};
const LEX_CSTRING k_tenant_binding_table = {
    STRING_WITH_LEN("alisql_ai_tenant_binding")};
const LEX_CSTRING k_tenant_account_table = {
    STRING_WITH_LEN("alisql_ai_tenant_account")};
const LEX_CSTRING k_model_config_table = {
    STRING_WITH_LEN("alisql_ai_model_config")};

std::string FieldValue(Field *field) {
  if (field->is_null()) return {};
  String value;
  field->val_str(&value, &value);
  return std::string(value.ptr(), value.length());
}

Ai_capability CapabilityFromEnum(longlong value) {
  return value == 1 ? Ai_capability::k_text_embedding
                    : Ai_capability::k_text_generation;
}
#endif

}  // namespace

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

namespace {

bool IsPlaintextDevConfig(uint64_t expected_config_id,
                          uint64_t expected_config_version,
                          uint64_t actual_config_id,
                          uint64_t actual_config_version, bool active,
                          std::string_view credential_kind) {
  return expected_config_id == actual_config_id &&
         expected_config_version == actual_config_version && active &&
         credential_kind == "PLAINTEXT_DEV";
}

Ai_error ReadPlaintextDevCredential(bool allow_plaintext_dev,
                                    std::string_view credential_kind,
                                    std::string_view plaintext_value,
                                    Secure_string *out) {
  if (!allow_plaintext_dev || out == nullptr ||
      credential_kind != "PLAINTEXT_DEV" || plaintext_value.empty())
    return Ai_error::k_credential_unavailable;
  out->Assign(std::string(plaintext_value));
  return Ai_error::k_ok;
}

#ifndef EXTRA_CODE_FOR_UNIT_TESTING
Ai_error ReadPlaintextDevFromSystemTable(THD *thd,
                                         const Ai_resolved_model &model,
                                         Secure_string *out) {
#ifdef NDEBUG
  (void)thd;
  (void)model;
  (void)out;
  return Ai_error::k_credential_unavailable;
#else
  if (thd == nullptr || out == nullptr) return Ai_error::k_credential_unavailable;
  Ai_system_table_access access;
  Open_tables_backup backup;
  TABLE *table = nullptr;
  if (access.open_table(thd, k_mysql_schema, k_model_config_table, 16,
                        TL_READ, &table, &backup))
    return Ai_error::k_credential_unavailable;

  bool read_error = table->file->ha_rnd_init(true) != 0;
  Ai_error result = Ai_error::k_credential_unavailable;
  while (!read_error) {
    const int scan = table->file->ha_rnd_next(table->record[0]);
    if (scan == HA_ERR_END_OF_FILE) break;
    if (scan != 0) {
      read_error = true;
      break;
    }
    if (!IsPlaintextDevConfig(
            model.config_id, model.config_version,
            static_cast<uint64_t>(table->field[0]->val_int()),
            static_cast<uint64_t>(table->field[1]->val_int()),
            table->field[15]->val_int() != 0, FieldValue(table->field[12])))
      continue;
    if (!table->field[14]->is_null()) {
      String plaintext;
      String *value = table->field[14]->val_str(&plaintext, &plaintext);
      if (value != nullptr)
        result = ReadPlaintextDevCredential(
            true, model.credential_kind,
            std::string_view(value->ptr(), value->length()), out);
    }
    break;
  }
  if (!read_error) table->file->ha_rnd_end();
  if (access.close_table(thd, table, &backup, read_error, false) || read_error)
    return Ai_error::k_credential_unavailable;
  return result;
#endif
}
#endif

}  // namespace

Ai_error Ai_model_registry::Resolve(THD *thd, std::string_view model_name,
                                    Ai_capability capability,
                                    Ai_resolved_model *out) const {
  if (thd == nullptr) return Ai_error::k_model_not_found;
  uint64_t tenant_id = 0;
  const Ai_error tenant_error = ResolveTenant(thd, &tenant_id);
  if (tenant_error != Ai_error::k_ok) return tenant_error;
  std::vector<Ai_model_profile> profiles;
  const Ai_error load_error = LoadProfiles(thd, &profiles);
  if (load_error != Ai_error::k_ok) return load_error;
  return ResolveProfiles(tenant_id, model_name, capability, profiles, out);
}

Ai_error Ai_model_registry::ResolveTenant(THD *thd, uint64_t *tenant_id) const {
#ifdef EXTRA_CODE_FOR_UNIT_TESTING
  (void)thd;
  if (tenant_id == nullptr) return Ai_error::k_provider_error;
  *tenant_id = 0;
  return Ai_error::k_ok;
#else
  if (tenant_id == nullptr || thd == nullptr || thd->security_context() == nullptr)
    return Ai_error::k_model_not_found;
  *tenant_id = 0;
  const auto user = thd->security_context()->priv_user();
  const auto host = thd->security_context()->priv_host();
  Ai_system_table_access access;
  Open_tables_backup backup;
  TABLE *table = nullptr;
  if (access.open_table(thd, k_mysql_schema, k_tenant_account_table, 4,
                        TL_READ, &table, &backup))
    return Ai_error::k_model_not_found;
  bool read_error = table->file->ha_rnd_init(true) != 0;
  bool found = false;
  while (!read_error) {
    const int scan = table->file->ha_rnd_next(table->record[0]);
    if (scan == HA_ERR_END_OF_FILE) break;
    if (scan != 0) {
      read_error = true;
      break;
    }
    if (table->field[3]->val_int() == 0 ||
        FieldValue(table->field[0]) != std::string(user.str, user.length) ||
        FieldValue(table->field[1]) != std::string(host.str, host.length))
      continue;
    *tenant_id = static_cast<uint64_t>(table->field[2]->val_int());
    found = true;
    break;
  }
  if (!read_error) table->file->ha_rnd_end();
  if (access.close_table(thd, table, &backup, read_error, false) || read_error)
    return Ai_error::k_model_not_found;
  // An absent account mapping is the explicit global tenant fallback.
  (void)found;
  return Ai_error::k_ok;
#endif
}

Ai_error Ai_model_registry::LoadProfiles(
    THD *thd, std::vector<Ai_model_profile> *profiles) const {
#ifdef EXTRA_CODE_FOR_UNIT_TESTING
  (void)thd;
  (void)profiles;
  // GUnit runs offline and deliberately has no table handler linkage.
  return Ai_error::k_model_not_found;
#else
  if (profiles == nullptr) return Ai_error::k_provider_error;
  Ai_system_table_access access;
  Open_tables_backup binding_backup;
  TABLE *binding_table = nullptr;
  if (access.open_table(thd, k_mysql_schema, k_tenant_binding_table, 5,
                        TL_READ, &binding_table, &binding_backup))
    return Ai_error::k_model_not_found;

  struct Binding { uint64_t tenant_id; std::string model_name; Ai_capability capability; uint64_t config_id; };
  std::vector<Binding> bindings;
  bool read_error = binding_table->file->ha_rnd_init(true) != 0;
  while (!read_error) {
    const int scan = binding_table->file->ha_rnd_next(binding_table->record[0]);
    if (scan == HA_ERR_END_OF_FILE) break;
    if (scan != 0) {
      read_error = true;
      break;
    }
    if (binding_table->field[4]->val_int() == 0) continue;
    bindings.push_back({static_cast<uint64_t>(binding_table->field[0]->val_int()),
                        FieldValue(binding_table->field[1]),
                        CapabilityFromEnum(binding_table->field[2]->val_int()),
                        static_cast<uint64_t>(binding_table->field[3]->val_int())});
  }
  if (!read_error) binding_table->file->ha_rnd_end();
  if (access.close_table(thd, binding_table, &binding_backup, read_error, false))
    return Ai_error::k_model_not_found;

  Open_tables_backup config_backup;
  TABLE *config_table = nullptr;
  if (access.open_table(thd, k_mysql_schema, k_model_config_table, 16,
                        TL_READ, &config_table, &config_backup))
    return Ai_error::k_model_not_found;
  read_error = config_table->file->ha_rnd_init(true) != 0;
  while (!read_error) {
    const int scan = config_table->file->ha_rnd_next(config_table->record[0]);
    if (scan == HA_ERR_END_OF_FILE) break;
    if (scan != 0) {
      read_error = true;
      break;
    }
    const uint64_t config_id = static_cast<uint64_t>(config_table->field[0]->val_int());
    if (config_table->field[15]->val_int() == 0) continue;
    for (const auto &binding : bindings) {
      if (binding.config_id != config_id) continue;
      Ai_model_profile profile;
      profile.tenant_id = binding.tenant_id;
      profile.config_id = config_id;
      profile.config_version = static_cast<uint64_t>(config_table->field[1]->val_int());
      profile.model_name = FieldValue(config_table->field[2]);
      profile.capability = CapabilityFromEnum(config_table->field[3]->val_int());
      profile.provider = FieldValue(config_table->field[4]);
      profile.provider_model_name = FieldValue(config_table->field[5]);
      profile.model_revision = FieldValue(config_table->field[6]);
      profile.endpoint_type = FieldValue(config_table->field[7]);
      profile.endpoint = FieldValue(config_table->field[8]);
      profile.dimension = config_table->field[9]->is_null() ? 0 : static_cast<uint32_t>(config_table->field[9]->val_int());
      profile.embedding_space_id = FieldValue(config_table->field[10]);
      profile.distance_metric = FieldValue(config_table->field[11]);
      profile.credential_kind = FieldValue(config_table->field[12]);
      profile.credential_ref = FieldValue(config_table->field[13]);
      profile.active = profile.model_name == binding.model_name && profile.capability == binding.capability;
      if (profile.active) profiles->push_back(std::move(profile));
    }
  }
  if (!read_error) config_table->file->ha_rnd_end();
  if (access.close_table(thd, config_table, &config_backup, read_error, false))
    return Ai_error::k_model_not_found;
  return read_error ? Ai_error::k_model_not_found : Ai_error::k_ok;
#endif
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
      out->dimension = profile.dimension;
      out->capability = profile.capability;
      out->model_name = profile.model_name;
      out->provider = profile.provider;
      out->provider_model_name = profile.provider_model_name;
      out->model_revision = profile.model_revision;
      out->endpoint_type = profile.endpoint_type;
      out->endpoint = profile.endpoint;
      out->embedding_space_id = profile.embedding_space_id;
      out->distance_metric = profile.distance_metric;
      out->credential_kind = profile.credential_kind;
      out->credential_ref = profile.credential_ref;
      return Ai_error::k_ok;
    }
    if (profile.tenant_id == 0) fallback = &profile;
  }
  if (fallback == nullptr) return Ai_error::k_model_not_found;

  out->config_id = fallback->config_id;
  out->config_version = fallback->config_version;
  out->dimension = fallback->dimension;
  out->capability = fallback->capability;
  out->model_name = fallback->model_name;
  out->provider = fallback->provider;
  out->provider_model_name = fallback->provider_model_name;
  out->model_revision = fallback->model_revision;
  out->endpoint_type = fallback->endpoint_type;
  out->endpoint = fallback->endpoint;
  out->embedding_space_id = fallback->embedding_space_id;
  out->distance_metric = fallback->distance_metric;
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

Ai_error Ai_credential_resolver::ReadSecret(THD *thd,
                                            const Ai_resolved_model &model,
                                            Secure_string *out) const {
  if (out == nullptr) return Ai_error::k_credential_unavailable;
  if (model.credential_kind == "PLAINTEXT_DEV") {
#ifdef EXTRA_CODE_FOR_UNIT_TESTING
    (void)thd;
    return Ai_error::k_credential_unavailable;
#else
    return ReadPlaintextDevFromSystemTable(thd, model, out);
#endif
  }
  if (model.credential_kind != "SECRET_REF" || model.credential_ref.empty())
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

#ifdef EXTRA_CODE_FOR_UNIT_TESTING
Ai_error Ai_credential_resolver::ReadPlaintextDevForTest(
    bool allow_plaintext_dev, std::string_view credential_kind,
    std::string_view plaintext_value, Secure_string *out) const {
  return ReadPlaintextDevCredential(allow_plaintext_dev, credential_kind,
                                    plaintext_value, out);
}

bool Ai_credential_resolver::IsPlaintextDevConfigForTest(
    uint64_t expected_config_id, uint64_t expected_config_version,
    uint64_t actual_config_id, uint64_t actual_config_version, bool active,
    std::string_view credential_kind) const {
  return IsPlaintextDevConfig(expected_config_id, expected_config_version,
                              actual_config_id, actual_config_version, active,
                              credential_kind);
}
#endif

}  // namespace alisql::ai
