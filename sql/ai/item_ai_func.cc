/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */
#include "sql/ai/item_ai_func.h"

#include "mysqld_error.h"
#include "sql/ai/ai_model_registry.h"
#include "sql/ai/ai_runtime.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/sql_class.h"
#include "vidx/vidx_field.h"

namespace alisql::ai {
namespace {
bool ResolveStringArguments(THD *thd, Item_func *item, uint min_args,
                            uint max_args) {
  (void)thd;
  if (item->arg_count < min_args || item->arg_count > max_args) {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), item->func_name());
    return true;
  }
  for (uint i = 0; i < item->arg_count; ++i) {
    if (item->get_arg(i)->result_type() != STRING_RESULT ||
        item->get_arg(i)->data_type() == MYSQL_TYPE_JSON) {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), item->func_name());
      return true;
    }
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

  Ai_runtime runtime(nullptr, nullptr);
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
  if (ResolveStringArguments(thd, this, 2, 3)) return true;
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

  Ai_runtime runtime(nullptr, nullptr);
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
  const std::string result =
      "{\"model_name\":\"" + model.model_name + "\",\"config_id\":" +
      std::to_string(model.config_id) + ",\"config_version\":" +
      std::to_string(model.config_version) + ",\"model_revision\":\"" +
      model.model_revision + "\",\"dimension\":" +
      std::to_string(model.dimension) + "}";
  if (buffer.mem_realloc(result.size())) return error_str();
  memcpy(buffer.ptr(), result.data(), result.size());
  buffer.length(result.size());
  buffer.set_charset(&my_charset_utf8mb4_0900_ai_ci);
  return &buffer;
}

}  // namespace alisql::ai
