# AliSQL DB4AI MaaS P0：运维与可复现实例

本文档是 `AI_EMBEDDING` 和 `AI_ANALYZE` 的交付使用说明。它只描述当前
AliSQL 分支已经具备的内置函数、VECTOR 索引和离线验证；生产真实 MaaS 请求必须由
管理员显式配置受控的 keyring 引用，默认 MTR 不会访问任何云端。

## 1. 上线前配置和权限

模型配置属于 `mysql` 系统库。业务用户只使用逻辑模型名，不能在 SQL 参数中传
endpoint、API Key、provider JSON、厂商模型 ID 或 Adapter 名称。

```sql
-- AI_ADMIN 管理员使用正常 DML 发布 Profile；credential_ref 是 keyring 名称，不是明文 key。
-- 正常 DML 会写入 binlog 并复制到备机。
INSERT INTO mysql.taurusdb_ai_model_config
  (model_name, provider, capability, provider_model_name, endpoint_url,
   credential_mode, credential_ref, default_dimension, is_default, status,
   config_version)
VALUES
  ('huawei/bge-m3', 'huawei', 'TEXT_EMBEDDING', 'bge-m3',
   'https://api.modelarts-maas.com/v1/embeddings',
   'SECRET_REF', 'prod/huawei-maas/token', 1024, true, 'ACTIVE', 1),
  ('huawei/glm-5.2', 'huawei', 'TEXT_GENERATION', 'glm-5.2',
   'https://api.modelarts-maas.com/v2/chat/completions',
   'SECRET_REF', 'prod/huawei-maas/token', NULL, true, 'ACTIVE', 1);

GRANT AI_INVOKE ON *.* TO 'rag_app'@'%';
```

`AI_EMBEDDING` and `AI_ANALYZE` check `AI_INVOKE` before model resolution or
network egress. A missing profile, unsupported endpoint, missing keyring
reader or missing secret fails closed. Keys are neither SQL arguments nor MTR
fixtures. `PLAINTEXT_DEV` is not a production configuration path.

### Debug-only 明文凭据

本地 Debug 构建可由具备 `AI_ADMIN` 和表 DML 权限的管理员使用
`credential_mode='PLAINTEXT_DEV'` 和 `api_key_plaintext` 做短期联调。该路径仍不向
`AI_EMBEDDING`、`AI_ANALYZE`、`AI_MODEL_INFO` 添加任何密钥参数或输出：runtime 只按
已解析 Profile 的 config id/version 在 dispatch 前读取该字段，值仅存在于短生命周期的
内存 `Secure_string`。Release 构建拒绝 `PLAINTEXT_DEV`，生产必须使用 `SECRET_REF`。

不要把明文值写进 SQL 历史、general log、slow log、支持包、MTR fixture、文档、shell
history 或 Git。Debug 配置必须使用 Huawei 的 embedding
`/v1/embeddings` 与 V2 Chat `/v2/chat/completions` 路径；当前生成逻辑模型为
`huawei/glm-5.2`。完成联调后立即用
`UPDATE mysql.taurusdb_ai_model_config SET status='DISABLED' ...` 停用临时 Profile。

Profile 是实例级配置。拥有 `AI_INVOKE` 的有效 MySQL 账号可调用全部 `ACTIVE`
Profile；业务数据的 tenant 过滤仍由业务 SQL 负责，不能通过 AI 函数参数绕过。

Current P0 supports Huawei HTTPS JSON profiles. The runtime resolves a
profile by capability and logical model, freezes its `config_id` and
`config_version` for the invocation, and requires `bge-m3` to be 1024
dimensions. Future providers (Bailian OpenAI-compatible/DashScope, Bedrock
Converse/Invoke and Ark Chat/Responses) add Adapter/Profile support; no new
customer function is required.

### 审计日志

当全局 `ai_invoke_audit=ON`（默认）时，每个已解析 Profile 的调用会在 MaaS 出站前向
受控本地文件写入并安全落盘 `AI_CALL_STARTED`；返回后追加成功或失败终态。日志包含调用时间、
账号、客户端 IP、模型、配置版本、耗时、Token 用量和脱敏错误分类；不包含 API Key、
Authorization、完整 prompt、完整响应或原始 embedding。日志文件由 TaurusDB 日志平台管理，
P0 不提供 `AI_AUDIT_INFO()` 或普通 SQL 文件读取。

`ai_invoke_audit` 是仅 GLOBAL 的动态开关，默认 `ON`，只能由具备系统变量管理权限的管理员
设置；不支持 `SET SESSION ai_invoke_audit`。`ai_invoke_audit_log_file` 是仅 GLOBAL 的只读启动
变量，用 `--ai-invoke-audit-log-file=/path/to/file` 指定日志文件；未配置时使用
`<datadir>/ai_invoke_audit.jsonl`。修改日志路径需要重启实例。

## 2. Enterprise-manual RAG

Create a new corpus and new VECTOR INDEX whenever model revision, dimension,
normalization, distance metric or embedding-space contract changes. Do not
mix vectors from different spaces in one index.

```sql
CREATE TABLE product_manual_chunk (
  tenant_id BIGINT UNSIGNED NOT NULL,
  source_id VARCHAR(64) NOT NULL,
  chunk_id INT NOT NULL,
  product_line VARCHAR(32) NOT NULL,
  access_label ENUM('internal','support') NOT NULL,
  content TEXT NOT NULL,
  embedding VECTOR(1024) NOT NULL,
  embedding_model VARCHAR(128) NOT NULL,
  embedding_config_version BIGINT UNSIGNED NOT NULL,
  embedding_space_id VARCHAR(128) NOT NULL,
  PRIMARY KEY (tenant_id, source_id, chunk_id),
  KEY ix_tenant_product (tenant_id, product_line, access_label),
  VECTOR INDEX ix_manual_embedding (embedding)
) ENGINE=InnoDB;

INSERT INTO product_manual_chunk
  (tenant_id, source_id, chunk_id, product_line, access_label, content,
   embedding, embedding_model, embedding_config_version, embedding_space_id)
VALUES
  (42, 'manual-001', 1, 'gateway', 'support',
   'Reset an offline gateway only after checking power and network indicators.',
   AI_EMBEDDING('Reset an offline gateway only after checking power and network indicators.',
                'huawei/bge-m3', 1024),
   'huawei/bge-m3', 1, 'huawei/bge-m3/current/1024/cosine');

SET @question = 'How should support handle an offline gateway?';
SET @query_embedding = AI_EMBEDDING(@question, 'huawei/bge-m3', 1024);

SELECT source_id, chunk_id, content,
       VEC_DISTANCE_COSINE(embedding, @query_embedding) AS distance
  FROM product_manual_chunk
 WHERE tenant_id = 42
   AND product_line = 'gateway'
   AND access_label = 'support'
   AND embedding_model = 'huawei/bge-m3'
   AND embedding_config_version = 1
   AND embedding_space_id = 'huawei/bge-m3/current/1024/cosine'
 ORDER BY VEC_DISTANCE_COSINE(embedding, @query_embedding)
 LIMIT 8;
```

The tenant, business and access predicates remain normal SQL predicates;
they are not delegated to the model. The application builds the RAG input
from this result and preserves its database-generated sources:

```sql
SELECT AI_ANALYZE(
  'Answer using only the supplied chunks. State uncertainty when they are insufficient.',
  JSON_OBJECT('question', @question,
              'chunks', JSON_ARRAY(JSON_OBJECT('source_id','manual-001',
                                                'chunk_id',1,
                                                'content','...'))),
  JSON_OBJECT('mode','rag','output_format','json','return_sources',true,
              'model_name','huawei/glm-5.2','max_output_tokens',400,
              'timeout_ms',15000)) AS answer;
```

The generated answer is not an authority for provenance: return the selected
`source_id`/`chunk_id` rows alongside it. `AI_ANALYZE` never executes SQL or
changes data, configuration or DBA settings.

## 3. SQL analysis and read-only DBA diagnosis

First perform filtering, aggregation and PII removal in SQL. Pass only the
bounded result to the model.

```sql
SELECT AI_ANALYZE(
  'Summarize trends, anomalies and caveats for the following aggregated data.',
  JSON_OBJECT('period','2026-07', 'rows', JSON_ARRAY(/* approved aggregates */)),
  JSON_OBJECT('mode','analyze','output_format','json','max_output_tokens',500))
  AS report;

SELECT AI_ANALYZE(
  'Return reason, evidence, recommendation and risk. Do not propose executable repairs.',
  JSON_OBJECT('sql_digest','SELECT ...', 'explain','...', 'elapsed_ms',4210,
              'rows_examined',900000, 'lock_wait_ms',0, 'io','...'),
  JSON_OBJECT('mode','diagnose','output_format','json','max_output_tokens',500))
  AS readonly_diagnosis;
```

`options_json` rejects any field outside `model_name`, `mode`,
`output_format`, `return_sources`, `max_output_tokens` and `timeout_ms`.
Provider-private controls such as `temperature`, `thinking` and `tools` are
not customer SQL surface. The Huawei adapter returns only final `content`; a
reasoning-only response, absent final content, or `finish_reason=length` is a
failure rather than a customer result.

## 4. Verification, error boundaries and capacity

Default verification is offline:

```text
cmake --build build-debug --target mysqld ai_huawei_maas_adapter-t ai_audit-t ai_runtime-t
build-debug/runtime_output_directory/ai_huawei_maas_adapter-t
build-debug/runtime_output_directory/ai_audit-t
build-debug/runtime_output_directory/ai_runtime-t
cd build-debug/mysql-test && ./mtr --suite=rds \
  ai_maas_contract ai_maas_governance ai_maas_rag ai_maas_analysis vidx_func
```

The contract suite covers public arity, NULL-without-egress, configuration
selection, `AI_INVOKE`, absent credentials and no accidental real egress. Unit
tests cover Huawei payload conversion, stable limits, non-2xx redaction,
malformed data, response completeness and the bge-m3 dimension guard.

A real smoke invocation is opt-in: production operators provision a keyring
secret outside the repository, while local Debug operators may create a
temporary `PLAINTEXT_DEV` Profile. Configure the approved host, grant a
disposable principal `AI_INVOKE`, then run `scripts/db4ai_maas_smoke.sh`. Set
`DB4AI_MYSQL` to the client command/path and `DB4AI_DATABASE` to a simple
application schema; the optional model-name variables select logical Profiles.
Do not run it in default CI. It emits only the vector dimension and completion
length, never token, prompt, completion or embedding.

Current hard boundaries are a 1 MiB HTTP response cap and a default 30-second
request timeout (overridable only downward/upward through the stable
`timeout_ms` option subject to deployment policy). HNSW/VECTOR memory, index
build time and recall are corpus- and hardware-dependent; capacity testing
must measure the production dimension (1024), chunk count, filter selectivity,
top-k and concurrent calls. Begin with a separate corpus per embedding-space
version and observe p50/p95 provider latency, errors and token usage before
increasing concurrency.

## 5. Capability comparison and current gaps

| Dimension | AliSQL DB4AI P0 evidence | PolarDB MySQL comparison status |
|---|---|---|
| Customer API | Built-in `AI_EMBEDDING`/`AI_ANALYZE`; see `sql/item_create.cc` and `rds.ai_maas_contract` | Not asserted as equivalent to `PREDICT`; no unverified claim is made. |
| Multi-cloud evolution | Profile/version plus canonical request/response and Adapter boundary | AliSQL design advantage; provider additions need Adapter tests. |
| Vector/RAG governance | Native VECTOR and VECTOR INDEX; SQL retains tenant/scalar filters and sources | Demonstrated by the schema/query above; benchmark comparison remains future work. |
| Data egress and errors | `AI_INVOKE`, keyring `SECRET_REF`, endpoint allow-list, redacted failures | No external PolarDB validation was run in this branch. |
| Audit/operations | 两阶段、脱敏的受控审计文件；主备节点均可写入 | 日志采集、保留、告警与运维平台展示由 TaurusDB 日志平台提供。 |

当前 P0 使用实例级 Profile 和 `AI_INVOKE`，不提供账号到模型的细粒度授权。
`AI_ADMIN` 负责受控 Profile 发布与停用；Profile 驱动的 embedding-space 元数据强制、
多 Provider Adapter 和异步批量任务属于后续工作。离线 RAG/分析/诊断证据由
`rds.ai_maas_rag` 与 `rds.ai_maas_analysis` 覆盖。
