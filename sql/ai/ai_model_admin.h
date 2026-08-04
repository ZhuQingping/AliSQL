/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */

#ifndef SQL_AI_AI_MODEL_ADMIN_INCLUDED
#define SQL_AI_AI_MODEL_ADMIN_INCLUDED

#include <string>

#include "sql/ai/ai_model_registry.h"
#include "sql/package/proc.h"

namespace alisql::ai {

extern const LEX_CSTRING AI_MODEL_ADMIN_PROC_SCHEMA;

struct Ai_model_admin_request {
  std::string model_name;
  Ai_capability capability;
  std::string provider_model_name;
  std::string credential_mode;
  Secure_string credential_value;
};

class Ai_model_admin_proc : public im::Proc, public im::Disable_copy_base {
 public:
  enum class Operation { k_register, k_update, k_delete, k_show };
  Ai_model_admin_proc(Operation operation, const char *name);
  Sql_cmd *evoke_cmd(THD *thd, mem_root_deque<Item *> *list) const override;
  const std::string str() const override;
  const std::string qname() const override;
  Operation operation() const { return operation_; }

 private:
  Operation operation_;
  std::string name_;
};

class Ai_model_register_proc final : public Ai_model_admin_proc {
 public:
  Ai_model_register_proc();
  static im::Proc *instance();
};
class Ai_model_update_proc final : public Ai_model_admin_proc {
 public:
  Ai_model_update_proc();
  static im::Proc *instance();
};
class Ai_model_delete_proc final : public Ai_model_admin_proc {
 public:
  Ai_model_delete_proc();
  static im::Proc *instance();
};
class Ai_model_show_proc final : public Ai_model_admin_proc {
 public:
  Ai_model_show_proc();
  static im::Proc *instance();
};

}  // namespace alisql::ai

#endif  // SQL_AI_AI_MODEL_ADMIN_INCLUDED
