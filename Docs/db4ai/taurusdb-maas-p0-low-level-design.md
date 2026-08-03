# TaurusDB 对接华为云 MaaS P0 Low Level Design

日期：2026-08-04

## 1. 范围与原则

本设计只完成 2026-09-30 P0 的华为云 MaaS 文本能力闭环：文本向量化、文本生成、
RAG 问答、SQL 结果分析、DBA 只读诊断、模型治理和文件审计。百炼、火山、Bedrock、
异步批处理、流式、多模态、Rerank、配额和按模型授权不在本次实现范围。

模型治理以简单、易用和高质量为首要原则。客户不直接修改 `mysql` 系统表，不填写
Provider、Endpoint、向量维度、内部 ID、配置版本或白名单；P0 也不建立模型绑定、
租户绑定、状态管理或默认模型等额外客户概念。

## 2. `dbms_ai` 原生管理包

复用 AliSQL Native Procedure 框架，在 `dbms_ai` schema 注册四个过程。四个过程均要求
全局动态权限 `AI_ADMIN`。系统表 `mysql.taurusdb_ai_model_config` 只供内核运行时、升级和
受控管理包读写；普通账号不得直接获得该表的 DML 权限。

```sql
CALL dbms_ai.register_model(
  model_name, capability, provider_model_name, credential_mode, credential_value);

CALL dbms_ai.update_model(
  model_name, capability, provider_model_name, credential_mode, credential_value);

CALL dbms_ai.delete_model(model_name, capability);

CALL dbms_ai.show_models();
```

### 2.1 参数与校验

- `model_name` 是稳定逻辑名，例如 `huawei/bge-m3` 或 `huawei/glm-5.2`。
- `capability` 只能是 `TEXT_EMBEDDING` 或 `TEXT_GENERATION`。
- `provider_model_name` 是华为 MaaS 请求中的 `model` 值。
- `credential_mode` 只能是 `PLAINTEXT_DEV` 或 `SECRET_REF`。
- `credential_value` 在开发/测试模式是 API Key；在生产模式是已存在的 CSMS/KMS/keyring
  Secret 引用。过程不显示、回显或记录该值。
- P0 固定 Provider 为 `huawei`。`TEXT_EMBEDDING` 自动使用
  `https://api.modelarts-maas.com/v1/embeddings`；`TEXT_GENERATION` 自动使用
  `https://api.modelarts-maas.com/v2/chat/completions`。客户不传 Endpoint，也无需维护
  Endpoint allowlist。
- `huawei/bge-m3` 的 `TEXT_EMBEDDING` 自动固定为 1024 维；显式请求其他维度在出网前失败。
- `PLAINTEXT_DEV` 只允许 Debug/开发测试实例；Release/生产实例拒绝该模式。

### 2.2 生命周期

`register_model()` 仅允许注册不存在的逻辑模型。`update_model()` 不原地修改已发布行：它创建
递增的内部 `config_version` 并使前一活动版本仅供审计关联，新的调用原子切换到新版本。
`delete_model()` 将全部版本标记为内部 `RETIRED`，不物理删除审计关联所需的版本信息；后续
调用本地失败。

`status`、`config_version`、`Id`、Endpoint、Provider、凭据存储字段和时间戳继续保留在系统表，
但不是 `dbms_ai` 的客户参数或展示字段。`show_models()` 仅返回模型名、能力、实际模型名、
固定维度和内部版本号；不返回状态、Endpoint、API Key、Secret 引用或完整 HTTP 配置。

P0 不设置默认模型。`AI_EMBEDDING()` 必须显式传入 `model_name`；`AI_ANALYZE()` 的第三个
参数必须含有 `model_name`。

## 3. `AI_ANALYZE` 受控契约

函数签名保持不变：

```sql
AI_ANALYZE(task_text, input_value, options_json)
```

其中 `task_text` 是调用方的任务说明，不再直接映射为可覆盖的 Provider `system` message。
Runtime 按 `mode` 选择内置且不可被普通用户覆盖的 system prompt，将任务和业务输入作为
user message 发送。P0 支持 `analyze`、`summarize`、`rag`、`diagnose`、`classify`、`extract`；
它们共用文本生成 Adapter，但 `rag` 和 `diagnose` 使用更严格的输入和输出校验。

- 默认 `output_format='text'`，返回模型最终内容。
- `output_format='json'` 返回 TaurusDB 生成的 JSON，包含 `content`、`model_name`、
  `config_version` 和 `usage`。不返回原始 Provider JSON 或 reasoning。
- `mode='rag'` 且 `return_sources=true` 时，`input_value` 必须是 JSON object，并包含
  `question` 及 `sources` 数组。每个 source 至少有 `source_id`、`chunk_id` 和 `content`。
  Runtime 将 `sources` 的 `source_id`、`chunk_id` 原样写入返回 JSON；模型只能生成
  `content`，不能制造来源。
- `mode='diagnose'` 的输入必须是 JSON object 形式的证据集合。受控 system prompt 强制
  “原因、证据、风险、建议”的只读诊断边界，禁止自动执行或生成修复 SQL。
- `return_sources=true` 仅允许 `mode='rag'` 和 `output_format='json'`。

RAG 调用方仍必须在数据库 SQL 中完成业务租户、访问标签和标量过滤后，才将可访问片段传给
`AI_ANALYZE()`；该函数不尝试绕过或替代数据库的数据访问控制。

## 4. 调用审计与可观测性

`ai_invoke_audit=ON` 时，Runtime 在 MaaS 出站前将 `AI_CALL_STARTED` 追加并 `fsync` 到
JSON Lines 文件；写入失败则请求失败且不出站。调用结束后写 `AI_CALL_SUCCEEDED` 或
`AI_CALL_FAILED`。

终态写入失败时，Runtime 返回 `AUDIT_UNAVAILABLE`，在服务器错误日志中写入不含敏感信息的
告警；已有 `STARTED` 但无终态的 `call_id` 被日志采集平台识别为 `UNKNOWN`。由于终态文件
已经不可写，内核不能伪造成功持久化的 `UNKNOWN` 行。

每次追加均重新打开审计文件，因此 TaurusDB 既有日志轮转可通过 rename/create 生效；保留、
采集、磁盘容量告警和查看权限复用现有日志平台。P0 不新增审计系统表或普通 SQL 审计查询。

## 5. 凭据、网络与错误边界

开发验证可使用 `PLAINTEXT_DEV`；其密钥仅短暂构造 Authorization header，代码不会写入
错误日志、审计日志或 SQL 结果。生产只能使用 `SECRET_REF`，并通过现有 keyring reader
在内存中读取 Secret。

Endpoint 不再由 SQL 管理员输入，运行时只接受由 capability 派生的华为标准 HTTPS Endpoint。
这同时形成 P0 的 Endpoint allowlist，避免 loopback、内网、任意端口和 DNS 重绑定类配置风险。
超时、响应大小上限、HTTP 非 2xx、限流、协议不匹配、凭据错误和响应缺失均在 Runtime 分类，
并仅向 SQL 返回脱敏错误。

云侧 VPC、Policy Route、SNAT/DNAT、EIP 和跨 Region 网络配置由 TaurusDB 云平台实施；本仓库
交付配置约束、错误处理和验证用例，不修改云侧基础设施。

## 6. 测试与验收

默认 MTR 全离线，使用 `mtr/fixture-*`，不得读取真实密钥或访问公网。覆盖原生管理包权限、
注册/更新/删除/展示、无默认模型、Endpoint 自动映射、维度校验、受控 Analyze JSON、RAG
来源、诊断边界、审计起始失败、终态失败和脱敏。

真实 MaaS 验证保持显式 opt-in：使用 `scripts/db4ai_maas_smoke.sql` 验证 Embedding/Analyze，
使用 `scripts/db4ai_maas_real_embedding_rag_smoke.sql` 验证 Embedding、向量索引、STORED
生成列和 RAG。生产 `SECRET_REF`、主备/只读节点、日志采集和跨 Region 网络路径必须在
TaurusDB 目标环境执行并记录为发布验收证据。
