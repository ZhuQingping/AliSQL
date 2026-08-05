/* Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved. */
#ifndef SQL_AI_ITEM_AI_FUNC_INCLUDED
#define SQL_AI_ITEM_AI_FUNC_INCLUDED

#include "sql/item_strfunc.h"

namespace alisql::ai {

class Item_func_ai_embedding final : public Item_str_func {
  String buffer;
 public:
  Item_func_ai_embedding(const POS &pos, PT_item_list *list)
      : Item_str_func(pos, list) {}
  Item_func_ai_embedding(const POS &pos, Item *a) : Item_str_func(pos, a) {}
  Item_func_ai_embedding(const POS &pos, Item *a, Item *b)
      : Item_str_func(pos, a, b) {}
  Item_func_ai_embedding(const POS &pos, Item *a, Item *b, Item *c)
      : Item_str_func(pos, a, b, c) {}
  bool itemize(Parse_context *pc, Item **res) override;
  bool resolve_type(THD *thd) override;
  String *val_str(String *str) override;
  const char *func_name() const override { return "ai_embedding"; }
};

class Item_func_ai_analyze final : public Item_str_func {
  String buffer;
 public:
  Item_func_ai_analyze(const POS &pos, PT_item_list *list)
      : Item_str_func(pos, list) {}
  Item_func_ai_analyze(const POS &pos, Item *a, Item *b)
      : Item_str_func(pos, a, b) {}
  Item_func_ai_analyze(const POS &pos, Item *a, Item *b, Item *c)
      : Item_str_func(pos, a, b, c) {}
  bool itemize(Parse_context *pc, Item **res) override;
  bool resolve_type(THD *thd) override;
  String *val_str(String *str) override;
  const char *func_name() const override { return "ai_analyze"; }
};

class Item_func_ai_model_info final : public Item_str_func {
  String buffer;
 public:
  Item_func_ai_model_info(const POS &pos, PT_item_list *list)
      : Item_str_func(pos, list) {}
  explicit Item_func_ai_model_info(const POS &pos) : Item_str_func(pos) {}
  Item_func_ai_model_info(const POS &pos, Item *a) : Item_str_func(pos, a) {}
  bool resolve_type(THD *thd) override;
  String *val_str(String *str) override;
  const char *func_name() const override { return "ai_model_info"; }
};

}  // namespace alisql::ai
#endif
