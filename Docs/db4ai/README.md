# TaurusDB MySQL 对接华为云 MaaS：移植 README

本目录记录 AliSQL 中 TaurusDB MySQL 对接华为云 MaaS 的 P0 实现、验证与移植边界。
本文是将该能力移植到 TaurusDB 代码线时的入口文档；详细设计以
[`taurusdb-maas-p0-high-level-design.md`](taurusdb-maas-p0-high-level-design.md) 和
[`alisql-maas-p0-low-level-design.md`](alisql-maas-p0-low-level-design.md) 为准。

## 1. 目标与范围

目标是在数据库内提供受控的 MaaS 调用能力：应用使用 SQL 完成文本向量化、向量检索后的
RAG 问答、SQL 结果分析和 DBA 只读诊断；数据库负责权限、模型配置、审计、错误治理和向量
数据契约，华为云 MaaS 负责模型推理。

当前 P0 已交付：

- `AI_EMBEDDING(text, model_name [, dimension])`：调用华为 MaaS `bge-m3` 文本向量模型，
  返回 1024 维 `VECTOR`；可写入向量列、HNSW 索引和 `STORED` 生成列。
- `AI_ANALYZE(task_text, input_value [, options_json])`：调用华为 MaaS V2 Chat 文本生成模型，
  支持 `analyze`、`rag`、`diagnose`、`summarize`、`classify`、`extract` 等受控模式。
- `dbms_ai` 原生模型管理包：`register_model`、`update_model`、`delete_model`、`show_models`。
- `AI_INVOKE` 调用权限、`AI_ADMIN` 模型管理权限，以及出站前 `STARTED`、结束后终态的两阶段
  脱敏 JSON Lines 文件审计。
- 离线可重复 MTR，以及显式授权、可能产生费用的真实华为 MaaS SQL smoke 用例。

当前未交付：百炼、火山方舟、Bedrock 等 Provider 的生产 Adapter 与真实联调；异步/批量
Embedding、流式输出、多模态、按模型或预算的细粒度授权，以及数据库内审计文件查询接口。

## 2. 移植时必须保留的设计边界

1. 业务 SQL 只传逻辑模型名，不能传 Endpoint、Provider 原始 JSON 或 API Key。
2. 每次调用必须先校验 `AI_INVOKE`，再解析 Profile 和发起网络出站。
3. 模型配置表 `mysql.taurusdb_ai_model_config` 是内部控制表；只能由 `dbms_ai` 管理过程写入，
   即使持有表 DML/DDL 权限也必须拒绝直接修改。
4. P0 没有默认模型；`AI_EMBEDDING()` 和 `AI_ANALYZE()` 均须显式选择模型。
5. 审计开启时，出站前必须先安全写入 `AI_CALL_STARTED`；无法写入则 fail closed，不能请求 MaaS。
   完成事件写入失败时保留 `STARTED`，按 `UNKNOWN` 处置。
6. `PLAINTEXT_DEV` 仅用于 Debug/开发验证；Release/生产只允许 `SECRET_REF` 经现有 keyring 读取。
   API Key、Authorization、完整请求/响应和原始向量不得进入 SQL 结果、错误、审计或 Git。
7. RAG 的 tenant、业务标签、权限和标量过滤必须由数据库 SQL 完成；模型只消费已授权的上下文。

## 3. 代码清单

| 位置 | 职责 |
|---|---|
| `sql/ai/ai_types.{h,cc}` | Provider 无关的请求、响应、错误、配置和审计类型。 |
| `sql/ai/ai_model_registry.{h,cc}` | Profile 解析、能力/维度/Endpoint/凭据约束。 |
| `sql/ai/ai_model_admin.{h,cc}` | `dbms_ai` 四个原生管理过程及受控系统表写入。 |
| `sql/ai/ai_runtime.{h,cc}`、`ai_runtime_server.cc` | SQL 调用编排、权限、模型解析、系统变量与审计接入。 |
| `sql/ai/item_ai_func.{h,cc}` | `AI_EMBEDDING`、`AI_ANALYZE`、`AI_MODEL_INFO` 内置函数。 |
| `sql/ai/ai_huawei_maas_adapter.{h,cc}` | 华为 MaaS Embedding 与 V2 Chat 请求/响应适配。 |
| `sql/ai/ai_http_transport.{h,cc}` | libcurl HTTPS、Endpoint allowlist、超时、响应大小与错误脱敏。 |
| `sql/ai/ai_file_audit.{h,cc}`、`ai_audit_service.cc` | 两阶段、追加式、本地审计日志文件。 |
| `sql/ai/ai_vector_codec.{h,cc}` | MaaS float 向量到 AliSQL `VECTOR` 的校验与编码。 |
| `sql/CMakeLists.txt` | AI Runtime、libcurl 依赖及服务端目标编译。 |
| `sql/auth/sql_authorization.cc` | `AI_INVOKE`、`AI_ADMIN` 的动态权限和控制表保护。 |
| `sql/package/package_cache.cc`、`sql/sql_parse.cc` | `dbms_ai` 原生管理包注册与调用路由。 |

移植时以 `sql/ai/` 作为一个完整模块迁入，再接入目标分支的 CMake、原生过程框架、动态权限框架、
系统表升级框架、keyring 服务和现有审计日志平台；不要只拷贝 SQL 函数或单个 Adapter。

## 4. SQL 与运维接口

```sql
-- 仅 AI_ADMIN 可执行；直接修改 mysql.taurusdb_ai_model_config 会被拒绝。
CALL dbms_ai.register_model(
  'huawei/bge-m3', 'TEXT_EMBEDDING', 'bge-m3',
  'SECRET_REF', 'maas/bge-m3/key');
CALL dbms_ai.register_model(
  'huawei/glm-5.2', 'TEXT_GENERATION', 'glm-5.2',
  'SECRET_REF', 'maas/glm-5.2/key');
CALL dbms_ai.show_models();

GRANT AI_INVOKE ON *.* TO 'app'@'%';

SELECT AI_EMBEDDING('数据库支持 RAG 检索', 'huawei/bge-m3', 1024);
SELECT AI_ANALYZE(
  '根据证据返回原因、证据、风险和建议；不要执行自动修复 SQL。',
  JSON_OBJECT('sql_digest', 'SELECT * FROM orders WHERE tenant_id = ?',
              'elapsed_ms', 4210, 'rows_examined', 900000),
  JSON_OBJECT('model_name', 'huawei/glm-5.2', 'mode', 'diagnose',
              'timeout_ms', 60000));
```

相关全局变量：

- `ai_invoke_audit`：仅 GLOBAL、动态、默认 `ON`。关闭需管理员系统变量权限；不支持
  `SET SESSION`。
- `ai_invoke_audit_log_file`：仅 GLOBAL、启动时指定；未配置时使用
  `<datadir>/ai_invoke_audit.jsonl`。修改路径需重启。

## 5. 测试清单

### 默认离线 MTR

以下用例不会读取真实密钥或访问公网，适合作为移植后的首轮回归：

```bash
cd build-debug/mysql-test
./mtr --suite=rds \
  ai_maas_contract ai_maas_embedding ai_maas_analysis ai_maas_governance \
  ai_maas_rag ai_maas_model_admin ai_maas_model_admin_rpl
```

| 用例 | 覆盖内容 |
|---|---|
| `ai_maas_contract` | SQL 参数、NULL、显式模型选择、拒绝 Provider 私有参数与本地失败无出站。 |
| `ai_maas_embedding` | 1024 维、模型/维度校验、VECTOR 结果与离线 fixture。 |
| `ai_maas_analysis` | Analyze 选项、模式、最终内容、reasoning-only/截断错误。 |
| `ai_maas_governance` | `AI_INVOKE`、审计开关、审计脱敏与出站前失败。 |
| `ai_maas_rag` | 向量写入、RAG 过滤、来源契约、`STORED + AI_EMBEDDING()`。 |
| `ai_maas_model_admin` | `dbms_ai` 管理、`AI_ADMIN` 与控制表直接 DML/DDL 拒绝。 |
| `ai_maas_model_admin_rpl` | 受控模型变更的复制 applier 行为。 |

对应文件位于 `mysql-test/suite/rds/t/ai_maas_*.test` 与
`mysql-test/suite/rds/r/ai_maas_*.result`。

### 真实华为 MaaS 验证

下列脚本均已纳管，但不会由 MTR/CI 自动执行。运行前需在专用验证库中配置模型、凭据和
`AI_INVOKE`；它们会产生外部调用及可能的费用。

| 文件 | 验证内容 |
|---|---|
| `scripts/db4ai_maas_smoke.sql` | `bge-m3` Embedding 的维度，以及 `glm-5.2` Analyze 的非空结果。 |
| `scripts/db4ai_maas_real_embedding_rag_smoke.sql` | `bge-m3` 直接向量化、VECTOR 索引、`STORED` 自动向量化、文本更新与 RAG 召回。 |
| `scripts/db4ai_maas_generation_model_comparison.sql` | 六个已配置华为文本生成模型的慢 SQL 诊断与订单经营分析两个用例。 |

在 mysql 客户端选定专用 schema 后执行：

```sql
source /absolute/path/to/scripts/db4ai_maas_smoke.sql;
source /absolute/path/to/scripts/db4ai_maas_real_embedding_rag_smoke.sql;
source /absolute/path/to/scripts/db4ai_maas_generation_model_comparison.sql;
```

生成模型对比覆盖：`huawei/glm-5.2`、`huawei/kimi-k2.6`、
`huawei/deepseek-v4-pro`、`huawei/deepseek-v4-flash`、
`huawei/openpangu-2.0-pro`、`huawei/openpangu-2.0-flash`。单次输出受模型准入、配额、
网络与时延影响，只能用于当次环境的功能和质量人工比较，不构成可用性或性能承诺。

## 6. 推荐移植顺序

1. 迁入 `sql/ai`、CMake 与 libcurl 依赖，先构建 Debug `mysqld`。
2. 接入动态权限、`dbms_ai` 包、控制表升级和目标分支的复制/只读节点策略。
3. 接入 keyring/Secret 引用和文件审计 Sink；确认日志采集、轮转、磁盘告警和访问控制。
4. 跑第 5 节全部离线 MTR，修复目标分支框架差异后再继续。
5. 在隔离验证实例配置可撤销的开发凭据，执行真实 SQL smoke；完成后删除 Profile，并在 MaaS
   侧轮换或吊销测试 Key。
6. 最后验证主节点、只读节点、切换和审计文件不可写四类场景。

## 7. 相关文档

- [P0 高层设计](taurusdb-maas-p0-high-level-design.md)
- [P0 低层设计](alisql-maas-p0-low-level-design.md)
- [验收标准](taurusdb-maas-p0-acceptance-criteria.md)
- [运维与 SQL 示例](alisql-maas-p0-operations-and-examples.md)
- [验证状态](alisql-maas-p0-validation-status.md)
- [文本生成模型双用例对比设计](../superpowers/specs/2026-08-04-maas-generation-model-comparison-design.md)
