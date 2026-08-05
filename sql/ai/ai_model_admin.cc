/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include "sql/ai/ai_model_admin.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <my_rapidjson_size_t.h>
#include <rapidjson/document.h>

#include "my_dbug.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/derror.h"
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/item.h"
#include "sql/ai/ai_runtime.h"
#include "sql/ai/ai_provider_endpoint_policy.h"
#include "sql/protocol.h"
#include "sql/rpl_table_access.h"
#include "sql/sql_class.h"
#include "sql/sql_base.h"
#include "sql/table.h"

namespace alisql::ai {
const LEX_CSTRING AI_MODEL_ADMIN_PROC_SCHEMA = {STRING_WITH_LEN("dbms_ai")};

namespace {
constexpr uint k_fields = 12;
const LEX_CSTRING k_mysql = {STRING_WITH_LEN("mysql")};
const LEX_CSTRING k_table_name = {STRING_WITH_LEN("ai_model_config")};

class Ai_system_table_access final : public System_table_access {
 public:
  void before_open(THD *) override {
    m_flags = MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK | MYSQL_OPEN_IGNORE_FLUSH |
              MYSQL_LOCK_IGNORE_TIMEOUT;
  }
};

std::string Value(Field *field) {
  if (field->is_null()) return {};
  String value;
  String *result = field->val_str(&value, &value);
  return result == nullptr ? std::string() : std::string(result->ptr(), result->length());
}
const char *CapabilityName(Ai_capability capability) {
  return capability == Ai_capability::k_text_embedding ? "TEXT_EMBEDDING" : "TEXT_GENERATION";
}
bool ParseCapability(const std::string &value, Ai_capability *capability) {
  if (value == "TEXT_EMBEDDING") { *capability = Ai_capability::k_text_embedding; return true; }
  if (value == "TEXT_GENERATION") { *capability = Ai_capability::k_text_generation; return true; }
  return false;
}
bool IsDebugMtrFixtureName(const Ai_model_admin_request &request) {
#ifndef NDEBUG
  return (request.model_name == "mtr/fixture-embedding" &&
          request.capability == Ai_capability::k_text_embedding) ||
         (request.model_name == "mtr/fixture-chat" &&
          request.capability == Ai_capability::k_text_generation);
#else
  (void)request;
  return false;
#endif
}
bool IsDebugMtrFixture(const Ai_model_admin_request &request) {
  return request.provider == "mtr" && IsDebugMtrFixtureName(request) &&
         ((request.capability == Ai_capability::k_text_embedding &&
           request.provider_model_name == "fixture-embedding" &&
           request.endpoint_url ==
               "https://db4ai-mtr-fixture.invalid/v1/embeddings") ||
          (request.capability == Ai_capability::k_text_generation &&
           request.provider_model_name == "fixture-chat" &&
           request.endpoint_url ==
               "https://db4ai-mtr-fixture.invalid/v2/chat/completions"));
}
bool IsEmptyJsonObject(const std::string &value) {
  rapidjson::Document document;
  document.Parse(value.data(), value.size());
  // P0 has no provider-private knobs.  Keeping this object empty prevents a
  // credential, Authorization header, URL override, or future option from
  // being stored as opaque model metadata. A later Adapter can publish a
  // narrow per-provider schema together with its credential resolver.
  return !document.HasParseError() && document.IsObject() &&
         document.MemberCount() == 0;
}
bool ValidRequest(THD *thd, const Ai_model_admin_request &request) {
  (void)thd;
  if (request.model_name.empty() || request.provider_model_name.empty() ||
      request.provider.empty() || request.endpoint_url.empty() ||
      request.provider_options.empty() ||
      !IsEmptyJsonObject(request.provider_options))
    return false;
  if (IsDebugMtrFixture(request)) return true;
  if (Provider_endpoint_policy::Validate(request.provider, request.capability,
                                         request.endpoint_url,
                                         request.provider_options) !=
      Ai_error::k_ok)
    return false;
  if (request.capability == Ai_capability::k_text_embedding &&
      (request.provider_model_name != "bge-m3" || request.dimension != 1024))
    return false;
  return true;
}
void Store(Field *field, const std::string &value) {
  field->store(value.c_str(), value.length(), system_charset_info);
}
void StoreText(Field *field, const char *value) { field->store(value, std::strlen(value), system_charset_info); }
void WriteRecord(TABLE *table, const Ai_model_admin_request &request, uint64_t version) {
  std::memcpy(table->record[0], table->s->default_values, table->s->reclength);
  table->next_number_field = table->found_next_number_field;
  table->field[0]->set_null();
  Store(table->field[1], request.model_name);
  Store(table->field[2], request.provider);
  StoreText(table->field[3], CapabilityName(request.capability));
  Store(table->field[4], request.provider_model_name);
  Store(table->field[5], request.endpoint_url);
  if (request.capability == Ai_capability::k_text_embedding) {
    table->field[6]->set_notnull();
    table->field[6]->store(request.dimension, false);
  } else
    table->field[6]->set_null();
  Store(table->field[7], request.provider_options);
  StoreText(table->field[8], "ACTIVE");
  table->field[9]->store(version, true);
}
bool Match(TABLE *table, const Ai_model_admin_request &request) {
  return Value(table->field[1]) == request.model_name &&
         Value(table->field[3]) == CapabilityName(request.capability);
}
bool Open(Ai_system_table_access *access, THD *thd, TABLE **table, Open_tables_backup *backup) {
  return access->open_table(thd, k_mysql, k_table_name, k_fields, TL_WRITE, table, backup);
}
bool CloseWrite(Ai_system_table_access *access, THD *thd, TABLE *table,
                Open_tables_backup *backup, bool error) {
  // Handler failures are intentionally redacted, but an admin command must
  // still leave a diagnostic for the command dispatcher.
  if (error && !thd->is_error()) my_error(ER_UNKNOWN_ERROR, MYF(0));
  const bool close_error = access->close_table(thd, table, backup, error, true);
  if ((error || close_error) && !thd->is_error()) my_error(ER_UNKNOWN_ERROR, MYF(0));
  return error || close_error;
}
void ResetAutoIncrement(TABLE *table) {
  table->file->ha_release_auto_increment();
  if (table->next_number_field != nullptr) {
    table->next_number_field->reset();
    table->next_number_field = nullptr;
  }
}
bool Register(THD *thd, const Ai_model_admin_request &request) {
  if (!ValidRequest(thd, request)) { my_error(ER_WRONG_ARGUMENTS, MYF(0), "dbms_ai model request"); return true; }
  Ai_system_table_access access; Open_tables_backup backup; TABLE *table = nullptr;
  if (Open(&access, thd, &table, &backup)) return true;
  bool error = table->file->ha_rnd_init(true) != 0;
  const bool scan_initialized = !error;
  uint64_t latest_version = 0;
  while (!error) { int rc = table->file->ha_rnd_next(table->record[0]); if (rc == HA_ERR_END_OF_FILE) break; if (rc) { error = true; break; } const bool same_name = Value(table->field[1]) == request.model_name; const bool active = Value(table->field[8]) == "ACTIVE"; if (same_name && active && !Match(table, request)) { my_error(ER_WRONG_ARGUMENTS, MYF(0), "ambiguous dbms_ai model"); error = true; break; } if (!Match(table, request)) continue; latest_version = std::max(latest_version, static_cast<uint64_t>(table->field[9]->val_int())); if (active) { my_error(ER_WRONG_ARGUMENTS, MYF(0), "duplicate dbms_ai model"); error = true; break; } }
  if (scan_initialized) table->file->ha_rnd_end();
  if (!error) { WriteRecord(table, request, latest_version + 1); error = table->file->update_auto_increment() != 0 || table->file->ha_write_row(table->record[0]) != 0; }
  ResetAutoIncrement(table);
  return CloseWrite(&access, thd, table, &backup, error);
}
bool Update(THD *thd, const Ai_model_admin_request &request) {
  if (!ValidRequest(thd, request)) { my_error(ER_WRONG_ARGUMENTS, MYF(0), "dbms_ai model request"); return true; }
  Ai_system_table_access access; Open_tables_backup backup; TABLE *table = nullptr;
  if (Open(&access, thd, &table, &backup)) return true;
  bool error = table->file->ha_rnd_init(true) != 0; uint64_t version = 0; bool found = false;
  const bool scan_initialized = !error;
  while (!error) { int rc = table->file->ha_rnd_next(table->record[0]); if (rc == HA_ERR_END_OF_FILE) break; if (rc) { error = true; break; } if (Match(table, request) && Value(table->field[8]) == "ACTIVE") { found = true; version = static_cast<uint64_t>(table->field[9]->val_int()); std::memcpy(table->record[1], table->record[0], table->s->reclength); break; } }
  if (scan_initialized) table->file->ha_rnd_end();
  if (!error && !found) { my_error(ER_WRONG_ARGUMENTS, MYF(0), "unknown dbms_ai model"); error = true; }
  if (!error) { std::memcpy(table->record[0], table->record[1], table->s->reclength); StoreText(table->field[8], "DISABLED"); error = table->file->ha_update_row(table->record[1], table->record[0]) != 0; }
  if (!error) { WriteRecord(table, request, version + 1); error = table->file->update_auto_increment() != 0 || table->file->ha_write_row(table->record[0]) != 0; }
  ResetAutoIncrement(table);
  return CloseWrite(&access, thd, table, &backup, error);
}
bool Delete(THD *thd, const Ai_model_admin_request &request) {
  Ai_system_table_access access; Open_tables_backup backup; TABLE *table = nullptr;
  if (Open(&access, thd, &table, &backup)) return true;
  bool error = table->file->ha_rnd_init(true) != 0;
  const bool scan_initialized = !error;
  while (!error) {
    const int rc = table->file->ha_rnd_next(table->record[0]);
    if (rc == HA_ERR_END_OF_FILE) break;
    if (rc) {
      error = true;
      break;
    }
    if (!Match(table, request)) continue;

    std::memcpy(table->record[1], table->record[0], table->s->reclength);
    if (IsDebugMtrFixtureName(request)) {
      if (table->file->ha_delete_row(table->record[1])) {
        error = true;
        break;
      }
      continue;
    }

    std::memcpy(table->record[0], table->record[1], table->s->reclength);
    StoreText(table->field[8], "RETIRED");
    if (table->file->ha_update_row(table->record[1], table->record[0])) {
      error = true;
      break;
    }
  }
  if (scan_initialized) table->file->ha_rnd_end();
  return CloseWrite(&access, thd, table, &backup, error);
}
struct Safe_row { std::string name, capability, provider_model; bool has_dimension; uint64_t dimension, version; };
bool ReadRows(THD *thd, std::vector<Safe_row> *rows) {
  Ai_system_table_access access; Open_tables_backup backup; TABLE *table = nullptr;
  if (access.open_table(thd, k_mysql, k_table_name, k_fields, TL_READ, &table, &backup)) return true;
  bool error = table->file->ha_rnd_init(true) != 0;
  while (!error) { int rc = table->file->ha_rnd_next(table->record[0]); if (rc == HA_ERR_END_OF_FILE) break; if (rc) { error = true; break; } if (Value(table->field[8]) != "ACTIVE") continue; rows->push_back({Value(table->field[1]), Value(table->field[3]), Value(table->field[4]), !table->field[6]->is_null(), table->field[6]->is_null() ? 0 : static_cast<uint64_t>(table->field[6]->val_int()), static_cast<uint64_t>(table->field[9]->val_int())}); }
  if (!error) table->file->ha_rnd_end();
  const bool close_error = access.close_table(thd, table, &backup, error, false);
  if ((error || close_error) && !thd->is_error()) my_error(ER_UNKNOWN_ERROR, MYF(0));
  return error || close_error;
}
bool ReadRequest(mem_root_deque<Item *> *list, Ai_model_admin_request *request, bool delete_only) {
  if (list == nullptr || list->size() != (delete_only ? 2U : 7U)) return false;
  String storage; String *name = (*list)[0]->val_str(&storage); if (name == nullptr) return false;
  request->model_name.assign(name->ptr(), name->length());
  String cap_storage; String *cap = (*list)[1]->val_str(&cap_storage); if (cap == nullptr || !ParseCapability(std::string(cap->ptr(), cap->length()), &request->capability)) return false;
  if (delete_only) return !request->model_name.empty();
  String provider_storage, model_storage, endpoint_storage, options_storage;
  String *provider = (*list)[2]->val_str(&provider_storage);
  String *model = (*list)[3]->val_str(&model_storage);
  String *endpoint = (*list)[4]->val_str(&endpoint_storage);
  String *options = (*list)[6]->val_str(&options_storage);
  const longlong dimension = (*list)[5]->val_int();
  if (provider == nullptr || model == nullptr || endpoint == nullptr ||
      options == nullptr || (*list)[5]->null_value || dimension < 0 ||
      static_cast<ulonglong>(dimension) > UINT32_MAX)
    return false;
  request->provider.assign(provider->ptr(), provider->length());
  request->provider_model_name.assign(model->ptr(), model->length());
  request->endpoint_url.assign(endpoint->ptr(), endpoint->length());
  request->dimension = static_cast<uint32_t>(dimension);
  request->provider_options.assign(options->ptr(), options->length());
  return !request->model_name.empty();
}
class Sql_cmd_ai_model_admin final : public im::Sql_cmd_admin_proc {
 public:
  Sql_cmd_ai_model_admin(THD *thd, mem_root_deque<Item *> *list, const Ai_model_admin_proc *proc) : Sql_cmd_admin_proc(thd, list, proc), operation_(proc->operation()) {}
  bool check_access(THD *thd) override { if (operation_ != Ai_model_admin_proc::Operation::k_show && !IsAiMaaSEnabled()) { my_error(ER_NOT_SUPPORTED_YET, MYF(0), "DB4AI MaaS feature is disabled"); return true; } auto *sctx = thd->security_context(); if (sctx == nullptr || !sctx->has_global_grant(STRING_WITH_LEN("AI_ADMIN")).first) { my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), "AI_ADMIN"); return true; } return false; }
  bool pc_execute(THD *thd) override { if (operation_ == Ai_model_admin_proc::Operation::k_show) return false; if (!IsAiMaaSEnabled()) { my_error(ER_NOT_SUPPORTED_YET, MYF(0), "DB4AI MaaS feature is disabled"); return true; } Ai_model_admin_request request; if (!ReadRequest(m_list, &request, operation_ == Ai_model_admin_proc::Operation::k_delete)) { my_error(ER_WRONG_ARGUMENTS, MYF(0), "dbms_ai model request"); return true; } return operation_ == Ai_model_admin_proc::Operation::k_register ? Register(thd, request) : operation_ == Ai_model_admin_proc::Operation::k_update ? Update(thd, request) : Delete(thd, request); }
  void send_result(THD *thd, bool error) override { if (error) return; if (operation_ != Ai_model_admin_proc::Operation::k_show) { my_ok(thd); return; } std::vector<Safe_row> rows; if (ReadRows(thd, &rows)) return; if (m_proc->send_result_metadata(thd)) return; for (const auto &row : rows) { Protocol *p = thd->get_protocol(); p->start_row(); p->store_string(row.name.c_str(), row.name.size(), system_charset_info); p->store_string(row.capability.c_str(), row.capability.size(), system_charset_info); p->store_string(row.provider_model.c_str(), row.provider_model.size(), system_charset_info); if (row.has_dimension) p->store_longlong(row.dimension, true); else p->store_null(); p->store_longlong(row.version, true); if (p->end_row()) return; } my_eof(thd); }
 private: Ai_model_admin_proc::Operation operation_;
};
}  // namespace

Ai_model_admin_proc::Ai_model_admin_proc(Operation operation, const char *name) : Proc(0), operation_(operation), name_(name) {
  if (operation == Operation::k_show) { m_result_type = Result_type::RESULT_SET; Column_element columns[] = {{MYSQL_TYPE_VARCHAR, STRING_WITH_LEN("MODEL_NAME"), 255}, {MYSQL_TYPE_VARCHAR, STRING_WITH_LEN("CAPABILITY"), 32}, {MYSQL_TYPE_VARCHAR, STRING_WITH_LEN("PROVIDER_MODEL_NAME"), 255}, {MYSQL_TYPE_LONGLONG, STRING_WITH_LEN("DIMENSION"), 0}, {MYSQL_TYPE_LONGLONG, STRING_WITH_LEN("CONFIG_VERSION"), 0}}; for (size_t i = 0; i < 5; ++i) m_columns.assign_at(i, columns[i]); } else { m_result_type = Result_type::RESULT_OK; const size_t n = operation == Operation::k_delete ? 2 : 7; for (size_t i = 0; i < n; ++i) m_parameters.assign_at(i, i == 5 ? MYSQL_TYPE_LONGLONG : MYSQL_TYPE_VARCHAR); }
}
Sql_cmd *Ai_model_admin_proc::evoke_cmd(THD *thd, mem_root_deque<Item *> *list) const { return new (thd->mem_root) Sql_cmd_ai_model_admin(thd, list, this); }
const std::string Ai_model_admin_proc::str() const { return name_; }
const std::string Ai_model_admin_proc::qname() const { return std::string(AI_MODEL_ADMIN_PROC_SCHEMA.str) + "." + name_; }
Ai_model_register_proc::Ai_model_register_proc() : Ai_model_admin_proc(Operation::k_register, "register_model") {} im::Proc *Ai_model_register_proc::instance() { static im::Proc *p = new Ai_model_register_proc(); return p; }
Ai_model_update_proc::Ai_model_update_proc() : Ai_model_admin_proc(Operation::k_update, "update_model") {} im::Proc *Ai_model_update_proc::instance() { static im::Proc *p = new Ai_model_update_proc(); return p; }
Ai_model_delete_proc::Ai_model_delete_proc() : Ai_model_admin_proc(Operation::k_delete, "delete_model") {} im::Proc *Ai_model_delete_proc::instance() { static im::Proc *p = new Ai_model_delete_proc(); return p; }
Ai_model_show_proc::Ai_model_show_proc() : Ai_model_admin_proc(Operation::k_show, "show_models") {} im::Proc *Ai_model_show_proc::instance() { static im::Proc *p = new Ai_model_show_proc(); return p; }
}  // namespace alisql::ai
