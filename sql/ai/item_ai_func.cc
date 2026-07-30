/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */
#include "sql/ai/item_ai_func.h"

#include <limits>

#include <my_rapidjson_size_t.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "mysqld_error.h"
#include "sql/ai/ai_audit.h"
#include "sql/ai/ai_model_registry.h"
#include "sql/ai/ai_runtime.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/rpl_table_access.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/table.h"
#include "vidx/vidx_field.h"

namespace alisql::ai {
namespace {
bool ResolveAnalyzeArguments(THD *thd, Item_func *item) {
  (void)thd;
  if (item->arg_count < 2 || item->arg_count > 3) {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), item->func_name());
    return true;
  }
  for (uint i = 0; i < 2; ++i) {
    if (item->get_arg(i)->result_type() != STRING_RESULT ||
        item->get_arg(i)->data_type() == MYSQL_TYPE_JSON) {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), item->func_name());
      return true;
    }
  }
  if (item->arg_count == 3 &&
      item->get_arg(2)->result_type() != STRING_RESULT &&
      item->get_arg(2)->data_type() != MYSQL_TYPE_JSON) {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), item->func_name());
    return true;
  }
  return false;
}

bool CheckAiInvokePrivilege(THD *thd) {
  if (thd != nullptr && thd->security_context() != nullptr &&
      thd->security_context()->has_global_grant(STRING_WITH_LEN("AI_INVOKE"))
          .first)
    return false;
  my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), "AI_INVOKE");
  return true;
}

bool CheckAiAuditPrivilege(THD *thd) {
  if (thd != nullptr && thd->security_context() != nullptr &&
      (thd->security_context()
           ->has_global_grant(STRING_WITH_LEN("AI_AUDIT_VIEWER"))
           .first ||
       thd->security_context()->has_global_grant(STRING_WITH_LEN("AI_ADMIN"))
           .first))
    return false;
  my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), "AI_AUDIT_VIEWER");
  return true;
}

bool IsAiAdmin(THD *thd) {
  return thd != nullptr && thd->security_context() != nullptr &&
         thd->security_context()->has_global_grant(STRING_WITH_LEN("AI_ADMIN"))
             .first;
}

constexpr uint k_audit_field_count = 17;
const LEX_CSTRING k_mysql_schema = {STRING_WITH_LEN("mysql")};
const LEX_CSTRING k_audit_table = {STRING_WITH_LEN("alisql_ai_call_audit")};

class Ai_audit_read_table_access final : public System_table_access {
 public:
  void before_open(THD *) override {
    m_flags = MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK | MYSQL_OPEN_IGNORE_FLUSH |
              MYSQL_LOCK_IGNORE_TIMEOUT;
  }
};

std::string AuditFieldValue(Field *field) {
  if (field->is_null()) return {};
  String value;
  field->val_str(&value, &value);
  return std::string(value.ptr(), value.length());
}

void WriteAuditString(rapidjson::Writer<rapidjson::StringBuffer> *writer,
                      const char *key, Field *field) {
  writer->Key(key);
  if (field->is_null()) {
    writer->Null();
    return;
  }
  const std::string value = AuditFieldValue(field);
  writer->String(value.data(), value.size());
}

void RaiseAiRuntimeError(Ai_error error) {
  const char *detail = "DB4AI provider request failed";
  switch (error) {
    case Ai_error::k_model_not_found:
      detail = "DB4AI model configuration was not found";
      break;
    case Ai_error::k_credential_unavailable:
      detail = "DB4AI credential is unavailable";
      break;
    case Ai_error::k_dimension_mismatch:
      detail = "DB4AI embedding dimension is incompatible with the model";
      break;
    case Ai_error::k_timeout:
      detail = "DB4AI provider request timed out";
      break;
    case Ai_error::k_response_too_large:
      detail = "DB4AI provider response is too large";
      break;
    case Ai_error::k_access_denied:
      detail = "DB4AI provider credential or account access was denied";
      break;
    case Ai_error::k_rate_limited:
      detail = "DB4AI provider rate limit was exceeded";
      break;
    case Ai_error::k_protocol_mismatch:
      detail = "DB4AI model endpoint is incompatible with its capability";
      break;
    case Ai_error::k_audit_unavailable:
      detail = "DB4AI audit service is unavailable";
      break;
    default:
      break;
  }
  my_error(ER_NOT_SUPPORTED_YET, MYF(0), detail);
}
}  // namespace

bool Item_func_ai_embedding::resolve_type(THD *thd) {
  (void)thd;
  if (arg_count < 1 || arg_count > 3 ||
      args[0]->result_type() != STRING_RESULT ||
      args[0]->data_type() == MYSQL_TYPE_JSON ||
      (arg_count >= 2 && (args[1]->result_type() != STRING_RESULT ||
                          args[1]->data_type() == MYSQL_TYPE_JSON)) ||
      (arg_count == 3 && args[2]->result_type() != INT_RESULT)) {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
    return true;
  }
  set_data_type_vector(
      static_cast<ulonglong>(vidx::Field_vector::dimension_bytes(1024)));
  return false;
}

String *Item_func_ai_embedding::val_str(String *str) {
  null_value = false;
  String *input = args[0]->val_str(str);
  if (input == nullptr) {
    null_value = true;
    return nullptr;
  }
  if (CheckAiInvokePrivilege(current_thd)) return error_str();

  std::string model_name{"huawei/bge-m3"};
  if (arg_count >= 2) {
    String model_buffer;
    String *model = args[1]->val_str(&model_buffer);
    if (model == nullptr) {
      null_value = true;
      return nullptr;
    }
    model_name.assign(model->ptr(), model->length());
  }

  uint32_t dimension = 0;
  if (arg_count == 3) {
    const longlong supplied_dimension = args[2]->val_int();
    if (args[2]->null_value || supplied_dimension <= 0 ||
        static_cast<ulonglong>(supplied_dimension) > UINT32_MAX) {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
      return error_str();
    }
    dimension = static_cast<uint32_t>(supplied_dimension);
  }

  Ai_system_table_audit_sink audit;
  Ai_runtime runtime(nullptr, &audit);
  std::string encoded_vector;
  const Ai_error error = runtime.Embed(
      current_thd, std::string(input->ptr(), input->length()), model_name,
      dimension, &encoded_vector);
  if (error != Ai_error::k_ok) {
    RaiseAiRuntimeError(error);
    return error_str();
  }
  if (buffer.mem_realloc(encoded_vector.size())) return error_str();
  memcpy(buffer.ptr(), encoded_vector.data(), encoded_vector.size());
  buffer.length(encoded_vector.size());
  return &buffer;
}

bool Item_func_ai_analyze::resolve_type(THD *thd) {
  if (ResolveAnalyzeArguments(thd, this)) return true;
  set_data_type_string(1024 * 1024, &my_charset_utf8mb4_0900_ai_ci);
  return false;
}

String *Item_func_ai_analyze::val_str(String *str) {
  null_value = false;
  String *task = args[0]->val_str(str);
  if (task == nullptr) {
    null_value = true;
    return nullptr;
  }
  String input_buffer;
  String *input = args[1]->val_str(&input_buffer);
  if (input == nullptr) {
    null_value = true;
    return nullptr;
  }
  if (CheckAiInvokePrivilege(current_thd)) return error_str();

  Ai_analyze_options options;
  if (arg_count == 3) {
    String options_buffer;
    String *options_json = args[2]->val_str(&options_buffer);
    if (options_json == nullptr) {
      null_value = true;
      return nullptr;
    }
    Ai_runtime parser(nullptr, nullptr);
    const Ai_error option_error = parser.ParseAnalyzeOptions(
        std::string(options_json->ptr(), options_json->length()), &options);
    if (option_error != Ai_error::k_ok) {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
      return error_str();
    }
  }

  Ai_system_table_audit_sink audit;
  Ai_runtime runtime(nullptr, &audit);
  std::string final_content;
  const Ai_error error = runtime.Analyze(
      current_thd, std::string(task->ptr(), task->length()),
      std::string(input->ptr(), input->length()), options, &final_content);
  if (error != Ai_error::k_ok) {
    RaiseAiRuntimeError(error);
    return error_str();
  }
  if (buffer.mem_realloc(final_content.size())) return error_str();
  memcpy(buffer.ptr(), final_content.data(), final_content.size());
  buffer.length(final_content.size());
  return &buffer;
}

bool Item_func_ai_model_info::resolve_type(THD *thd) {
  (void)thd;
  if (arg_count > 1 ||
      (arg_count == 1 && (args[0]->result_type() != STRING_RESULT ||
                          args[0]->data_type() == MYSQL_TYPE_JSON))) {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
    return true;
  }
  set_data_type_string(1024, &my_charset_utf8mb4_0900_ai_ci);
  return false;
}

String *Item_func_ai_model_info::val_str(String *str) {
  null_value = false;
  if (CheckAiAuditPrivilege(current_thd)) return error_str();
  std::string model_name{"huawei/bge-m3"};
  if (arg_count == 1) {
    String *model = args[0]->val_str(str);
    if (model == nullptr) {
      null_value = true;
      return nullptr;
    }
    model_name.assign(model->ptr(), model->length());
  }
  Ai_model_registry registry;
  Ai_resolved_model model;
  Ai_error error = registry.Resolve(current_thd, model_name,
                                    Ai_capability::k_text_embedding, &model);
  if (error == Ai_error::k_model_not_found)
    error = registry.Resolve(current_thd, model_name,
                             Ai_capability::k_text_generation, &model);
  if (error != Ai_error::k_ok) {
    RaiseAiRuntimeError(error);
    return error_str();
  }
  rapidjson::StringBuffer json;
  rapidjson::Writer<rapidjson::StringBuffer> writer(json);
  writer.StartObject();
  writer.Key("model_name");
  writer.String(model.model_name.c_str(), model.model_name.size());
  writer.Key("config_id");
  writer.Uint64(model.config_id);
  writer.Key("config_version");
  writer.Uint64(model.config_version);
  writer.Key("model_revision");
  writer.String(model.model_revision.c_str(), model.model_revision.size());
  writer.Key("dimension");
  writer.Uint(model.dimension);
  writer.Key("embedding_space_id");
  writer.String(model.embedding_space_id.c_str(), model.embedding_space_id.size());
  writer.Key("distance_metric");
  writer.String(model.distance_metric.c_str(), model.distance_metric.size());
  writer.EndObject();
  if (buffer.mem_realloc(json.GetSize())) return error_str();
  memcpy(buffer.ptr(), json.GetString(), json.GetSize());
  buffer.length(json.GetSize());
  buffer.set_charset(&my_charset_utf8mb4_0900_ai_ci);
  return &buffer;
}

bool Item_func_ai_audit_info::resolve_type(THD *thd) {
  (void)thd;
  if (arg_count > 1 ||
      (arg_count == 1 && args[0]->result_type() != INT_RESULT)) {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
    return true;
  }
  set_data_type_string(1024 * 1024, &my_charset_utf8mb4_0900_ai_ci);
  return false;
}

String *Item_func_ai_audit_info::val_str(String *str) {
  (void)str;
  null_value = false;
  if (CheckAiAuditPrivilege(current_thd)) return error_str();

  uint64_t limit = 100;
  if (arg_count == 1) {
    const longlong requested = args[0]->val_int();
    if (args[0]->null_value || requested < 1 || requested > 100) {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
      return error_str();
    }
    limit = static_cast<uint64_t>(requested);
  }

  uint64_t caller_tenant_id = 0;
  if (!IsAiAdmin(current_thd)) {
    Ai_model_registry registry;
    if (registry.ResolveTenant(current_thd, &caller_tenant_id) !=
        Ai_error::k_ok) {
      my_error(ER_NOT_SUPPORTED_YET, MYF(0), "DB4AI audit service is unavailable");
      return error_str();
    }
  }

  Ai_audit_read_table_access access;
  Open_tables_backup backup;
  TABLE *table = nullptr;
  if (access.open_table(current_thd, k_mysql_schema, k_audit_table,
                        k_audit_field_count, TL_READ, &table, &backup)) {
    my_error(ER_NOT_SUPPORTED_YET, MYF(0), "DB4AI audit service is unavailable");
    return error_str();
  }

  rapidjson::StringBuffer json;
  rapidjson::Writer<rapidjson::StringBuffer> writer(json);
  writer.StartArray();
  bool read_error = table->file->ha_rnd_init(true) != 0;
  uint64_t returned = 0;
  while (!read_error && returned < limit) {
    const int scan = table->file->ha_rnd_next(table->record[0]);
    if (scan == HA_ERR_END_OF_FILE) break;
    if (scan != 0) {
      read_error = true;
      break;
    }
    const uint64_t tenant_id = static_cast<uint64_t>(table->field[1]->val_int());
    if (!IsAiAdmin(current_thd) && tenant_id != caller_tenant_id) continue;

    writer.StartObject();
    writer.Key("call_id");
    writer.Uint64(static_cast<uint64_t>(table->field[0]->val_int()));
    writer.Key("tenant_id");
    writer.Uint64(tenant_id);
    writer.Key("config_id");
    writer.Uint64(static_cast<uint64_t>(table->field[2]->val_int()));
    writer.Key("config_version");
    writer.Uint64(static_cast<uint64_t>(table->field[3]->val_int()));
    WriteAuditString(&writer, "capability", table->field[4]);
    WriteAuditString(&writer, "status", table->field[5]);
    WriteAuditString(&writer, "error_code", table->field[6]);
    WriteAuditString(&writer, "provider_request_id", table->field[7]);
    writer.Key("prompt_tokens");
    writer.Uint64(static_cast<uint64_t>(table->field[8]->val_int()));
    writer.Key("completion_tokens");
    writer.Uint64(static_cast<uint64_t>(table->field[9]->val_int()));
    writer.Key("reasoning_tokens");
    writer.Uint64(static_cast<uint64_t>(table->field[10]->val_int()));
    writer.Key("cached_tokens");
    writer.Uint64(static_cast<uint64_t>(table->field[11]->val_int()));
    writer.Key("total_tokens");
    writer.Uint64(static_cast<uint64_t>(table->field[12]->val_int()));
    WriteAuditString(&writer, "created_at", table->field[13]);
    WriteAuditString(&writer, "completed_at", table->field[14]);
    writer.Key("latency_ms");
    writer.Uint64(static_cast<uint64_t>(table->field[15]->val_int()));
    writer.Key("http_status");
    writer.Uint(table->field[16]->val_int());
    writer.EndObject();
    ++returned;
  }
  if (!read_error) table->file->ha_rnd_end();
  if (access.close_table(current_thd, table, &backup, read_error, false) ||
      read_error) {
    my_error(ER_NOT_SUPPORTED_YET, MYF(0), "DB4AI audit service is unavailable");
    return error_str();
  }
  writer.EndArray();
  if (buffer.mem_realloc(json.GetSize())) return error_str();
  memcpy(buffer.ptr(), json.GetString(), json.GetSize());
  buffer.length(json.GetSize());
  buffer.set_charset(&my_charset_utf8mb4_0900_ai_ci);
  return &buffer;
}

}  // namespace alisql::ai
