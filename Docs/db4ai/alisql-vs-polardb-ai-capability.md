# AliSQL DB4AI P0 与 PolarDB for MySQL：能力与证据边界

更新日期：2026-07-30。本文件不是基准报告，也不是 SQL 兼容性承诺。它把本分支的
可复验证据、阿里云公开文档所述能力和没有验证的部分分开记录；除非另有说明，PolarDB
列均是文档观察，**不是**在本工作区或客户集群上的实测结果。

## 前提与术语

AliSQL P0 是服务器内置的 `AI_EMBEDDING` / `AI_ANALYZE` 调用面：它通过受控 Profile
访问华为 MaaS，默认测试没有云端 egress。PolarDB 文档中同时出现两类 AI 路径：
`/*polar4ai*/ PREDICT`（PolarDB for AI 的内置模型/AI node）以及 `EMBEDDING()`
（Model Studio embedding）。两者的部署前提、模型来源与语法都不同，不能与 AliSQL P0
做函数级替换或性能等价推断。

官方文档声明 PolarDB for AI 需要 Enterprise Cluster Edition、相应内核/PolarProxy 版本及
AI node，并按 AI node 计费；向量索引另需要 IMCI 只读节点。具体版本和资源要求应以部署时
的 [PolarDB for AI 文档](https://help.aliyun.com/en/polardb/polardb-for-mysql/polardb-for-ai/)
和 [向量检索文档](https://help.aliyun.com/en/polardb/polardb-for-mysql/vector-index-usage)
为准。

## 需求到证据矩阵

| 能力 | AliSQL P0：本分支已验证 | PolarDB for MySQL：公开资料观察 | 不作出的结论 |
|---|---|---|---|
| SQL 向量 | `VECTOR(N)`、HNSW、余弦检索由 `rds.vidx_func`；`rds.ai_maas_rag` 验证实际 `AI_EMBEDDING` 写入 VECTOR INDEX。 | 官方文档描述 `VECTOR(N)`、IMCI/HNSW 和 `DISTANCE()`，有单独的 IMCI 部署前提。 | 不比较召回、延迟、并发、索引构建或事务性能。 |
| 文本 embedding | `AI_EMBEDDING(text, model_name [, dimension])` 固定受 Profile/实例级 `AI_INVOKE` 约束；bge-m3 1024 维本地校验，Debug MTR 无 egress。 | 官方文档描述 `EMBEDDING(text, model_name, dimension)`，目前标为 beta，并列出 Model Studio 模型与支持维度。 | 不宣称 API、维度、模型、认证或错误语义兼容。 |
| 生成/分析 | `AI_ANALYZE(model_name, prompt [, options_json])` 仅接受稳定 options；Huawei V2 Chat 的 final content、长度截断和 reasoning-only 均受校验。 | 官方 PolarDB for AI 文档给出 `/*polar4ai*/ PREDICT(MODEL ..., SELECT ...)` 的聊天、摘要、情感分析和 NL2SQL 示例。 | 不把 `AI_ANALYZE` 视为 `PREDICT` 的别名，也不推断任一模型输出质量。 |
| RAG | `rds.ai_maas_rag` 验证 tenant/业务/space/version SQL 过滤、schema contract 拒绝、数据库来源回传；示例见 `examples/rag_product_manual.sql`。 | 官方资料描述向量化、检索和 RAG/文档检索流程，含 `PREDICT` 批量异步向量化示例。 | 不声称两者的索引格式、批处理、权限隔离或来源可追溯行为相同。 |
| 凭据与 egress | Profile 仅允许 Huawei HTTPS JSON；生产 `SECRET_REF`，Debug `PLAINTEXT_DEV` 被 Release 拒绝；无凭据 fail-closed。 | 公开文档描述 AI node、内置模型或 Model Studio 路径，但本分支未对其凭据/网络控制做审计。 | 不比较密钥托管、VPC、出网控制或合规效果。 |
| 审计与权限 | 持久 `STARTED`→完成的脱敏本地审计文件、实例级 `AI_INVOKE` 与 `AI_ADMIN` 由 MTR 覆盖；P0 不提供普通 SQL 审计文件查询。 | 本次未找到并验证与此同范围的 PolarDB 审计/权限 SQL 合约。 | 不声称 PolarDB 缺少审计；仅标记为未验证。 |
| 运维与成本 | 真实 MaaS 仅通过显式授权的短 smoke 执行；默认 GUnit/MTR 离线。 | 官方文档说明 PolarDB for AI 需要 AI node 且会按节点计费；RAG 文档也描述异步批量向量化。 | 不比较云费用、节点成本、吞吐或服务等级。 |

PolarDB 的功能观察分别来自官方的 [text embedding](https://help.aliyun.com/en/polardb/polardb-for-mysql/use-the-embedding-function)、[document retrieval](https://help.aliyun.com/en/polardb/polardb-for-mysql/case-2-building-a-document-retrieval-system) 和
[PolarDB for AI](https://help.aliyun.com/en/polardb/polardb-for-mysql/polardb-for-ai/) 文档。上述
链接只说明厂商公开能力，版本、地区、白名单、节点规格与计费会变化。

## AliSQL P0 交付判断

本分支对 P0 的可验收范围是：受控 Huawei MaaS embedding/V2 Chat、离线可重复的 SQL/MTR
合同、VECTOR RAG 业务约束、只读分析/诊断、持久脱敏审计，以及一次明确授权后的最小云端
smoke。生产仍未完成的门禁包括 Profile 管理 SQL、通用 Profile 驱动的 embedding contract
强制、审计保留/重试/仪表盘和其他 Provider Adapter；详见
[`alisql-maas-p0-validation-status.md`](alisql-maas-p0-validation-status.md)。

任何迁移或选型需要在目标版本、地区、模型准入、节点规格、数据规模和真实 workload 下做
独立的兼容性、成本、安全与性能验证。
