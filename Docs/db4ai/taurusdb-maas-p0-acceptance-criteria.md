# TaurusDB MySQL 对接华为云 MaaS P0 验收基线

目标版本：2026-09-30 P0 预览版本  
状态：评审稿  
关联设计：[TaurusDB 对接华为云 MaaS P0 High Level Design](taurusdb-maas-p0-high-level-design.md)

## 评审与批注约定

请直接在有疑问的条目后加入以下 Markdown 块，不必修改原文：

```markdown
> [REVIEW-001][QUESTION][2.8]
> 问题：P95 5 秒的依据是什么？
> 期望：改为 3 秒，或补充测量依据。
> 状态：OPEN
```

- 编号按 `REVIEW-001`、`REVIEW-002` 递增，便于逐项跟踪。
- 类型使用 `QUESTION`（需要解释）、`CHANGE`（建议修改）、`BLOCKER`（不解决则不能验收）或
  `ACCEPT`（确认接受）。
- 方括号中的最后一项是条目编号，例如 `[2.8]` 表示第 2 章第 8 项。
- 我处理后会保留原批注，并补充 `回复：` 和 `状态：RESOLVED`，避免讨论结论丢失。
- 如果问题针对一句原文，请在“问题”前复制该句的前 10 至 20 个字；无需截图或另行描述上下文。

---

## 1. 业务场景和交付范围

### 1.1 文本向量化

TaurusDB MySQL 支持应用通过 `AI_EMBEDDING()` 调用华为云 MaaS 的文本向量模型。P0 固定验证
`huawei/bge-m3`、标准 `/v1/embeddings` 接口和 1024 维输出。模型、Endpoint、配置版本和
凭据引用均由 DBA/控制面配置，业务 SQL 不得传入 API Key、Endpoint 或厂商私有 JSON。

### 1.2 知识库写入和 RAG 检索

支持将 `AI_EMBEDDING()` 用于显式写入向量列，以及 `STORED` 生成列自动生成向量；支持
VECTOR INDEX、业务标量过滤、向量召回和来源返回。RAG 查询必须在 SQL 层完成租户、权限或
业务标签过滤，不能只按向量距离取 Top-K。

### 1.3 SQL 结果分析与 DBA 只读诊断

支持 `AI_ANALYZE()` 调用一个在验收前固定的华为 MaaS V2 Chat Profile，用于 SQL 结果摘要、
分类、抽取、RAG 回答和只读诊断建议。诊断只返回原因、证据、建议与风险，不执行自动修复 SQL。

### 1.4 简化后的模型与权限控制

P0 仅保留模型配置表。拥有 `AI_INVOKE` 的数据库账号可以调用全部 ACTIVE 的 P0 模型；不实现
tenant 到账号、账号到模型的绑定表，不实现按模型的细粒度授权。`AI_ADMIN` 用于受控模型
Profile 管理。`AI_AUDIT_VIEWER` 不作为 P0 的数据库内日志读取能力交付。

### 1.5 主节点和只读节点调用

主节点与只读节点均支持调用 AI 接口。二者均不写 AI 系统审计表，而是写受控 AI 审计日志文件；
只读节点不能因审计写入而拒绝正常调用。

### 1.6 审计与计量

默认开启 `ai_invoke_audit=ON`。每次实际外呼采用两阶段审计：出站前记录 `AI_CALL_STARTED`，
返回后记录成功、失败或未知终态，以及耗时、Token、模型、用户、客户端 IP 等脱敏信息。

### 1.7 本版本明确不交付

不交付百炼、火山方舟、Bedrock Adapter；不交付流式输出、工具调用、多模态、异步任务、
批量回填平台、按模型授权、数据库 SQL 直接查询审计文件，以及高并发 OLTP 场景保障。

## 2. 量化、可衡量的验收指标

### 2.1 构建与离线回归

在指定 Release 构建配置下完成构建；DB4AI 的 GUnit 与 MTR 回归必须 100% 通过。MTR 必须覆盖
`AI_EMBEDDING()`、`AI_ANALYZE()`、模型 Profile 解析、`AI_INVOKE` 拒绝、错误维度、本地
fail-closed、HTTP/timeout/429/403 错误映射、向量编码、RAG 标量过滤和脱敏日志。

### 2.2 模型与真实网络路径冻结

验收前冻结并记录 TaurusDB Region、MaaS Region、Endpoint、逻辑模型名、Provider 模型 ID、
模型版本或 `UNRESOLVED` 标记、配置版本、凭据引用标识和网络出口路径。

对 `bge-m3`，验收要求返回 1024 维有限浮点向量；指定非 1024 维时必须在网络出站前失败。
华为云公开资料当前将标准 BGE-M3 Embedding 列为贵阳一、1024 维，因此验收前必须重新确认
模型准入和区域信息，不能以历史资料替代现场验证。

### 2.3 Embedding 功能

在真实 MaaS 环境中，以单条输入不超过 2 KiB UTF-8 的测试数据连续执行 100 次
`AI_EMBEDDING()`：

- 100 次均返回 1024 维、无 NaN/Inf 的 VECTOR。
- 任一无权限、模型不存在、模型非 ACTIVE、维度不符、凭据缺失或 Endpoint 不被允许的请求，
  必须失败。
- 对于本地可判定的失败，MaaS 请求数必须为 0。
- 每次实际外呼均可在审计日志中通过 `call_id` 关联到起始和最终状态。

### 2.4 Analyze 功能

对冻结的 V2 Chat Profile，以输入不超过 2 KiB UTF-8、输出上限 512 Token 的条件连续执行
50 次 `AI_ANALYZE()`：

- 成功率不低于 98%。
- 每个成功响应必须有非空 final 内容。
- reasoning-only、空 final 内容和 `finish_reason=length` 必须按定义返回不完整输出错误，
  不能伪装为成功。
- 不允许用户通过 `options_json` 透传厂商私有参数、Endpoint 或模型 ID。

### 2.5 RAG 与向量质量

冻结 50 篇文档、至少 4 个业务分类、20 条问题和每条问题的标准答案来源。真实向量化后：

- 20 条问题的 `Recall@3` 不低于 90%。
- 任一问题返回不属于其 tenant、业务标签或访问范围的文档数量必须为 0。
- 返回结果必须包含稳定的 `source_id`、`chunk_id` 或等价来源字段。
- 同一索引不得混用不同模型版本、维度或 `embedding_space_id`。

### 2.6 STORED 生成列

在真实 MaaS 环境中：

- 插入 4 条文档，必须产生 4 次 embedding 调用。
- 更新 1 条文档的 `doc`，必须额外产生 1 次 embedding 调用。
- 在 `binlog_format=ROW` 且 `binlog_row_image=MINIMAL` 下，仅更新与 `doc` 无关的字段，
  必须产生 0 次额外 embedding 调用。
- 审计日志中的调用数与上述预期完全一致。

### 2.7 审计文件

`ai_invoke_audit` 默认必须为 `ON`，且仅管理员能执行 `SET GLOBAL`，不支持 `SET SESSION`。

审计开启时：

- 主节点和只读节点各完成至少 20 次真实 embedding 调用和 10 次真实 Analyze 调用。
- 每次外呼在请求发出前必须有一条 `AI_CALL_STARTED`。
- 成功、可分类失败和超时必须有对应终态；进程中断或终态无法落盘时必须保留 `STARTED` 并可识别为 `UNKNOWN`。
- 模拟审计文件不可写、磁盘满或滚动失败时，调用必须在 MaaS 出站前失败，MaaS 请求数为 0。
- 审计文件中不得出现 API Key、Authorization、`credential_ref`、完整 prompt、完整响应或原始 embedding。

### 2.8 性能

以下阈值是 P0 低频同步调用目标，不代表 OLTP SLO：

- Embedding 单并发 100 次：P95 不高于 5 秒，P99 不高于 10 秒，成功率不低于 99%。
- Embedding 4 并发 200 次：P95 不高于 8 秒，P99 不高于 15 秒，成功率不低于 98%。
- Analyze 单并发 50 次：P95 不高于 15 秒，P99 不高于 30 秒，成功率不低于 98%。
- 429、超时和网络失败均计入失败，不允许通过客户端静默重试掩盖失败率。

### 2.9 生产安全

Release 环境必须使用 `SECRET_REF` 或 TaurusDB 指定的安全凭据引用；`PLAINTEXT_DEV` 必须被拒绝。
真实 API Key 不得出现在系统表查询面、SQL 历史、binlog、错误日志、审计日志、MTR 输出或验收报告中。

### 2.10 验收证据

截至 2026-09-30，必须提交可复跑、脱敏的验收报告，至少包含构建 commit、模型配置版本、
Region、Endpoint 哈希、网络出口说明、测试时间窗、并发、输入规模、P50/P95/P99、成功率、
错误分类、审计事件计数、日志脱敏检查和清理结果。

## 3. 约束和限制

### 3.1 同步外部依赖

`AI_EMBEDDING()` 和 `AI_ANALYZE()` 会占用数据库执行线程并等待外部网络和模型推理结果。
P0 仅适用于低频、显式触发的调用，不适用于交易热路径、无界大表逐行扫描、在线同步批处理
或高并发实时服务。

### 3.2 超时、回滚与费用

请求超时、连接中断或用户事务回滚，不代表 MaaS 未接收请求、未完成推理或未产生费用。P0
默认不对超时请求自动重试；后续若引入重试，必须先解决幂等标识、重复收费和审计关联问题。

### 3.3 STORED 的复制配置

使用 `STORED + AI_EMBEDDING()` 的实例必须满足 `ROW` binlog 下的
`binlog_row_image=MINIMAL` 约束。若无法保证该配置，不应在生产中启用该建表范式；否则一次
非文档字段更新也可能重新触发外部 embedding 调用。

### 3.4 实例级调用权限

`AI_INVOKE` 用户可调用全部 ACTIVE 模型。若客户要求不同用户使用不同模型、不同 API Key、
不同 Endpoint、不同预算或配额，属于后续按账号/模型授权模块，不在 P0 范围。

### 3.5 模型与向量索引版本化

`bge-m3` P0 固定为 1024 维。切换模型、模型版本或维度时，必须创建新的 Profile、
`embedding_space_id`、向量列/索引和回填任务，不能把新旧向量混入同一索引。COSINE 或
EUCLIDEAN 由向量索引和检索语句选择，不是模型凭据或模型本身的属性。

### 3.6 网络能力按 Region 验收

P0 不承诺任意 Region 自动可访问 MaaS。每个计划上线的 TaurusDB Region 都必须单独完成
VPC 出站、路由、NAT/EIP、DNS、TLS、Endpoint allowlist 和 MaaS 服务准入验证。Embedding 与
Chat 若位于不同 MaaS Region，必须分别验收。

### 3.7 审计开关关闭期间

`ai_invoke_audit` 默认开启，但管理员可以动态关闭。开关关闭期间允许 AI 调用时，该期间不满足
“每次调用可追溯”的审计承诺。因此 9/30 正式验收和生产基线必须保持 `ON`；关闭仅应作为
受控运维操作，并纳入 TaurusDB 常规参数变更审计。

### 3.8 审计日志文件不是数据库查询表

P0 不提供普通 SQL 读取日志文件，也不提供 `AI_AUDIT_INFO()` 作为正式审计查询面。审计检索、
保留、访问控制和集中采集由 TaurusDB 日志平台承担。
