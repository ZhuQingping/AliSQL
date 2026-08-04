/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include "sql/ai/ai_model_admin.h"

#include <cstring>
#include <vector>

#include "my_dbug.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/derror.h"
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/item.h"
#include "sql/protocol.h"
#include "sql/rpl_table_access.h"
#include "sql/sql_class.h"
#include "sql/sql_base.h"
#include "sql/table.h"

namespace alisql::ai {
const LEX_CSTRING AI_MODEL_ADMIN_PROC_SCHEMA = {STRING_WITH_LEN("dbms_ai")};

namespace {
constexpr uint k_fields = 21;
const LEX_CSTRING k_mysql = {STRING_WITH_LEN("mysql")};
const LEX_CSTRING k_table_name = {STRING_WITH_LEN("taurusdb_ai_model_config")};

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
const char *Endpoint(Ai_capability capability) {
  return capability == Ai_capability::k_text_embedding
             ? "https://api.modelarts-maas.com/v1/embeddings"
             : "https://api.modelarts-maas.com/v2/chat/completions";
}
bool ValidRequest(const Ai_model_admin_request &request) {
  if (request.model_name.empty() || request.provider_model_name.empty() ||
      request.credential_value.empty()) return false;
  if (request.credential_mode != "SECRET_REF" && request.credential_mode != "PLAINTEXT_DEV") return false;
#ifdef NDEBUG
  if (request.credential_mode == "PLAINTEXT_DEV") return false;
#endif
  return request.capability == Ai_capability::k_text_generation ||
         (request.capability == Ai_capability::k_text_embedding &&
          request.provider_model_name == "bge-m3");
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
  StoreText(table->field[2], "huawei");
  StoreText(table->field[3], CapabilityName(request.capability));
  Store(table->field[4], request.provider_model_name);
  StoreText(table->field[5], Endpoint(request.capability));
  StoreText(table->field[6], "BEARER_API_KEY");
  Store(table->field[7], request.credential_mode);
  if (request.credential_mode == "SECRET_REF") {
    Store(table->field[8], std::string(request.credential_value.view()));
    table->field[9]->set_null();
  } else {
    table->field[8]->set_null();
    Store(table->field[9], std::string(request.credential_value.view()));
  }
  if (request.capability == Ai_capability::k_text_embedding)
    table->field[10]->store(1024, false);
  else
    table->field[10]->set_null();
  table->field[11]->set_null(); table->field[12]->set_null();
  table->field[13]->set_null(); table->field[14]->set_null();
  table->field[15]->store(0, false); table->field[16]->store(0, false);
  StoreText(table->field[17], "ACTIVE");
  table->field[18]->store(version, true);
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
  if (!ValidRequest(request)) { my_error(ER_WRONG_ARGUMENTS, MYF(0), "dbms_ai model request"); return true; }
  Ai_system_table_access access; Open_tables_backup backup; TABLE *table = nullptr;
  if (Open(&access, thd, &table, &backup)) return true;
  bool error = table->file->ha_rnd_init(true) != 0;
  while (!error) { int rc = table->file->ha_rnd_next(table->record[0]); if (rc == HA_ERR_END_OF_FILE) break; if (rc) { error = true; break; } if (Match(table, request)) { my_error(ER_WRONG_ARGUMENTS, MYF(0), "duplicate dbms_ai model"); error = true; break; } }
  if (!error) table->file->ha_rnd_end();
  if (!error) { WriteRecord(table, request, 1); error = table->file->update_auto_increment() != 0 || table->file->ha_write_row(table->record[0]) != 0; }
  ResetAutoIncrement(table);
  return CloseWrite(&access, thd, table, &backup, error);
}
bool Update(THD *thd, const Ai_model_admin_request &request) {
  if (!ValidRequest(request)) { my_error(ER_WRONG_ARGUMENTS, MYF(0), "dbms_ai model request"); return true; }
  Ai_system_table_access access; Open_tables_backup backup; TABLE *table = nullptr;
  if (Open(&access, thd, &table, &backup)) return true;
  bool error = table->file->ha_rnd_init(true) != 0; uint64_t version = 0; bool found = false;
  while (!error) { int rc = table->file->ha_rnd_next(table->record[0]); if (rc == HA_ERR_END_OF_FILE) break; if (rc) { error = true; break; } if (Match(table, request) && Value(table->field[17]) == "ACTIVE") { found = true; version = static_cast<uint64_t>(table->field[18]->val_int()); std::memcpy(table->record[1], table->record[0], table->s->reclength); break; } }
  if (!error) table->file->ha_rnd_end();
  if (!error && !found) { my_error(ER_WRONG_ARGUMENTS, MYF(0), "unknown dbms_ai model"); error = true; }
  if (!error) { std::memcpy(table->record[0], table->record[1], table->s->reclength); StoreText(table->field[17], "DISABLED"); error = table->file->ha_update_row(table->record[1], table->record[0]) != 0; }
  if (!error) { WriteRecord(table, request, version + 1); error = table->file->update_auto_increment() != 0 || table->file->ha_write_row(table->record[0]) != 0; }
  ResetAutoIncrement(table);
  return CloseWrite(&access, thd, table, &backup, error);
}
bool Delete(THD *thd, const Ai_model_admin_request &request) {
  Ai_system_table_access access; Open_tables_backup backup; TABLE *table = nullptr;
  if (Open(&access, thd, &table, &backup)) return true;
  bool error = table->file->ha_rnd_init(true) != 0;
  while (!error) { int rc = table->file->ha_rnd_next(table->record[0]); if (rc == HA_ERR_END_OF_FILE) break; if (rc) { error = true; break; } if (!Match(table, request)) continue; std::memcpy(table->record[1], table->record[0], table->s->reclength); std::memcpy(table->record[0], table->record[1], table->s->reclength); StoreText(table->field[17], "RETIRED"); if (table->file->ha_update_row(table->record[1], table->record[0])) { error = true; break; } }
  if (!error) table->file->ha_rnd_end();
  return CloseWrite(&access, thd, table, &backup, error);
}
struct Safe_row { std::string name, capability, provider_model; bool has_dimension; uint64_t dimension, version; };
bool ReadRows(THD *thd, std::vector<Safe_row> *rows) {
  Ai_system_table_access access; Open_tables_backup backup; TABLE *table = nullptr;
  if (access.open_table(thd, k_mysql, k_table_name, k_fields, TL_READ, &table, &backup)) return true;
  bool error = table->file->ha_rnd_init(true) != 0;
  while (!error) { int rc = table->file->ha_rnd_next(table->record[0]); if (rc == HA_ERR_END_OF_FILE) break; if (rc) { error = true; break; } if (Value(table->field[17]) != "ACTIVE") continue; rows->push_back({Value(table->field[1]), Value(table->field[3]), Value(table->field[4]), !table->field[10]->is_null(), table->field[10]->is_null() ? 0 : static_cast<uint64_t>(table->field[10]->val_int()), static_cast<uint64_t>(table->field[18]->val_int())}); }
  if (!error) table->file->ha_rnd_end();
  access.close_table(thd, table, &backup, error, false);
  return error;
}
bool ReadRequest(mem_root_deque<Item *> *list, Ai_model_admin_request *request, bool delete_only) {
  if (list == nullptr || list->size() != (delete_only ? 2U : 5U)) return false;
  String storage; String *name = (*list)[0]->val_str(&storage); if (name == nullptr) return false;
  request->model_name.assign(name->ptr(), name->length());
  String cap_storage; String *cap = (*list)[1]->val_str(&cap_storage); if (cap == nullptr || !ParseCapability(std::string(cap->ptr(), cap->length()), &request->capability)) return false;
  if (delete_only) return !request->model_name.empty();
  String model_storage, mode_storage, credential_storage;
  String *model = (*list)[2]->val_str(&model_storage); String *mode = (*list)[3]->val_str(&mode_storage); String *credential = (*list)[4]->val_str(&credential_storage);
  if (model == nullptr || mode == nullptr || credential == nullptr) return false;
  request->provider_model_name.assign(model->ptr(), model->length()); request->credential_mode.assign(mode->ptr(), mode->length()); request->credential_value.Assign(std::string(credential->ptr(), credential->length()));
  return true;
}
class Sql_cmd_ai_model_admin final : public im::Sql_cmd_admin_proc {
 public:
  Sql_cmd_ai_model_admin(THD *thd, mem_root_deque<Item *> *list, const Ai_model_admin_proc *proc) : Sql_cmd_admin_proc(thd, list, proc), operation_(proc->operation()) {}
  bool check_access(THD *thd) override { auto *sctx = thd->security_context(); if (sctx == nullptr || !sctx->has_global_grant(STRING_WITH_LEN("AI_ADMIN")).first) { my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), "AI_ADMIN"); return true; } return false; }
  bool pc_execute(THD *thd) override { if (operation_ == Ai_model_admin_proc::Operation::k_show) return false; Ai_model_admin_request request; if (!ReadRequest(m_list, &request, operation_ == Ai_model_admin_proc::Operation::k_delete)) { my_error(ER_WRONG_ARGUMENTS, MYF(0), "dbms_ai model request"); return true; } return operation_ == Ai_model_admin_proc::Operation::k_register ? Register(thd, request) : operation_ == Ai_model_admin_proc::Operation::k_update ? Update(thd, request) : Delete(thd, request); }
  void send_result(THD *thd, bool error) override { if (error) return; if (operation_ != Ai_model_admin_proc::Operation::k_show) { my_ok(thd); return; } if (m_proc->send_result_metadata(thd)) return; std::vector<Safe_row> rows; if (ReadRows(thd, &rows)) return; for (const auto &row : rows) { Protocol *p = thd->get_protocol(); p->start_row(); p->store_string(row.name.c_str(), row.name.size(), system_charset_info); p->store_string(row.capability.c_str(), row.capability.size(), system_charset_info); p->store_string(row.provider_model.c_str(), row.provider_model.size(), system_charset_info); if (row.has_dimension) p->store_longlong(row.dimension, true); else p->store_null(); p->store_longlong(row.version, true); if (p->end_row()) return; } my_eof(thd); }
 private: Ai_model_admin_proc::Operation operation_;
};
}  // namespace

Ai_model_admin_proc::Ai_model_admin_proc(Operation operation, const char *name) : Proc(0), operation_(operation), name_(name) {
  if (operation == Operation::k_show) { m_result_type = Result_type::RESULT_SET; Column_element columns[] = {{MYSQL_TYPE_VARCHAR, STRING_WITH_LEN("MODEL_NAME"), 255}, {MYSQL_TYPE_VARCHAR, STRING_WITH_LEN("CAPABILITY"), 32}, {MYSQL_TYPE_VARCHAR, STRING_WITH_LEN("PROVIDER_MODEL_NAME"), 255}, {MYSQL_TYPE_LONGLONG, STRING_WITH_LEN("DIMENSION"), 0}, {MYSQL_TYPE_LONGLONG, STRING_WITH_LEN("CONFIG_VERSION"), 0}}; for (size_t i = 0; i < 5; ++i) m_columns.assign_at(i, columns[i]); } else { m_result_type = Result_type::RESULT_OK; const size_t n = operation == Operation::k_delete ? 2 : 5; for (size_t i = 0; i < n; ++i) m_parameters.assign_at(i, MYSQL_TYPE_VARCHAR); }
}
Sql_cmd *Ai_model_admin_proc::evoke_cmd(THD *thd, mem_root_deque<Item *> *list) const { return new (thd->mem_root) Sql_cmd_ai_model_admin(thd, list, this); }
const std::string Ai_model_admin_proc::str() const { return name_; }
const std::string Ai_model_admin_proc::qname() const { return std::string(AI_MODEL_ADMIN_PROC_SCHEMA.str) + "." + name_; }
Ai_model_register_proc::Ai_model_register_proc() : Ai_model_admin_proc(Operation::k_register, "register_model") {} im::Proc *Ai_model_register_proc::instance() { static im::Proc *p = new Ai_model_register_proc(); return p; }
Ai_model_update_proc::Ai_model_update_proc() : Ai_model_admin_proc(Operation::k_update, "update_model") {} im::Proc *Ai_model_update_proc::instance() { static im::Proc *p = new Ai_model_update_proc(); return p; }
Ai_model_delete_proc::Ai_model_delete_proc() : Ai_model_admin_proc(Operation::k_delete, "delete_model") {} im::Proc *Ai_model_delete_proc::instance() { static im::Proc *p = new Ai_model_delete_proc(); return p; }
Ai_model_show_proc::Ai_model_show_proc() : Ai_model_admin_proc(Operation::k_show, "show_models") {} im::Proc *Ai_model_show_proc::instance() { static im::Proc *p = new Ai_model_show_proc(); return p; }
}  // namespace alisql::ai
