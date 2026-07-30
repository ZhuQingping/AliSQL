/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */
#include "sql/ai/item_ai_func.h"

#include "mysqld_error.h"
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
}  // namespace

bool Item_func_ai_embedding::resolve_type(THD *thd) {
  if (ResolveStringArguments(thd, this, 1, 3)) return true;
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
  my_error(ER_NOT_SUPPORTED_YET, MYF(0), "DB4AI runtime execution");
  return error_str();
}

bool Item_func_ai_analyze::resolve_type(THD *thd) {
  if (ResolveStringArguments(thd, this, 2, 3)) return true;
  set_data_type_string(1024 * 1024, &my_charset_utf8mb4_0900_ai_ci);
  return false;
}

String *Item_func_ai_analyze::val_str(String *) {
  null_value = false;
  my_error(ER_NOT_SUPPORTED_YET, MYF(0), "DB4AI runtime execution");
  return error_str();
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

String *Item_func_ai_model_info::val_str(String *) {
  null_value = false;
  my_error(ER_NOT_SUPPORTED_YET, MYF(0), "DB4AI model metadata");
  return error_str();
}

}  // namespace alisql::ai
