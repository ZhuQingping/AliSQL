# TaurusDB MaaS 商用化差距 Backlog

> **文档角色：任务状态和关闭证据台账，不重新定义设计。** 设计边界以关联主设计为准；Backlog 状态变化
> 必须与主设计中的“当前实现/目标差距”和验证证据同步更新。

**关联设计：** [TaurusDB MySQL 对接华为云 MaaS](taurusdb-maas-p0-committer-design.md)

**用途：** 本文件是 MaaS 特性从当前“主机开发验证可用”走向商用的单一差距清单。设计文档说明
架构和当前事实；本文件只跟踪尚未闭环的交付项。每项完成后必须同时更新状态、验收证据和关联设计，
不能仅删除“待实现”文字。

## 使用规则

- `P0-Blocker`：未完成不得在生产实例开放该能力。
- `P1`：不阻断“华为 MaaS 单 Provider、受限灰度”上线，但阻断更大范围商用或扩展承诺。
- `P2`：后续增强，不纳入当前商用门禁。
- `状态`仅使用：`未开始`、`设计中`、`开发中`、`验证中`、`已关闭`、`不在当前范围`。
- 每项关闭时必须增加：代码/配置变更、离线回归、目标环境验证记录和风险结论。外部依赖未满足时，
  状态保持为`验证中`或`未开始`，不得标记为已关闭。

## 当前结论

当前 `ai_maas` 分支已经完成 Huawei MaaS 的同步 Embedding、Analyze、向量索引/RAG、模型 Profile 管理、实例
总开关、动态权限和本地两阶段审计；真实验证已成功调用 `bge-m3` 和 `glm-5.2`。

当前只适合作为**主机、开发验证或受控灰度**能力。生产商用的主要差距在凭据管控、HA/升级、日志平台、
网络验收、性能容量和生产 DFX，而不是基础 SQL 调用链。

## 2026-08-08 代码—设计差距复核

本次复核基于代码提交 `6307be989878b06d0b6b2aa2ac108ff5ab1241f0`，并重新构建 `mysqld`、
`merge_small_tests-t`，执行 AI 相关 GUnit 和 10 个 DB4AI MTR。结论是：当前代码基线能够稳定回归，
但设计中的 P0 目标尚未全部落地。后续排优先级以本表为入口；“当前实现”描述事实，“剩余工作”才是
待开发范围。

| ID | 当前代码事实 | 剩余实现 | 尚缺测试或外部证据 |
| --- | --- | --- | --- |
| G-001 | Huawei Key 通过启动参数 `rds_api_key` 明文注入内存，SQL 不可读写，模型表不保存 Key。 | 接入 SCC/Secret 密文托管、解密、轮换、吊销；按冻结命名改为 `rds_ai_maas_huawei_api_key`。 | Key 轮换/吊销、进程与日志泄漏扫描、节点一致性及恢复验证。 |
| G-002 | ROW 复制、MIXED 转 ROW、`STORED + AI_EMBEDDING()` 避免备机重复外呼已有离线 MTR。 | 明确并实现主/只读节点、升主和故障切换时的凭据、参数、审计边界。 | `FULL/MINIMAL/NOBLOB` row image、正文/非正文更新、只读节点和切换矩阵的真实环境证据。 |
| G-003 | Registry 仅接受编译期允许的 Huawei HTTPS Endpoint；单机真实 MaaS 曾验证成功。 | 完成各上线 Region 的网络编排、Endpoint/模型准入与发布回退流程。 | DNS、TLS、Policy Route/NAT/EIP、安全组、回流、配额和网络故障演练。 |
| G-004 | 本地 JSONL 两阶段审计已实现；STARTED 写入失败时禁止出站，终态失败保留原调用结果。 | 接入轮转、采集、保留、访问控制、磁盘水位和告警；定义未闭合 STARTED 的运维处理。 | 磁盘满、轮转竞争、长期写入、采集延迟、权限和审计完整性验证。 |
| G-005 | 系统表和动态权限具备基础升级脚本；尚无完整生命周期闭环。 | 实现目标模型表迁移、root 权限补授予、新建实例初始化、回退拦截和恢复重建策略。 | 新建/升级/混部复制/降级/备份恢复的版本矩阵。 |
| G-006 | 审计含 `call_id`、Endpoint 指纹、Provider request id；错误多数仍折叠为 `ER_NOT_SUPPORTED_YET`。 | 为权限、配置、凭据、限流、超时、取消、协议、响应过大和审计失败定义稳定错误。 | SQLSTATE/errno 兼容性、脱敏错误信息及跨阶段关联测试。 |
| G-007 | HTTP Response Body 已有 1 MiB 硬上限；未形成商用容量模型。 | 根据目标环境测量确定内存、连接、线程、审计 I/O 和灰度退出条件。 | 长稳、429、慢请求、网络故障、审计不可写、磁盘满及容量压测。 |
| G-008 | `AI_INVOKE`、`AI_ADMIN` 已注册；当前升级脚本给 `root@'%'` 两项权限均不带转授权。 | 仅允许受限 root 转授 `AI_INVOKE`，不得转授 `AI_ADMIN`；覆盖新建与升级路径。 | root 向业务账号 GRANT/REVOKE、越权拒绝、存量/新建实例 MTR。 |
| G-009 | 两个 SQL 函数的新参数顺序已实现；模型表仍为多版本多行，Embedding 元数据固定 `VECTOR(1024)`，Huawei 请求不发送 `dimensions`，变量仍是旧名。 | 落地单行当前态模型表、`default_dimension` 可空、维度透传与实际维度返回、变量重命名；Analyze 改发 `max_completion_tokens`。 | 数据字典升级、单行版本递增/重新启用、维度省略/指定/不一致 Warning、旧名消失和真实协议测试。 |
| G-010 | 当前仍硬编码每语句最多 32 次、实例最多 32 个在途调用；没有按分钟限频。 | 删除两个 32 硬限制，实现 `rds_ai_maas_max_calls_per_minute` 和 Provider 前令牌桶，0 表示不限。 | 大于 32 次顺序调用、动态升降/取消限频、并发准确性、限流零出站、重启恢复。 |
| G-011 | libcurl 默认连接超时 5 秒、总超时 30 秒；Analyze 最大 60 秒；Response Body 1 MiB；没有 THD 取消回调。 | 连接超时 10 秒，Embedding 默认 60 秒、Analyze 默认 300 秒、最大 1800 秒；实现 `KILL QUERY` 中断、最终 content 独立 1 MiB 检查和 request id 长度上限。 | 连接/上传/等待/下载阶段取消，超时终止整条 SQL，1 MiB 边界、超长 request id 和审计终态测试。 |
| G-012 | `delete_model()` 会标记 RETIRED；没有 `disable_model()`，update 只处理 ACTIVE 并通过插入新行发布。 | 实现 disable、重新启用、单行原位版本递增，以及管控面退市识别、迁移和回退。 | ACTIVE/DISABLED/RETIRED 状态机、并发更新、退市调用拒绝和向量重建流程。 |

### 测试覆盖结论

- **当前已通过：** Debug `mysqld` 和 `merge_small_tests-t` 构建；AI GUnit 26 个、Huawei Adapter
  GUnit 10 个；10 个 DB4AI MTR 全部通过。
- **离线 fixture 的边界：** `mtr/fixture-*` 会绕过真实凭据和 HTTP，只证明 SQL、权限、模型选择、
  VECTOR/RAG、审计和复制的确定性逻辑，不能证明 libcurl、Huawei 请求字段、真实超时或网络故障正确。
- **下一轮必须新增的自动化：** 删除 32 限制与限频、目标超时和 `KILL QUERY`、维度透传和 Warning、
  单行模型表状态机、变量重命名、root 权限委派、嵌套 usage Token 解析、最终 content/request id 边界、
  稳定错误码。
- **只能在集成环境完成的证据：** SCC/Secret、Region 网络、日志平台、主备切换、升级恢复、真实 MaaS
  协议与性能容量。这些不能用离线 MTR 代替。

## 商用阻断项

| ID | 子模块 | 优先级 | 状态 | 责任角色 | 商用验收条件 | 主要依赖 |
| --- | --- | --- | --- | --- | --- | --- |
| G-001 | 凭据控制面 | P0-Blocker | 未开始 | 管控面 / 安全 | SCC/Secret 托管的密文下发、解密、轮换、吊销和节点一致性完成；Key 不出现在 SQL、日志、审计、binlog 或备份中。 | TaurusDB 管控面、KMS/Secret 服务 |
| G-002 | 主备、故障切换与复制 | P0-Blocker | 验证中 | 内核 / RDS HA | 主/只读节点真实调用、审计、凭据下发、升主、ROW image 和故障切换矩阵通过。 | RDS HA、复制、网络 |
| G-003 | 目标 Region 网络与模型准入 | P0-Blocker | 未开始 | 管控面 / 网络 | 每个上线 Region 的 DNS、TLS、Policy Route/NAT/EIP、安全组、回流、模型配额和 Endpoint 连通验证通过。 | VPC、MaaS、SRE |
| G-004 | 审计日志平台闭环 | P0-Blocker | 未开始 | 日志平台 / SRE | 日志平台/Agent（或后续内核）完成文件轮转、采集、保留、访问控制、磁盘水位和告警；STARTED 未闭合可按 `call_id` 追溯为 `UNKNOWN`。 | 日志平台、SRE、安全 |
| G-005 | 升级、回退与备份恢复 | P0-Blocker | 未开始 | 内核 / 升级 | 存量实例升级、旧表迁移、root 动态权限补授予、混部复制、降级拦截和备份恢复演练通过。 | 升级框架、备份恢复、复制 |
| G-006 | 生产错误码与 DFX | P0-Blocker | 设计中 | 内核 / DAS | 为权限、模型、凭据、限流、超时、协议和审计错误定义稳定 SQLSTATE/错误码；控制台可关联 `call_id`、Endpoint 指纹和 Provider request id。 | 内核错误码、DAS/OPS |
| G-007 | 性能、容量与可靠性 | P0-Blocker | 未开始 | 性能测试 / SRE | 目标 Region 完成长稳、限流、429、网络故障、审计不可写和磁盘满演练；形成容量模型、告警阈值和灰度退出条件。 | MaaS 配额、SRE、性能测试 |
| G-008 | 开发者 root 的 AI 调用授权委派 | P0-Blocker | 未开始 | 内核 / 账号管控 | 升级/bootstrap 与新建 root 均使 `root@'%'` 仅对 `AI_INVOKE` 具备动态 `WITH GRANT OPTION`；root 可向业务账号 GRANT/REVOKE `AI_INVOKE`，不能转授 `AI_ADMIN`、总开关或其他系统权限；MTR 覆盖存量升级和新建 root。 | MySQL 动态权限、升级脚本、TaurusDB 账号初始化 |
| G-009 | SQL 接口、模型表与变量命名冻结 | P0-Blocker | 开发中 | 内核 | `AI_EMBEDDING(model_name,text[,options])` 的维度/超时语义、`AI_ANALYZE(model_name,prompt[,options])` 的输出参数映射、`mysql.ai_model_config` 单行当前态结构，以及 `rds_ai_maas_*` 变量命名完成代码、升级和 MTR 对齐；不存在旧参数顺序、旧变量名、固定 1024 返回元数据或多行版本选择。 | SQL 函数、`dbms_ai`、系统变量、数据字典升级、Registry |
| G-010 | 实例级调用频率限制 | P0-Blocker | 未开始 | 内核 / 管控面 | 实现 `rds_ai_maas_max_calls_per_minute` 的全局动态参数和 Provider 前令牌桶；0 表示不限，正整数限制新外呼；管控参数系统完成持久化、重启和节点替换恢复，MTR 覆盖并发申请与零出站；删除当前每语句 32 次和实例并发 32 的硬编码限制。 | Runtime、参数系统、TaurusDB 管控面 |
| G-011 | 超时、取消与输出边界 | P0-Blocker | 开发中 | 内核 | 落地连接超时、Embedding/Analyze 单次 `timeout_ms`，不新增语句累计等待变量；`KILL QUERY` 可中断 libcurl 等待并正确审计；HTTP Body 与最终 content 均有 1 MiB 边界，输出 Token 参数按模型校验，故障注入和长响应测试通过。 | libcurl Transport、THD、错误码、性能测试 |
| G-012 | 模型生命周期与退市 | P0-Blocker | 开发中 | 管控面 / 内核 | `dbms_ai.disable_model()`、更新重新启用、`delete_model()` 标记 RETIRED、MaaS 模型退市识别、客户迁移和回退闭环完成；向量模型不得静默替换，升级需重算向量并重建索引。 | `dbms_ai`、模型目录、管控面、MaaS |

## P1：产品扩展与规模化能力

| ID | 子模块 | 状态 | 责任角色 | 完成标准 |
| --- | --- | --- | --- | --- |
| G-101 | 多 Provider Adapter | 未开始 | 内核 / Provider 接入 | 百炼、火山方舟、Bedrock 等各自具备 Adapter、认证解析、Endpoint 策略、选项 Schema、真实 smoke 与错误映射；不能仅在模型表加入 Provider 名。 |
| G-102 | 模型治理 | 设计中 | 管控面 / 内核 | 提供模型可用性探测、受控发布/回退、模型级限流、预算/Token/费用统计和运行状态；不开放任意 URL、Header 或 Provider JSON 透传。 |
| G-103 | Registry 缓存与一致性 | 未开始 | 内核 | 引入进程级 Profile 缓存、配置版本失效/刷新机制和并发更新语义；调用使用稳定快照，且升级/复制后无旧配置泄漏。 |
| G-104 | RAG 应用集成 | 未开始 | 应用 / 产品 | 应用/知识库服务交付资料解析、切分、版本、ACL、引用展示、反馈运营与重建任务；SQL 必须先做授权过滤。 |
| G-105 | DBA 诊断集成 | 未开始 | DAS / 产品 | 交付快照采集、脱敏、控制台展示、人工审批和变更单联动；模型输出永不自动执行。 |
| G-106 | 基于容量证据的并发保护 | 未开始 | 内核 / 性能测试 | P0 不预设 MaaS 专用并发上限。商用容量与长稳测试如证明慢请求会造成不可接受的连接、内存或线程堆积，再评审并发保护；设计须由实测阈值驱动，不预先冻结 32 或增加无证据参数。 |

## P2：明确不属于当前交付的能力

| ID | 能力 | 当前结论 | 后续触发条件 |
| --- | --- | --- | --- |
| G-201 | 异步/批量推理 | 当前只支持同步调用。 | 有明确离线导入、批处理或队列需求，并完成幂等、费用、取消和审计设计。 |
| G-202 | 流式、多模态、工具调用、Agent | 不属于当前 SQL 数据面。 | Provider、数据库资源隔离和安全模型均明确后单独立项。 |
| G-203 | 服务端 tenant/模型级授权 | 当前仅实例级 `AI_INVOKE` / `AI_ADMIN`。 | 出现多租隔离、按模型/预算授权的产品需求后设计。 |

## 当前实现已具备但仍需补测的边界

以下能力已在代码中存在，不应再列为“待开发”；其工作项是补齐自动化或目标环境证据：

- 输入和响应不超过 1 MiB；需补齐超限零出站和容量压测证据。当前每语句 32 次、实例并发 32 是待删除
  的实现差距，不作为产品规格。
- `ProviderEndpointPolicy` 已限制 Huawei 的 HTTPS Host、端口和路径；需补齐生产 Endpoint 发布、回退与网络环境证据。
- ROW 复制和 `STORED + AI_EMBEDDING()` 已有离线 MTR；需补齐目标主备、ROW image 与故障切换实测。
- 本地两阶段审计已实现并通过真实调用验证；需补齐日志平台采集、轮转、告警和访问控制。

## 关闭记录

| 日期 | ID | 结论 | 证据 |
| --- | --- | --- | --- |
| 2026-08-06 | 基线 | Huawei `bge-m3` Embedding、`glm-5.2` Analyze、STORED 向量与 RAG 真实 MaaS smoke 通过；离线 MTR 10 个 MaaS 用例加 shutdown report 通过。 | `scripts/db4ai_maas_smoke.sh`、`scripts/db4ai_maas_real_embedding_rag_smoke.sh`、`mysql-test/suite/rds/t/ai_maas_*.test` |
