# AliSQL DB4AI：受控 RAG、分析与只读诊断

`AI_EMBEDDING` 与 `AI_ANALYZE` 是受 Profile、tenant 和 `AI_INVOKE` 约束的调用面。
业务 SQL 只能传逻辑模型名和稳定的 options；endpoint、认证材料、厂商私有参数均不是
SQL 输入。生产 Profile 使用 keyring `SECRET_REF`；Debug 的 `PLAINTEXT_DEV` 只用于本地
联调，Release 构建拒绝它。

## RAG

完整、可运行的 schema 与检索示例见
[`examples/rag_product_manual.sql`](examples/rag_product_manual.sql)。向量的 model、config
version、embedding space 和 distance contract 必须随行保存，并由 schema 约束和检索
谓词共同保持一致。模型不可替代以下数据库职责：

- tenant、业务域、访问标签和 embedding contract 的过滤在 SQL 中完成；
- 只将已经选出的、最少必要的 chunk 传给模型；
- 返回答案时同时返回数据库生成的 `source_id`、`chunk_id`，不把生成文字当作来源。

更换模型 revision、维度、归一化方式、distance metric 或 embedding space 时，创建新的
语料和 VECTOR INDEX；不要混用旧向量。服务器尚未把 embedding contract 变成所有
Profile 的通用写入时检查，因此该约束是每个语料 schema 的交付责任。

## 分析与 DBA 诊断

`AI_ANALYZE(task, input_json, options_json)` 的 input 可以是 JSON。调用前先在 SQL 或
应用层完成聚合、行数限制和 PII 删除。`mode='analyze'` 用于受控业务摘要；
`mode='diagnose'` 只返回证据、风险和建议，不执行 SQL、不修改数据、不修改配置，也不
触发自动修复。`mode='rag'` 只消费已检索的上下文。

公开 options 仅为 `model_name`、`mode`、`output_format`、`return_sources`、
`max_output_tokens` 和 `timeout_ms`。`temperature`、`tools`、`thinking` 等厂商参数会被
拒绝。Huawei V2 Chat 仅接受有 final content 的完整响应；reasoning-only、缺 final content
或 `finish_reason=length` 都会失败。

## 离线验证边界

`rds.ai_maas_rag` 在 Debug 服务器中用专属不可路由的 fixture Profile，通过实际
`AI_EMBEDDING` 写入 VECTOR INDEX、执行 tenant/space 过滤并调用 RAG `AI_ANALYZE`。
`rds.ai_maas_analysis` 覆盖 JSON input、analyze/diagnose mode 和私有 options 拒绝。
fixture 只在 Debug 构建编译，位于凭据读取和 HTTP dispatch 之前：不会读取 secret，
不会产生网络 egress，也不代表真实云端验收。真实 MaaS 检查仍须按运维文档明确授权后
运行 `scripts/db4ai_maas_smoke.sh`。
