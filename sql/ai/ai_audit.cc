/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#include "sql/ai/ai_audit.h"

#include "sql/current_thd.h"
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/rpl_table_access.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/table.h"

namespace alisql::ai {
namespace {

constexpr uint k_audit_field_count = 17;
const LEX_CSTRING k_mysql_schema = {STRING_WITH_LEN("mysql")};
const LEX_CSTRING k_audit_table = {STRING_WITH_LEN("alisql_ai_call_audit")};

class Ai_audit_table_access final : public System_table_access {
 public:
  void before_open(THD *) override {
    m_flags = MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK | MYSQL_OPEN_IGNORE_FLUSH |
              MYSQL_LOCK_IGNORE_TIMEOUT;
  }
};

class Scoped_audit_thd {
 public:
  Scoped_audit_thd(Ai_audit_table_access *access, THD *caller)
      : access_(access), caller_(caller != nullptr ? caller : current_thd),
        audit_thd_(access_->create_thd()) {}
  ~Scoped_audit_thd() {
    if (audit_thd_ != nullptr) access_->drop_thd(audit_thd_);
    if (caller_ != nullptr) caller_->store_globals();
  }

  THD *get() const { return audit_thd_; }

 private:
  Ai_audit_table_access *access_;
  THD *caller_;
  THD *audit_thd_;
};

const char *CapabilityName(Ai_capability capability) {
  return capability == Ai_capability::k_text_embedding ? "TEXT_EMBEDDING"
                                                       : "TEXT_GENERATION";
}

const char *StatusName(Ai_audit_status status) {
  switch (status) {
    case Ai_audit_status::k_started:
      return "STARTED";
    case Ai_audit_status::k_succeeded:
      return "SUCCEEDED";
    case Ai_audit_status::k_failed:
      return "FAILED";
  }
  return "FAILED";
}

const char *ErrorName(Ai_error error) {
  switch (error) {
    case Ai_error::k_ok: return "OK";
    case Ai_error::k_invalid_options: return "INVALID_OPTIONS";
    case Ai_error::k_incomplete_output: return "INCOMPLETE_OUTPUT";
    case Ai_error::k_dimension_mismatch: return "DIMENSION_MISMATCH";
    case Ai_error::k_timeout: return "TIMEOUT";
    case Ai_error::k_provider_error: return "PROVIDER_ERROR";
    case Ai_error::k_model_not_found: return "MODEL_NOT_FOUND";
    case Ai_error::k_credential_unavailable: return "CREDENTIAL_UNAVAILABLE";
    case Ai_error::k_access_denied: return "ACCESS_DENIED";
    case Ai_error::k_response_too_large: return "RESPONSE_TOO_LARGE";
    case Ai_error::k_rate_limited: return "RATE_LIMITED";
    case Ai_error::k_protocol_mismatch: return "PROTOCOL_MISMATCH";
    case Ai_error::k_audit_unavailable: return "AUDIT_UNAVAILABLE";
  }
  return "PROVIDER_ERROR";
}

void StoreUnsigned(Field *field, uint64_t value) {
  field->store(static_cast<longlong>(value), true);
}

void StoreString(Field *field, const char *value) {
  field->set_notnull();
  field->store(value, strlen(value), system_charset_info);
}

void StoreRecordFields(TABLE *table, const Ai_audit_record &record,
                       bool include_call_id) {
  if (include_call_id) StoreUnsigned(table->field[0], record.call_id);
  StoreUnsigned(table->field[1], record.tenant_id);
  StoreUnsigned(table->field[2], record.config_id);
  StoreUnsigned(table->field[3], record.config_version);
  StoreString(table->field[4], CapabilityName(record.capability));
  StoreString(table->field[5], StatusName(record.status));
  StoreString(table->field[6], ErrorName(record.error));
  if (record.provider_request_id.empty())
    table->field[7]->set_null();
  else {
    table->field[7]->set_notnull();
    table->field[7]->store(record.provider_request_id.data(),
                           record.provider_request_id.size(),
                           system_charset_info);
  }
  StoreUnsigned(table->field[8], record.usage.prompt_tokens);
  StoreUnsigned(table->field[9], record.usage.completion_tokens);
  StoreUnsigned(table->field[10], record.usage.reasoning_tokens);
  StoreUnsigned(table->field[11], record.usage.cached_tokens);
  StoreUnsigned(table->field[12], record.usage.total_tokens);
  StoreUnsigned(table->field[15], record.latency_ms);
  StoreUnsigned(table->field[16], record.http_status);
}

Ai_error CloseAuditTable(Ai_audit_table_access *access, THD *thd,
                         TABLE *table, Open_tables_backup *backup,
                         bool error) {
  return access->close_table(thd, table, backup, error, true)
             ? Ai_error::k_audit_unavailable
             : error ? Ai_error::k_audit_unavailable : Ai_error::k_ok;
}

}  // namespace

Ai_error Ai_system_table_audit_sink::Start(THD *caller,
                                           const Ai_audit_record &record,
                                           uint64_t *call_id) {
  if (call_id == nullptr) return Ai_error::k_audit_unavailable;
  *call_id = 0;
  Ai_audit_table_access access;
  Scoped_audit_thd audit_thd(&access, caller);
  if (audit_thd.get() == nullptr) return Ai_error::k_audit_unavailable;

  Open_tables_backup backup;
  TABLE *table = nullptr;
  if (access.open_table(audit_thd.get(), k_mysql_schema, k_audit_table,
                        k_audit_field_count, TL_WRITE, &table, &backup))
    return Ai_error::k_audit_unavailable;
  table->use_all_columns();
  table->mark_columns_needed_for_insert(audit_thd.get());
  table->next_number_field = table->found_next_number_field;
  table->autoinc_field_has_explicit_non_null_value = true;
  restore_record(table, s->default_values);
  StoreRecordFields(table, record, false);
  const bool error = table->file->ha_write_row(table->record[0]) != 0;
  if (!error) *call_id = table->file->insert_id_for_cur_row;
  table->file->ha_release_auto_increment();
  table->next_number_field = nullptr;
  table->autoinc_field_has_explicit_non_null_value = false;
  const Ai_error result =
      CloseAuditTable(&access, audit_thd.get(), table, &backup, error);
  return result == Ai_error::k_ok && *call_id != 0 ? Ai_error::k_ok
                                                    : Ai_error::k_audit_unavailable;
}

Ai_error Ai_system_table_audit_sink::Complete(THD *caller, uint64_t call_id,
                                              const Ai_audit_record &record) {
  if (call_id == 0) return Ai_error::k_audit_unavailable;
  Ai_audit_table_access access;
  Scoped_audit_thd audit_thd(&access, caller);
  if (audit_thd.get() == nullptr) return Ai_error::k_audit_unavailable;

  Open_tables_backup backup;
  TABLE *table = nullptr;
  if (access.open_table(audit_thd.get(), k_mysql_schema, k_audit_table,
                        k_audit_field_count, TL_WRITE, &table, &backup))
    return Ai_error::k_audit_unavailable;
  table->use_all_columns();
  bool index_inited = false;
  bool error = table->file->ha_index_init(0, true) != 0;
  if (!error) index_inited = true;
  if (!error) {
    StoreUnsigned(table->field[0], call_id);
    uchar key[MAX_KEY_LENGTH];
    key_copy(key, table->record[0], table->key_info, table->key_info->key_length);
    error = table->file->ha_index_read_map(table->record[0], key, HA_WHOLE_KEY,
                                           HA_READ_KEY_EXACT) != 0;
  }
  if (!error) {
    store_record(table, record[1]);
    StoreRecordFields(table, record, false);
    const my_timeval completed = audit_thd.get()->query_start_timeval_trunc(0);
    table->field[14]->set_notnull();
    table->field[14]->store_timestamp(&completed);
    error = table->file->ha_update_row(table->record[1], table->record[0]) != 0;
  }
  if (index_inited) table->file->ha_index_end();
  return CloseAuditTable(&access, audit_thd.get(), table, &backup, error);
}

}  // namespace alisql::ai
