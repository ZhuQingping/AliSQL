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
#include "sql/mysqld.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
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

bool CheckAiModelInfoPrivilege(THD *thd) {
  if (thd != nullptr && thd->security_context() != nullptr &&
      (thd->security_context()
           ->has_global_grant(STRING_WITH_LEN("AI_INVOKE"))
           .first ||
       thd->security_context()->has_global_grant(STRING_WITH_LEN("AI_ADMIN"))
           .first))
    return false;
  my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), "AI_INVOKE or AI_ADMIN");
  return true;
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
      detail = "DB4AI MaaS invocation limit was exceeded locally";
      break;
    case Ai_error::k_protocol_mismatch:
      detail = "DB4AI model endpoint is incompatible with its capability";
      break;
    case Ai_error::k_audit_unavailable:
      detail = "DB4AI audit service is unavailable; invocation outcome is unknown";
      break;
    case Ai_error::k_feature_disabled:
      detail = "DB4AI MaaS feature is disabled";
      break;
    case Ai_error::k_replication_unsafe:
      detail = "DB4AI MaaS invocation in a data-changing statement requires row-based binlog";
      break;
    case Ai_error::k_request_too_large:
      detail = "DB4AI MaaS request input is too large";
      break;
    case Ai_error::k_unsafe_output:
      detail = "DB4AI diagnosis output violated the read-only contract";
      break;
    default:
      break;
  }
  my_error(ER_NOT_SUPPORTED_YET, MYF(0), detail);
}
}  // namespace

bool Item_func_ai_embedding::itemize(Parse_context *pc, Item **res) {
  if (skip_itemize(res)) return false;
  if (Item_str_func::itemize(pc, res)) return true;
  // Binlog format is decided after itemization but before expression
  // evaluation. Mark the statement here so MIXED emits row events; waiting
  // until resolve_type() or val_str() can already be too late for a STORED
  // generated column write.
  pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  pc->thd->lex->set_uncacheable(pc->select, UNCACHEABLE_SIDEEFFECT);
  return false;
}

bool Item_func_ai_embedding::resolve_type(THD *thd) {
  (void)thd;
  if (arg_count < 2 || arg_count > 3 ||
      args[0]->result_type() != STRING_RESULT ||
      args[0]->data_type() == MYSQL_TYPE_JSON ||
      (arg_count >= 2 && (args[1]->result_type() != STRING_RESULT ||
                          args[1]->data_type() == MYSQL_TYPE_JSON)) ||
      (arg_count == 3 && args[2]->result_type() != STRING_RESULT &&
       args[2]->data_type() != MYSQL_TYPE_JSON)) {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
    return true;
  }
  set_data_type_vector(
      static_cast<ulonglong>(vidx::Field_vector::dimension_bytes(1024)));
  return false;
}

String *Item_func_ai_embedding::val_str(String *str) {
  null_value = false;
  String model_buffer;
  String *model = args[0]->val_str(&model_buffer);
  if (model == nullptr) {
    null_value = true;
    return nullptr;
  }
  String *input = args[1]->val_str(str);
  if (input == nullptr) {
    null_value = true;
    return nullptr;
  }
  if (model == nullptr || model->length() == 0) {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
    return error_str();
  }
  const std::string model_name(model->ptr(), model->length());
  if (!IsAiMaaSEnabled()) {
    RaiseAiRuntimeError(Ai_error::k_feature_disabled);
    return error_str();
  }
  if (CheckAiInvokePrivilege(current_thd)) return error_str();

  Ai_embedding_options options;
  if (arg_count == 3) {
    String options_buffer;
    String *options_json = args[2]->val_str(&options_buffer);
    if (options_json == nullptr) {
      null_value = true;
      return nullptr;
    }
    Ai_runtime parser(nullptr, nullptr);
    if (parser.ParseEmbeddingOptions(
            std::string(options_json->ptr(), options_json->length()),
            &options) != Ai_error::k_ok) {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
      return error_str();
    }
  }

  Ai_runtime runtime(nullptr, Get_ai_invoke_audit_sink());
  std::string encoded_vector;
  const Ai_error error = runtime.Embed(
      current_thd, std::string(input->ptr(), input->length()), model_name,
      options.dimension, &encoded_vector);
  if (error != Ai_error::k_ok) {
    RaiseAiRuntimeError(error);
    return error_str();
  }
  if (buffer.mem_realloc(encoded_vector.size())) return error_str();
  memcpy(buffer.ptr(), encoded_vector.data(), encoded_vector.size());
  buffer.length(encoded_vector.size());
  return &buffer;
}

bool Item_func_ai_analyze::itemize(Parse_context *pc, Item **res) {
  if (skip_itemize(res)) return false;
  if (Item_str_func::itemize(pc, res)) return true;
  pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  pc->thd->lex->set_uncacheable(pc->select, UNCACHEABLE_SIDEEFFECT);
  return false;
}

bool Item_func_ai_analyze::resolve_type(THD *thd) {
  if (ResolveAnalyzeArguments(thd, this)) return true;
  set_data_type_string(1024 * 1024, &my_charset_utf8mb4_0900_ai_ci);
  return false;
}

String *Item_func_ai_analyze::val_str(String *str) {
  null_value = false;
  String model_buffer;
  String *model = args[0]->val_str(&model_buffer);
  if (model == nullptr) {
    null_value = true;
    return nullptr;
  }
  String *prompt = args[1]->val_str(str);
  if (prompt == nullptr) {
    null_value = true;
    return nullptr;
  }
  if (model->length() == 0 || prompt->length() == 0) {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
    return error_str();
  }
  if (!IsAiMaaSEnabled()) {
    RaiseAiRuntimeError(Ai_error::k_feature_disabled);
    return error_str();
  }
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
  if (CheckAiInvokePrivilege(current_thd)) return error_str();

  Ai_runtime runtime(nullptr, Get_ai_invoke_audit_sink());
  std::string final_content;
  const Ai_error error = runtime.Analyze(
      current_thd, std::string(model->ptr(), model->length()),
      std::string(prompt->ptr(), prompt->length()), options, &final_content);
  if (error == Ai_error::k_invalid_options) {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
    return error_str();
  }
  if (error != Ai_error::k_ok) {
    RaiseAiRuntimeError(error);
    return error_str();
  }
  if (buffer.mem_realloc(final_content.size())) return error_str();
  memcpy(buffer.ptr(), final_content.data(), final_content.size());
  buffer.length(final_content.size());
  buffer.set_charset(&my_charset_utf8mb4_0900_ai_ci);
  return &buffer;
}

bool Item_func_ai_model_info::resolve_type(THD *thd) {
  (void)thd;
  if (arg_count != 1 || args[0]->result_type() != STRING_RESULT ||
      args[0]->data_type() == MYSQL_TYPE_JSON) {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), func_name());
    return true;
  }
  set_data_type_string(1024, &my_charset_utf8mb4_0900_ai_ci);
  return false;
}

String *Item_func_ai_model_info::val_str(String *str) {
  null_value = false;
  if (CheckAiModelInfoPrivilege(current_thd)) return error_str();
  String *model_argument = args[0]->val_str(str);
  if (model_argument == nullptr) {
    null_value = true;
    return nullptr;
  }
  std::string model_name(model_argument->ptr(), model_argument->length());
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
  writer.Key("dimension");
  writer.Uint(model.dimension);
  writer.Key("provider");
  writer.String(model.provider.c_str(), model.provider.size());
  writer.Key("capability");
  writer.String(model.capability == Ai_capability::k_text_embedding
                    ? "TEXT_EMBEDDING"
                    : "TEXT_GENERATION");
  writer.EndObject();
  if (buffer.mem_realloc(json.GetSize())) return error_str();
  memcpy(buffer.ptr(), json.GetString(), json.GetSize());
  buffer.length(json.GetSize());
  buffer.set_charset(&my_charset_utf8mb4_0900_ai_ci);
  return &buffer;
}

}  // namespace alisql::ai
