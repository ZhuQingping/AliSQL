# TaurusDB MySQL 对接华为云 MaaS P0 需求与验收基线

目标版本：2026-09-30 P0 预览版本  
状态：评审稿  
关联设计：[TaurusDB 对接华为云 MaaS P0 High Level Design](taurusdb-maas-p0-high-level-design.md)

## 1. 明确业务场景和交付范围

### (1) 支持库内文本向量化

支持应用通过 `AI_EMBEDDING()` SQL 接口调用华为云 MaaS 的文本向量模型。P0 当前支持
`bge-m3` 文本向量化模型，输出 1024 维向量。模型、Endpoint、配置版本和凭据引用均由
TaurusDB 控制面配置；业务 SQL 不得传入 API Key、Endpoint 或厂商私有 JSON。

### (2) 支持向量数据写入和 RAG 检索

支持将 `AI_EMBEDDING()` 输出写入向量列，以及使用 `STORED` 生成列自动生成向量；配合
VECTOR INDEX、业务标量过滤和向量召回，实现 RAG 检索。RAG 查询必须在 SQL 层完成 tenant、
权限或业务标签过滤，不能只按向量距离取 Top-K；结果必须返回稳定的来源标识。

### (3) 支持 SQL 结果分析与 DBA 诊断

支持通过 `AI_ANALYZE()` 调用华为云 MaaS 文本生成模型，用于 SQL 结果摘要、分类、抽取、
RAG 回答和智能诊断。诊断只返回原因、证据、建议与风险，不执行自动修复 SQL。P0 验收前
冻结一个已开通的 MaaS V2 Chat Profile、模型 ID、Endpoint 和 Region。

### (4) 支持管理员配置和迭代模型

支持具备 `AI_ADMIN` 的管理员通过受控模型 Profile 管理路径新增、发布、停用和切换模型
配置。华为云 MaaS 推出新的或新版本的向量模型、文本生成模型时，管理员可以在不修改
业务 SQL 的前提下新增 Profile 并切换至新模型。

该能力仅适用于已实现的华为 MaaS Embedding 或 V2 Chat 协议、且已完成 Endpoint、维度、
凭据、模型能力和兼容性校验的模型。新协议、新 capability、可变维度模型或新 Provider
仍需要新增 Adapter 与独立验收，不能只插入一条配置就直接上线。

### (5) 支持权限控制

`AI_ADMIN` 用于受控模型 Profile 管理；`AI_INVOKE` 控制 AI 模型调用。P0 采用实例级
调用权限：拥有 `AI_INVOKE` 的有效数据库账号可以调用全部 ACTIVE 的 P0 模型，不实现
tenant 到账号、账号到模型的绑定表，也不实现按模型的细粒度授权。

### (6) 支持模型调用审计

支持记录模型调用审计信息，包括调用起始、调用成功或失败、执行耗时、Token 用量、模型、
用户和客户端 IP 等脱敏信息。默认开启 `ai_invoke_audit=ON`；审计采用出站前 `AI_CALL_STARTED`
和出站后终态事件组成的两阶段日志。审计日志写入受控文件，不写 AI 系统审计表。

### (7) 主机和备机均支持模型调用

主机和备机均支持 `AI_EMBEDDING()`、`AI_ANALYZE()` 和模型调用审计。主机支持模型配置
管理；备机不允许修改模型 Profile。主备节点均写受控 AI 审计日志文件，不依赖写入系统表。

## 2. 具体规格、指标是什么

### (1) 构建与离线回归指标

在指定 Release 构建配置下，`mysqld` 构建必须成功；DB4AI 相关 GUnit 和 MTR 回归必须
100% 通过。离线用例必须覆盖 `AI_EMBEDDING()`、`AI_ANALYZE()`、模型 Profile 解析、
`AI_INVOKE` 拒绝、维度检查、本地 fail-closed、HTTP/timeout/429/403 错误映射、VECTOR
编码、RAG 标量过滤、脱敏日志和主备审计日志行为。

### (2) 模型与网络配置冻结指标

验收前冻结并记录 TaurusDB Region、MaaS Region、Endpoint、逻辑模型名、Provider 模型 ID、
模型版本或 `UNRESOLVED` 标记、Profile 配置版本、凭据引用标识和网络出口路径。

`bge-m3` 必须返回 1024 维有限浮点向量；请求其他维度时必须在网络出站前失败。验收前必须
重新确认 MaaS 模型准入、Region 和接口可用性，不能以历史测试结果替代现场验证。

### (3) Embedding 功能指标

在真实 MaaS 环境中，以单条输入不超过 2 KiB UTF-8 的测试数据连续执行 100 次
`AI_EMBEDDING()`：

- 100 次均返回 1024 维、无 NaN/Inf 的 VECTOR。
- 无 `AI_INVOKE`、模型不存在、模型非 ACTIVE、维度不符、凭据缺失或 Endpoint 不被允许时，
  调用必须失败。
- 对于数据库本地可判定的失败，MaaS 请求数必须为 0。
- 每次实际外呼均必须可通过 `call_id` 在审计日志中关联到起始和最终状态。

### (4) Analyze 功能指标

对验收前冻结的 V2 Chat Profile，以输入不超过 2 KiB UTF-8、输出上限 512 Token 的条件
连续执行 50 次 `AI_ANALYZE()`：

- 成功率不低于 98%。
- 每个成功响应必须有非空 final 内容。
- reasoning-only、空 final 内容和 `finish_reason=length` 必须按定义返回不完整输出错误，
  不能伪装为成功。
- 用户不得通过 `options_json` 透传厂商私有参数、Endpoint 或模型 ID。

### (5) 模型配置管理指标

管理员必须能够在主机上完成一个兼容模型 Profile 的新增、校验、发布、停用和回滚。新增或
发布后，新调用必须解析到新的 `config_version`；停用后，后续调用必须在网络出站前拒绝。
每个 capability 至多允许一个 ACTIVE 的实例级默认 Profile；显式指定 `model_name` 时使用
指定模型，省略时使用对应 capability 的默认模型。

生产环境必须使用 `SECRET_REF` 或 TaurusDB 指定的安全凭据引用；`PLAINTEXT_DEV` 必须被
Release 环境拒绝。真实 API Key 不得出现在系统表查询面、SQL 历史、binlog、错误日志、
审计日志、MTR 输出或验收报告中。

### (6) 权限控制指标

未持有 `AI_INVOKE` 的账号调用 `AI_EMBEDDING()` 或 `AI_ANALYZE()` 必须被拒绝，且不得
读取凭据或发起 MaaS 请求。持有 `AI_INVOKE` 的账号可调用全部 ACTIVE P0 Profile。只有
拥有对应管理权限的管理员能够执行模型 Profile 管理和 `SET GLOBAL ai_invoke_audit`；不支持
`SET SESSION ai_invoke_audit`。

### (7) 调用审计指标

`ai_invoke_audit` 默认值必须为 `ON`。审计开启时，主机和备机各完成至少 20 次真实
Embedding 调用和 10 次真实 Analyze 调用：

- 每次 MaaS 出站前必须成功写入一条 `AI_CALL_STARTED`。
- 成功、可分类失败和超时必须写入对应终态；进程中断或终态无法落盘时，必须保留
  `STARTED` 并可按 `call_id` 识别为 `UNKNOWN`。
- 模拟审计文件不可写、磁盘满或滚动失败时，调用必须在 MaaS 出站前失败，MaaS 请求数为 0。
- 审计文件中不得出现 API Key、Authorization、`credential_ref`、完整 prompt、完整响应或
  原始 embedding。

### (8) RAG、STORED 生成列和向量质量指标

冻结 50 篇文档、至少 4 个业务分类、20 条问题和每条问题的标准答案来源。真实向量化后，
20 条问题的 `Recall@3` 不低于 90%；任一问题返回不属于其 tenant、业务标签或访问范围的
文档数量必须为 0。

在真实 MaaS 环境中，插入 4 条文档必须产生 4 次 embedding 调用；更新 1 条文档的 `doc`
必须额外产生 1 次 embedding 调用。在 `binlog_format=ROW` 且
`binlog_row_image=MINIMAL` 下，仅更新与 `doc` 无关的字段，必须产生 0 次额外 embedding
调用。审计日志中的调用数必须与上述预期完全一致。

### (9) 性能指标

以下阈值是 P0 低频同步调用目标，不代表 OLTP SLO：

- Embedding 单并发 100 次：P95 不高于 5 秒，P99 不高于 10 秒，成功率不低于 99%。
- Embedding 4 并发 200 次：P95 不高于 8 秒，P99 不高于 15 秒，成功率不低于 98%。
- Analyze 单并发 50 次：P95 不高于 15 秒，P99 不高于 30 秒，成功率不低于 98%。
- 429、超时和网络失败均计入失败，不允许通过客户端静默重试掩盖失败率。

### (10) 验收证据指标

截至 2026-09-30，必须提交可复跑、脱敏的验收报告，至少包含构建 commit、Profile 配置版本、
Region、Endpoint 哈希、网络出口说明、测试时间窗、并发、输入规模、P50/P95/P99、成功率、
错误分类、审计事件计数、日志脱敏检查和清理结果。

## 3. 约束和限制

### (1) 只支持同步调用模型

只支持同步调用模型，暂不支持异步和批量调用模型。`AI_EMBEDDING()` 和 `AI_ANALYZE()` 会
占用数据库执行线程并等待外部网络和模型推理结果；P0 仅适用于低频、显式触发的调用，
不适用于交易热路径、无界大表逐行扫描、在线同步批处理或高并发实时服务。

### (2) 超时、连接中断或事务回滚仍可能产生模型调用

请求超时、连接中断或用户事务回滚，不代表 MaaS 未接收请求、未完成推理或未产生费用。P0
默认不对超时请求自动重试；后续若引入重试，必须先解决幂等标识、重复收费和审计关联问题。

### (3) STORED 生成列受复制配置约束

使用 `STORED + AI_EMBEDDING()` 的实例必须满足 `ROW` binlog 下的
`binlog_row_image=MINIMAL` 约束。若无法保证该配置，不应在生产中启用该建表范式；否则一次
非文档字段更新也可能重新触发外部 embedding 调用。

### (4) P0 采用实例级粗粒度调用权限

`AI_INVOKE` 用户可调用全部 ACTIVE 模型。若客户要求不同用户使用不同模型、不同 API Key、
不同 Endpoint、不同预算或配额，属于后续按账号/模型授权模块，不在 P0 范围。

### (5) 模型和向量索引必须版本化

`bge-m3` P0 固定为 1024 维。切换模型、模型版本或维度时，必须创建新的 Profile、
`embedding_space_id`、向量列/索引和回填任务，不能把新旧向量混入同一索引。COSINE 或
EUCLIDEAN 由向量索引和检索语句选择，不是模型凭据或模型本身的属性。

### (6) 网络能力按实际 Region 验收

P0 不承诺任意 Region 自动可访问 MaaS。每个计划上线的 TaurusDB Region 都必须单独完成
VPC 出站、路由、NAT/EIP、DNS、TLS、Endpoint allowlist 和 MaaS 服务准入验证。Embedding 与
Chat 若位于不同 MaaS Region，必须分别验收。

### (7) 审计开关关闭期间不具备审计保证

`ai_invoke_audit` 默认开启，但管理员可以动态关闭。开关关闭期间允许 AI 调用时，该期间不
满足“每次调用可追溯”的审计承诺。因此 9/30 正式验收和生产基线必须保持 `ON`；关闭仅应
作为受控运维操作，并纳入 TaurusDB 常规参数变更审计。

### (8) 审计日志文件不是数据库查询表

P0 不提供普通 SQL 读取日志文件，也不提供 `AI_AUDIT_INFO()` 作为正式审计查询面。审计检索、
保留、访问控制和集中采集由 TaurusDB 日志平台承担。

## 评审与批注约定

请直接在有疑问的条目后加入以下 Markdown 块，不必修改原文：

```markdown
> [REVIEW-001][QUESTION][2.9]
> 问题：Embedding P95 5 秒的依据是什么？
> 期望：改为 3 秒，或补充测量依据。
> 状态：OPEN
```

- 编号按 `REVIEW-001`、`REVIEW-002` 递增。
- 类型使用 `QUESTION`（需要解释）、`CHANGE`（建议修改）、`BLOCKER`（不解决则不能验收）或
  `ACCEPT`（确认接受）。
- 方括号中的最后一项是条目编号，例如 `[2.9]` 表示第 2 章第 9 项。
- 我处理后会保留原批注，并补充 `回复：` 和 `状态：RESOLVED`。
