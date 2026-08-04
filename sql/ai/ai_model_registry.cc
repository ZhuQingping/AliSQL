/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include "my_cleanse.h"

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
#include "sql/sql_lex.h"
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
const LEX_CSTRING k_model_config_table = {
    STRING_WITH_LEN("taurusdb_ai_model_config")};

std::string FieldValue(Field *field) {
  if (field->is_null()) return {};
  String value;
  field->val_str(&value, &value);
  return std::string(value.ptr(), value.length());
}

Ai_capability CapabilityFromValue(std::string_view value) {
  return value == "TEXT_EMBEDDING" ? Ai_capability::k_text_embedding
                                    : Ai_capability::k_text_generation;
}

// System_table_access::close_table() commits the supplied THD. Metadata
// lookups can run while an AI function is evaluated inside DML, so they must
// restore the caller's open-table state without committing that DML statement.
void CloseReadOnlySystemTable(THD *thd, TABLE *table,
                              Open_tables_backup *backup) {
  if (table == nullptr) return;
  Query_tables_list query_tables_list_backup;
  thd->lex->reset_n_backup_query_tables_list(&query_tables_list_backup);
  close_thread_tables(thd);
  thd->lex->restore_backup_query_tables_list(&query_tables_list_backup);
  thd->restore_backup_open_tables_state(backup);
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
  if (!value_.empty()) my_cleanse(value_.data(), value_.size());
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
  if (access.open_table(thd, k_mysql_schema, k_model_config_table, 21,
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
            static_cast<uint64_t>(table->field[18]->val_int()),
            FieldValue(table->field[17]) == "ACTIVE",
            FieldValue(table->field[7])))
      continue;
    if (!table->field[9]->is_null()) {
      String plaintext;
      String *value = table->field[9]->val_str(&plaintext, &plaintext);
      if (value != nullptr)
        result = ReadPlaintextDevCredential(
            true, model.credential_kind,
            std::string_view(value->ptr(), value->length()), out);
    }
    break;
  }
  if (!read_error) table->file->ha_rnd_end();
  CloseReadOnlySystemTable(thd, table, &backup);
  if (read_error)
    return Ai_error::k_credential_unavailable;
  return result;
#endif
}
#endif

}  // namespace

Ai_error Ai_model_registry::Resolve(THD *thd, std::string_view model_name,
                                    Ai_capability capability,
                                    Ai_resolved_model *out) const {
  // P0 deliberately has no implicit/default production model.  A caller must
  // name the governed profile it wants to invoke.
  if (thd == nullptr || model_name.empty()) return Ai_error::k_model_not_found;
  std::vector<Ai_model_profile> profiles;
  const Ai_error load_error = LoadProfiles(thd, &profiles);
  if (load_error != Ai_error::k_ok) return load_error;
  return ResolveProfiles(0, model_name, capability, profiles, out);
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
  Open_tables_backup config_backup;
  TABLE *config_table = nullptr;
  if (access.open_table(thd, k_mysql_schema, k_model_config_table, 21,
                        TL_READ, &config_table, &config_backup))
    return Ai_error::k_model_not_found;
  bool read_error = config_table->file->ha_rnd_init(true) != 0;
  while (!read_error) {
    const int scan = config_table->file->ha_rnd_next(config_table->record[0]);
    if (scan == HA_ERR_END_OF_FILE) break;
    if (scan != 0) {
      read_error = true;
      break;
    }
    if (FieldValue(config_table->field[17]) != "ACTIVE") continue;
    Ai_model_profile profile;
    profile.config_id = static_cast<uint64_t>(config_table->field[0]->val_int());
    profile.config_version = static_cast<uint64_t>(config_table->field[18]->val_int());
    profile.model_name = FieldValue(config_table->field[1]);
    profile.provider = FieldValue(config_table->field[2]);
    const std::string capability = FieldValue(config_table->field[3]);
    if (capability != "TEXT_EMBEDDING" && capability != "TEXT_GENERATION")
      continue;
    profile.capability = CapabilityFromValue(capability);
    profile.provider_model_name = FieldValue(config_table->field[4]);
    profile.endpoint_type = "HTTPS_JSON";
    // Endpoint selection is owned by the server.  A stored endpoint is never
    // trusted for a production Huawei profile.
    if (profile.provider == "huawei") {
      profile.endpoint = profile.capability == Ai_capability::k_text_embedding
                             ? "https://api.modelarts-maas.com/v1/embeddings"
                             : "https://api.modelarts-maas.com/v2/chat/completions";
    }
#ifndef NDEBUG
    // Existing MTR fixtures are intentionally table-seeded/offline.  Keep the
    // exception narrow so it cannot create a non-Huawei production route.
    else if ((profile.model_name == "mtr/fixture-embedding" &&
              profile.capability == Ai_capability::k_text_embedding) ||
             (profile.model_name == "mtr/fixture-chat" &&
              profile.capability == Ai_capability::k_text_generation)) {
      profile.endpoint = FieldValue(config_table->field[5]);
    } else {
      continue;
    }
#else
    else {
      continue;
    }
#endif
    profile.credential_kind = FieldValue(config_table->field[7]);
    profile.credential_ref = FieldValue(config_table->field[8]);
    profile.dimension = config_table->field[10]->is_null()
                            ? 0
                            : static_cast<uint32_t>(config_table->field[10]->val_int());
    profile.model_revision = FieldValue(config_table->field[12]);
    profile.active = true;
    profile.is_default = false;
    if (profile.provider == "huawei" &&
        profile.capability == Ai_capability::k_text_embedding &&
        (profile.provider_model_name != "bge-m3" || profile.dimension != 1024))
      continue;
    profiles->push_back(std::move(profile));
  }
  if (!read_error) config_table->file->ha_rnd_end();
  CloseReadOnlySystemTable(thd, config_table, &config_backup);
  if (read_error)
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

  const Ai_model_profile *default_profile = nullptr;
  const Ai_model_profile *named_profile = nullptr;
  for (const auto &profile : profiles) {
    if (!profile.active ||
        (!model_name.empty() && profile.model_name != model_name) ||
        (model_name.empty() && !profile.is_default) ||
        profile.capability != capability || profile.endpoint_type != "HTTPS_JSON" ||
        profile.endpoint.rfind("https://", 0) != 0)
      continue;
    if (model_name.empty()) {
      if (default_profile == nullptr ||
          profile.config_version > default_profile->config_version) {
        default_profile = &profile;
      } else if (profile.config_version == default_profile->config_version) {
        return Ai_error::k_model_not_found;
      }
      continue;
    }
    if (named_profile == nullptr ||
        profile.config_version > named_profile->config_version) {
      named_profile = &profile;
    } else if (profile.config_version == named_profile->config_version) {
      return Ai_error::k_model_not_found;
    }
  }
  const Ai_model_profile *profile =
      model_name.empty() ? default_profile : named_profile;
  if (profile == nullptr) return Ai_error::k_model_not_found;
  out->tenant_id = tenant_id;
  out->config_id = profile->config_id;
  out->config_version = profile->config_version;
  out->dimension = profile->dimension;
  out->capability = profile->capability;
  out->model_name = profile->model_name;
  out->provider = profile->provider;
  out->provider_model_name = profile->provider_model_name;
  out->model_revision = profile->model_revision;
  out->endpoint_type = profile->endpoint_type;
  out->endpoint = profile->endpoint;
  out->embedding_space_id = profile->embedding_space_id;
  out->distance_metric = profile->distance_metric;
  out->credential_kind = profile->credential_kind;
  out->credential_ref = profile->credential_ref;
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
