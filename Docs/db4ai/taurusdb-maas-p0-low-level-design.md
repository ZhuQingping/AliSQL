# TaurusDB 对接华为云 MaaS P0 Low Level Design

日期：2026-08-04

> **状态：重组中，以下第 1～7 章为当前目标设计。**后附的早期原型章节仅保留实现溯源，
> 不得作为接口、凭据或控制面行为的依据；其中 `PLAINTEXT_DEV`、旧 `dbms_ai` 参数和硬编码
> Endpoint 均须按本设计迁移或删除。

## 1. 模块划分与实现依赖

| 模块 | 核心实现对象 | 持久/内存状态 | 完成定义 |
| --- | --- | --- | --- |
| 数据面 | `item_ai_func`、`Ai_runtime`、Adapter、Transport | 单语句 `Ai_request`/`Ai_response`/Profile 快照 | 本地失败零出站；成功结果映射为 SQL/VECTOR。 |
| 模型管理 | `dbms_ai`、`Ai_model_registry` | `mysql.ai_model_config`、不可变 Profile 快照 | 管理发布、复制、回退不影响运行中请求。 |
| 权限与开关 | 动态权限、`rds_ai_maas` | `mysql.global_grants`、全局开关 | 开关/权限在 Profile、审计、凭据和网络前阻断。 |
| 凭据与路由 | Credential Resolver、Endpoint policy | Huawei `rds_api_key`、外部 `credential_id` | 密钥只在内存存在，URL 不可由普通 SQL 覆盖。 |
| 审计与 DFX | `Ai_file_audit_sink`、`call_id` | JSON Lines STARTED/terminal | STARTED 失败 fail closed；终态失败按 UNKNOWN。 |
| 向量与 RAG | `Ai_vector_codec`、VECTOR SQL | `VECTOR` 值和索引由业务表持有 | 校验模型/版本/维度；SQL 先完成访问过滤。 |

### 1.1 统一数据面对象

```text
Ai_request  = { capability, model_name, input, options, thd_context }
Profile     = { id, config_version, provider, provider_model, endpoint, dimension }
Ai_response = { text | float_vector, usage, provider_request_id, error_category }
Call_context= { profile_snapshot, call_id, effective_timeout, credential_handle }
```

`Ai_request` 不保存明文 Key、Authorization 或 Provider 原始 JSON。`Profile` 在 Registry 中以不可变快照
交给 Runtime；配置更新只影响后续解析，已开始调用持有自己的 `Call_context`。

## 2. 数据面 LLD

### 2.1 关键接口与调用顺序

```text
AI_EMBEDDING(model, text, options) / AI_ANALYZE(model, prompt, options)
  → check rds_ai_maas
  → validate SQL arguments and AI_INVOKE
  → registry.resolve(model, capability)
  → audit.start(call_id) [when audit ON]
  → credential.resolve(profile)
  → adapter.build_request(request, profile)
  → transport.post_https(endpoint, payload, timeout)
  → adapter.parse_response()
  → audit.complete(call_id, terminal state)
  → SQL result / redacted error
```

检查顺序不可调整：`rds_ai_maas=OFF`、SQL 参数、`AI_INVOKE`、Profile/Endpoint、STARTED 审计、凭据均在
外呼前执行。Transport 返回后才可能产生 Provider 费用；任何超时、客户端断开或事务回滚不得声称远端未执行。

### 2.2 并发、内存与错误

- 不在 `THD` 锁、表锁或 Registry 写锁内执行 libcurl；网络调用只持有 Profile 快照。
- `timeout_ms` 只改变本次 Transport 总等待时间；`max_output_tokens` 只进入生成请求。二者不是实例参数。
- Adapter 的 JSON 解析、非 2xx、空 content、向量维度错误归类为“已可能出站”的失败；本地校验失败不创建 audit STARTED。
- MTR：参数/NULL/options、无权限、禁用 Profile、超时/协议 fixture、VECTOR 编解码、审计 call_id 关联。

## 3. 模型管理 LLD

### 3.1 `dbms_ai` 目标接口

```sql
CALL dbms_ai.register_model(model_name, capability, provider_model_name,
                            endpoint_url, dimension, provider_options);
CALL dbms_ai.update_model(model_name, capability, provider_model_name,
                          endpoint_url, dimension, provider_options);
CALL dbms_ai.delete_model(model_name, capability);
CALL dbms_ai.show_models();
```

写过程要求 `AI_ADMIN` 与 `rds_ai_maas=ON`；`show_models()` 仅返回脱敏元数据，可在开关 OFF 时用于 DFX。
接口不接收 API Key、Authorization、任意 headers 或原始 Provider 请求。

### 3.2 发布算法与复制

```text
AI_ADMIN → 参数/Provider/Endpoint/option 校验 → 凭据可解析性校验
  → 生成 config_version + 1 → 受控写 ai_model_config → 写 binlog
  → Registry 原子发布 ACTIVE Profile 快照 → 后续调用使用新版本
```

`delete_model()` 将 Profile 置为 `DISABLED`，不物理删除审计关联。复制 applier 可应用已验证的控制面变更；
只读节点不写审计系统表，但各节点各自写本地审计文件。MTR 覆盖重复注册、更新回退、停用、直接 DML 拒绝、
row-based 复制和并发“更新/调用”快照一致性。

## 4. 权限与实例开关 LLD

| 控制项 | 检查点 | 失败行为 |
| --- | --- | --- |
| `rds_ai_maas` | SQL 函数与 `dbms_ai` 写入口第一步 | `OFF` 时无 Profile、审计、凭据、网络或新调用审计。 |
| `AI_INVOKE` | 数据面参数校验后 | 无权限本地拒绝。 |
| `AI_ADMIN` | 每个 `dbms_ai` 写过程入口 | 拒绝模型发布；不授予系统表 DML。 |

升级时，在动态权限注册后由 mysql 系统升级路径幂等向存量 `root@'%'` 写入 `AI_INVOKE`、`AI_ADMIN`，
`WITH_GRANT_OPTION='N'`。普通 SQL 不得直写 `mysql.global_grants`；不自动授予 `SYSTEM_VARIABLES_ADMIN`。

## 5. 凭据、Endpoint 与协议 LLD

Huawei Profile 使用 `rds_api_key`：仅管控后台下发加密密文，Resolver 只在 Huawei Adapter 调用期间在
内存解密并构造 Bearer header。密文/明文不能经 `SHOW VARIABLES`、日志、binlog、SQL 错误或审计泄露。
外部 Provider 不读取该参数，而从 `provider_options.credential_id` 解析其受控加密凭据/IAM Role。

Endpoint 由 Profile 发布，Transport 接收已验证快照：HTTPS、443、无 userinfo/query/fragment，并匹配
Provider Host/路径 allowlist。请求协议由 Adapter 固化：Embedding 解析 `data[].embedding`；Chat 解析首个
非空最终 `message.content`、usage 与 request id。仅改 URL 不得绕过协议兼容性和 MTR。

## 6. 审计与 DFX LLD

`Ai_file_audit_sink::Start()` 先追加并 fsync `STARTED`，返回 `call_id`；失败立即阻止 Transport。
`Complete()` 写 `SUCCEEDED`、`FAILED` 或 `UNKNOWN`，包含用户、客户端 IP、模型、配置版本、耗时、
脱敏 usage、Provider request id 与错误分类。终态写失败不覆盖已存在的 STARTED：日志平台将其视为 UNKNOWN。

日志绝不记录 API Key、Authorization、完整 prompt/response、原始向量或 Provider 原始错误正文。DFX 使用
`call_id` 将 SQL 错误、审计行、错误日志和 Provider request id 关联；审计文件读取由日志平台权限控制，
不提供普通 SQL 查询。

## 7. 向量与 RAG LLD

Embedding Adapter 对模型 Profile 的固定维度做校验；`Ai_vector_codec` 将 `float[]` 编码为 `MYSQL_TYPE_VECTOR`。
`STORED + AI_EMBEDDING()` 仅在正文列变化时重新生成向量；开关 OFF 或本地调用失败时 DML 失败，不静默写
NULL/旧向量。RAG 由 SQL 完成 tenant/标签/业务过滤与来源 ID 保留，再把已授权片段组织为 Analyze prompt；
模型输出不能作为权限、引用或自动修复 SQL 的依据。

## 附录 A. 早期原型设计（仅作溯源，不作为当前实现目标）

## A.1 范围与原则

本设计只完成 2026-09-30 P0 的华为云 MaaS 文本能力闭环：文本向量化、文本生成、
RAG 问答、SQL 结果分析、DBA 只读诊断、模型治理和文件审计。百炼、火山、Bedrock、
异步批处理、流式、多模态、Rerank、配额和按模型授权不在本次实现范围。

模型治理以简单、易用和高质量为首要原则。客户不直接修改 `mysql` 系统表，不填写
Provider、Endpoint、向量维度、内部 ID、配置版本或白名单；P0 也不建立模型绑定、
租户绑定、状态管理或默认模型等额外客户概念。

## A.2 `dbms_ai` 原生管理包

复用 AliSQL Native Procedure 框架，在 `dbms_ai` schema 注册四个过程。四个过程均要求
全局动态权限 `AI_ADMIN`。系统表 `mysql.ai_model_config` 是内部控制表；其写入只供
升级和受控管理包，运行时仅作内部读取。普通账号即使同时具有 `AI_ADMIN` 和该表的 DML/DDL 权限，也不能直接
`INSERT`、`UPDATE`、`DELETE`、`ALTER`、`DROP` 或改索引。SQL 授权层仅放行 server bootstrap、
server upgrade 和复制 applier；`dbms_ai` 通过 `System_table_access` 完成受控写入。

```sql
CALL dbms_ai.register_model(
  model_name, capability, provider_model_name, credential_mode, credential_value);

CALL dbms_ai.update_model(
  model_name, capability, provider_model_name, credential_mode, credential_value);

CALL dbms_ai.delete_model(model_name, capability);

CALL dbms_ai.show_models();
```

### A.2.1 参数与校验

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

### A.2.2 生命周期

`register_model()` 仅允许注册不存在的逻辑模型。`update_model()` 不原地修改已发布行：它创建
递增的内部 `config_version` 并使前一活动版本仅供审计关联，新的调用原子切换到新版本。
`delete_model()` 将全部版本标记为内部 `RETIRED`，不物理删除审计关联所需的版本信息；后续
调用本地失败。

`status`、`config_version`、`Id`、Endpoint、Provider、凭据存储字段和时间戳继续保留在系统表，
但不是 `dbms_ai` 的客户参数或展示字段。`show_models()` 仅返回模型名、能力、实际模型名、
固定维度和内部版本号；不返回状态、Endpoint、API Key、Secret 引用或完整 HTTP 配置。

P0 不设置默认模型。`AI_EMBEDDING()` 和 `AI_ANALYZE()` 都必须显式传入 `model_name`。

## A.3 `AI_ANALYZE` 受控契约

函数签名为：

```sql
AI_ANALYZE(model_name, prompt [, options_json])
```

`model_name` 是已启用的逻辑生成模型；`prompt` 是调用方自然语言请求及已授权上下文。Runtime
使用不可由调用方覆盖的通用 system policy，将 `prompt` 放入 Huawei V2 Chat 的 user message。
函数只返回非空最终文本，不返回原始 Provider JSON、reasoning 或审计元数据。

`options_json` 仅支持 `max_output_tokens`（`1..32768`）和本地 `timeout_ms`（`1..60000`）。
`timeout_ms` 不发送给 MaaS；它控制 libcurl 总等待时间。`mode`、`output_format`、
`return_sources`、options 内 `model_name` 和 Provider 私有参数均在权限、审计、凭据读取和
MaaS 出站前失败。

RAG 调用方必须先在 SQL 中完成租户、访问标签和标量过滤，再把问题和可访问片段拼成 prompt；
答案旁的来源 ID 应取自检索 SQL，而不是模型输出。DBA 诊断同理：将证据与“不要自动执行”的
约束写入 prompt，模型文本不具有执行权限。

## A.4 调用审计与可观测性

`ai_invoke_audit=ON` 时，Runtime 在 MaaS 出站前将 `AI_CALL_STARTED` 追加并 `fsync` 到
JSON Lines 文件；写入失败则请求失败且不出站。调用结束后写 `AI_CALL_SUCCEEDED` 或
`AI_CALL_FAILED`。

终态写入失败时，Runtime 返回 `AUDIT_UNAVAILABLE`，在服务器错误日志中写入不含敏感信息的
告警；已有 `STARTED` 但无终态的 `call_id` 被日志采集平台识别为 `UNKNOWN`。由于终态文件
已经不可写，内核不能伪造成功持久化的 `UNKNOWN` 行。

每次追加均重新打开审计文件，因此 TaurusDB 既有日志轮转可通过 rename/create 生效；保留、
采集、磁盘容量告警和查看权限复用现有日志平台。P0 不新增审计系统表或普通 SQL 审计查询。

## A.5 凭据、网络与错误边界

开发验证可使用 `PLAINTEXT_DEV`；其密钥仅短暂构造 Authorization header，代码不会写入
错误日志、审计日志或 SQL 结果。生产只能使用 `SECRET_REF`，并通过现有 keyring reader
在内存中读取 Secret。Release 中 `register_model()` 和 `update_model()` 在发布前用同一
reader 探测引用可读且非空；探测失败不写配置、不回显 Secret。运行时仍在每次调用读取
Secret，因此发布后的 keyring 删除或轮换失败也会在出站前 fail-closed。

默认 Debug MTR 不加载 keyring：仅两个精确的 `mtr/fixture-*` Profile 在 Debug 编译中以
本地离线响应执行，且由 `dbms_ai` 管理包注册/删除。该例外不编入 Release，也不能作为
生产 `SECRET_REF` 验证证据；Release 的未知引用拒绝与目标环境的已配置 keyring 成功发布
分别验证。

Endpoint 不再由 SQL 管理员输入，运行时只接受由 capability 派生的华为标准 HTTPS Endpoint。
这同时形成 P0 的 Endpoint allowlist，避免 loopback、内网、任意端口和 DNS 重绑定类配置风险。
超时、响应大小上限、HTTP 非 2xx、限流、协议不匹配、凭据错误和响应缺失均在 Runtime 分类，
并仅向 SQL 返回脱敏错误。

云侧 VPC、Policy Route、SNAT/DNAT、EIP 和跨 Region 网络配置由 TaurusDB 云平台实施；本仓库
交付配置约束、错误处理和验证用例，不修改云侧基础设施。

## A.6 测试与验收

默认 MTR 全离线，使用 `mtr/fixture-*`，不得读取真实密钥或访问公网。覆盖原生管理包权限、
注册/更新/删除/展示、AI_ADMIN 加表 DML/DDL 仍拒绝直接控制表、复制 applier、无默认模型、
Endpoint 自动映射、维度校验、受控 Analyze JSON、RAG 来源、诊断边界、审计起始失败、终态
失败和脱敏。Release MTR 使用隔离 component keyring 先发布 fake `SECRET_REF`，移除该
引用后验证 `update_model()` 不改变 active 版本；未知 `SECRET_REF` 的发布必须在写表前失败。

真实 MaaS 验证保持显式 opt-in：使用 `scripts/db4ai_maas_smoke.sql` 验证 Embedding/Analyze，
使用 `scripts/db4ai_maas_real_embedding_rag_smoke.sql` 验证 Embedding、向量索引、STORED
生成列和 RAG。生产 `SECRET_REF`、主备/只读节点、日志采集和跨 Region 网络路径必须在
TaurusDB 目标环境执行并记录为发布验收证据。

2026-08-04 的本地 Debug 回归已离线通过 `ai_maas_embedding`、`ai_maas_analysis`、
`ai_maas_contract`、`ai_maas_governance`、`ai_maas_rag` 和 `ai_maas_model_admin`。本机
3344 验证实例尚未部署本分支的 `dbms_ai` 过程；为保护既有安装和数据目录，未覆盖该旧实例。
改用当前分支 Debug 二进制建立隔离临时实例后，`dbms_ai` 的 bge-m3/1024 解析与两阶段审计
事件均已实测；首次真实 Embedding 则收到 MaaS HTTP 401（SQL 脱敏为 `ACCESS_DENIED`），未继续
产生 Analyze 或 STORED/RAG 请求。更新为已授权的凭据后，仍须显式执行上述两个脚本并保存脱敏
成功结果；不得将本次离线回归或一次认证失败代替真实 Provider 验证。
