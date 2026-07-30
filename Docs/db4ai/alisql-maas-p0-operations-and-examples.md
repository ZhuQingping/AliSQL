# AliSQL DB4AI MaaS P0：运维与可复现实例

本文档是 `AI_EMBEDDING` 和 `AI_ANALYZE` 的交付使用说明。它只描述当前
AliSQL 分支已经具备的内置函数、VECTOR 索引和离线验证；生产真实 MaaS 请求必须由
管理员显式配置受控的 keyring 引用，默认 MTR 不会访问任何云端。

## 1. 上线前配置和权限

模型配置属于 `mysql` 系统库。业务用户只使用逻辑模型名，不能在 SQL 参数中传
endpoint、API Key、provider JSON、厂商模型 ID 或 Adapter 名称。

```sql
-- 由 AI_ADMIN 管理员执行；credential_ref 是已有 keyring 中的名称，不是明文 key。
INSERT INTO mysql.alisql_ai_model_config
  (config_version, model_name, capability, provider, provider_model_name,
   model_revision, endpoint_type, endpoint, dimension,
   embedding_space_id, distance_metric, credential_kind, credential_ref, active)
VALUES
  (1, 'huawei/bge-m3', 'TEXT_EMBEDDING', 'huawei', 'bge-m3',
   'current', 'HTTPS_JSON', 'https://<approved-maas-host>/v1/embeddings', 1024,
   'huawei/bge-m3/current/1024/cosine', 'COSINE',
   'SECRET_REF', 'prod/huawei-maas/token', TRUE),
  (1, 'huawei/glm-5.2', 'TEXT_GENERATION', 'huawei', 'glm-5.2',
   'current', 'HTTPS_JSON', 'https://<approved-maas-host>/v2/chat/completions', NULL,
   '', 'COSINE', 'SECRET_REF', 'prod/huawei-maas/token', TRUE);

INSERT INTO mysql.alisql_ai_tenant_binding
  (tenant_id, model_name, capability, config_id, active)
SELECT 42, model_name, capability, config_id, TRUE
  FROM mysql.alisql_ai_model_config
 WHERE config_version = 1;

-- Authentication identity is resolved by the server; applications cannot
-- supply or override tenant_id in either AI function.
INSERT INTO mysql.alisql_ai_tenant_account
  (account_user, account_host, tenant_id, active)
VALUES ('rag_app', '%', 42, TRUE);

GRANT AI_INVOKE ON *.* TO 'rag_app'@'%';
```

`AI_EMBEDDING` and `AI_ANALYZE` check `AI_INVOKE` before model resolution or
network egress. A missing profile, unsupported endpoint, missing keyring
reader or missing secret fails closed. Keys are neither SQL arguments nor MTR
fixtures. `PLAINTEXT_DEV` is not a production configuration path.

### Debug-only 明文凭据

本地 Debug 构建可在受控的 `mysql.alisql_ai_model_config` 行中使用
`credential_kind='PLAINTEXT_DEV'` 和 `api_key_plaintext` 做短期联调。该路径仍不向
`AI_EMBEDDING`、`AI_ANALYZE`、`AI_MODEL_INFO` 添加任何密钥参数或输出：runtime 只按
已解析 Profile 的 config id/version 在 dispatch 前读取该字段，值仅存在于短生命周期的
内存 `Secure_string`。Release 构建拒绝 `PLAINTEXT_DEV`，生产必须使用 `SECRET_REF`。

不要把明文值写进 SQL 历史、general log、slow log、支持包、MTR fixture、文档、shell
history 或 Git。Debug 配置必须使用 Huawei 的 embedding
`/v1/embeddings` 与 V2 Chat `/v2/chat/completions` 路径；当前生成逻辑模型为
`huawei/glm-5.2`。完成联调后立即删除临时 Profile、tenant binding 和账号映射。

The resolver uses the authenticated MySQL `user@host` identity to look up
`alisql_ai_tenant_account`, then resolves the matching tenant Profile. An
account with no mapping can use only a deliberately configured tenant `0`
global fallback. It cannot select a tenant through session variables or SQL
function arguments.

Current P0 supports Huawei HTTPS JSON profiles. The runtime resolves a
profile by capability and logical model, freezes its `config_id` and
`config_version` for the invocation, and requires `bge-m3` to be 1024
dimensions. Future providers (Bailian OpenAI-compatible/DashScope, Bedrock
Converse/Invoke and Ark Chat/Responses) add Adapter/Profile support; no new
customer function is required.

### 审计查询

每次已解析 Profile 的调用会在读取凭据和 HTTP egress 之前创建一条
`mysql.alisql_ai_call_audit` 记录，并在独立事务中更新为 `SUCCEEDED` 或 `FAILED`。
审计数据不包含 API key、credential ref、endpoint、prompt、task、原始响应或完整
embedding。`AI_AUDIT_VIEWER` 只能读取自身 tenant；`AI_ADMIN` 可读取跨 tenant 的相同
脱敏投影。

```sql
GRANT AI_AUDIT_VIEWER ON *.* TO 'rag_app'@'%';
SELECT AI_AUDIT_INFO(20);
```

`AI_AUDIT_INFO([limit])` 的 `limit` 为 1–100（默认 100）。每个 JSON 项只包含
call/config/version/tenant、capability、status、有限错误码、provider request id、token
usage、created/completed time、latency 和 HTTP status。它不是系统表 DML 权限；业务账号
不应被授予对 `mysql.alisql_ai_call_audit` 的直接写权限。

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
cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_contract ai_maas_governance
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
| Audit/operations | Independent persistent lifecycle audit plus tenant-scoped `AI_AUDIT_INFO` metadata | Persistent retry/retention policy and operator dashboards remain future work. |

The remaining implementation work is intentionally visible: authenticated
account-to-tenant resolution is implemented, with tenant `0` retained only as
an explicit global fallback. AI_ADMIN Profile management surfaces, configured
embedding-space metadata enforcement and full offline RAG/analysis/diagnosis
evidence remain before a production-complete P0 declaration.
