# AliSQL DB4AI MaaS P0 Low-Level Design

## Status and scope

This document defines the AliSQL implementation of the DB4AI P0 interface for
Huawei Cloud MaaS. It replaces neither the existing vector/HNSW implementation
nor the MaaS Component/UDF prototype in the sibling `mysql-server-maas`
repository. The prototype is validation input only; the product implementation
is built-in SQL functions plus an in-server AI Runtime and Provider Adapter.

P0 implements synchronous text embedding and text generation with Huawei MaaS.
It provides extension points for Bailian OpenAI-compatible APIs, DashScope,
AWS Bedrock and Volcano Ark, but does not invoke those providers in P0.

## Customer contract

The only AI capability functions are:

```sql
AI_EMBEDDING(text [, model_name [, dimension]])
AI_ANALYZE(task_text, input_value [, options_json])
```

`model_name` is a tenant-visible logical name. It is never a provider model
identifier, endpoint, adapter name or credential. `AI_EMBEDDING(NULL, ...)`
returns `NULL`. Huawei `huawei/bge-m3` is a fixed 1024-dimension Profile in
P0: specifying any other dimension fails locally before an HTTP request.

`AI_ANALYZE` returns a utf8mb4 final-content string by default. Its JSON
option accepts only these stable keys:

```json
{
  "model_name": "huawei/glm-5.2",
  "mode": "analyze",
  "output_format": "text",
  "return_sources": false,
  "max_output_tokens": 2048,
  "timeout_ms": 30000
}
```

The valid modes are `analyze`, `rag`, `diagnose`, `summarize`, `classify` and
`extract`. `return_sources=true` requires `output_format='json'`. The parser
rejects provider fields such as `temperature`, `thinking`, `tools`, request
messages, endpoint URLs and all credential fields. A successful HTTP response
without non-empty final content, with only reasoning, or truncated with
`finish_reason=length` is `AI_ANALYZE_INCOMPLETE_OUTPUT`; reasoning is never a
customer result.

`AI_MODEL_INFO([model_name])` is a read-only metadata function. It returns
sanitized JSON containing logical model name, provider, capability, resolved
provider model, revision state, dimensions, embedding space, lifecycle state,
config version and the latest health classification. It never returns endpoint
details, credential references or secrets.

## Runtime architecture

```text
Item_func_ai_embedding / Item_func_ai_analyze
  -> Ai_runtime
      -> Model_registry and tenant resolver
      -> Credential_resolver
      -> Provider_adapter selected by provider + capability + model/revision
         + endpoint type
      -> Http_transport
      -> Canonical_response validation
      -> Vector_codec or content renderer
      -> Independent audit and metering sink
```

`Ai_canonical_request` contains the requested capability, task, input,
whitelisted generation options, resolved model configuration, tenant and audit
context. `Ai_canonical_response` contains only final content, embeddings,
completion state, finish reason, usage, database request ID and provider request
ID. Provider-specific JSON exists only within an Adapter.

P0 registers two adapters:

| Adapter | Profile match | Request | Required response |
|---|---|---|---|
| `Huawei_maas_embedding_adapter` | `HUAWEI_MAAS`, `TEXT_EMBEDDING`, MaaS embedding endpoint | model, input, encoding format | `data[].embedding`, response model and usage |
| `Huawei_maas_v2_chat_adapter` | `HUAWEI_MAAS`, `TEXT_GENERATION`, V2 chat endpoint | canonical task/input and controlled generation defaults | non-empty final message content, finish reason, usage |

The Adapter registry key intentionally includes capability, provider model and
endpoint type. Adding a Bailian OpenAI-compatible, Bedrock Converse/Invoke or
Ark Chat/Responses adapter therefore does not change either SQL function.

`Http_transport` uses server libcurl support, requires HTTPS and an allowlisted
endpoint, enforces the resolved timeout and a 1 MiB response limit, and records
only redacted provider status and error class. It does not log request bodies,
Authorization headers or raw provider responses.

## Model, credential and tenant data

The bootstrap/upgrade scripts create these protected InnoDB tables in `mysql`:

```text
alisql_ai_model_config
  id, tenant_id, model_name, provider, capability,
  provider_model_name, model_revision, endpoint_type, endpoint_url,
  credential_mode, credential_ref, api_key_plaintext,
  default_dimension, allowed_dimensions, embedding_space_id,
  distance_metric, generation_defaults, generation_limits,
  status, config_version, is_builtin, created_at, updated_at

alisql_ai_tenant_binding
  user, host, tenant_id, status, created_at, updated_at

alisql_ai_call_audit
  request_id, provider_request_id, db_user, tenant_id, capability,
  model_name, resolved_config_id, resolved_config_version,
  provider_model_name, model_revision, protocol_family,
  prompt_tokens, completion_tokens, reasoning_tokens, cached_tokens,
  total_tokens, latency_ms, http_status, error_code, state, created_at
```

Model resolution selects an `ACTIVE` Profile for the authenticated account's
tenant, then falls back to tenant `0` only when the Profile explicitly permits
that fallback. The result is a fixed `(id, config_version)` for the call.
`status='ACTIVE'` means configuration lifecycle only; it is distinct from model
visibility, account entitlement, successful inference and complete response.

Production credentials use `credential_ref` and the existing server keyring
reader service. The secret is held only in request memory and zeroed after use.
`PLAINTEXT_DEV` is accepted only in explicit development/smoke configuration,
must not be enabled for production, and has no SQL function parameter. It is
not visible through metadata or audit APIs.

## Vector and RAG invariants

AliSQL already stores `VECTOR(N)` as binary float data and exposes
`VEC_FROMTEXT`, `VEC_DISTANCE_EUCLIDEAN`, `VEC_DISTANCE_COSINE`, `VECTOR_DIM`
and HNSW vector indexes. `Vector_codec` writes the same validated little-endian
float representation and declares `MYSQL_TYPE_VECTOR` through
`set_data_type_vector()`. Thus this is valid P0 SQL:

```sql
INSERT INTO ai_doc_chunks (tenant_id, source_id, chunk_id, content, embedding)
VALUES (42, 'manual-001', 1, '...', AI_EMBEDDING('...', 'huawei/bge-m3', 1024));
```

Each persisted vector also stores config ID/version, provider model, dimension,
distance metric, corpus version, index version and `embedding_space_id`.
`embedding_space_id` binds provider, model/revision, config version, dimension,
normalization/codec and metric. A vector cannot be written to or searched with
an index using another space. Embedding changes require a new Profile version,
new vectors and a new HNSW index; chat aliases may change independently.

The RAG tutorial uses an application schema with `tenant_id`, `source_id`,
`chunk_id`, business scalar filters, provenance and the fields above. Retrieval
always applies the tenant/security/scalar predicates in ordinary SQL before
assembling chunks. `AI_ANALYZE(mode='rag')` receives those already-authorized
chunks and returns database-supplied `source_id`/`chunk_id` values only. It
never executes SQL, alters DBA configuration or invents sources.

## Permissions, audit and errors

P0 registers three dynamic privileges:

| Privilege | Grants |
|---|---|
| `AI_INVOKE` | Execute `AI_EMBEDDING` and `AI_ANALYZE` for authorized Profiles |
| `AI_ADMIN` | Manage model Profiles, tenant bindings and development credential configuration |
| `AI_AUDIT_VIEWER` | Read sanitized model-health, audit and usage surfaces |

An independent audit transaction writes `CREATED` before egress; inability to
record it fails the invocation closed. It is updated to `SUCCEEDED`, `FAILED`
or `UNKNOWN` independently of the caller transaction, because a later SQL
rollback cannot undo a cloud request or its charge.

The public error categories are configuration, database permission, credential
or account entitlement, regional restriction, model unavailable, protocol
mismatch, timeout, rate limit, response too large, incomplete output and
embedding dimension/space mismatch. Error text is deterministic and redacted.

## Source layout

| Path | Responsibility |
|---|---|
| `sql/ai/ai_types.h`, `sql/ai/ai_types.cc` | canonical request/response, enums and redacted error model |
| `sql/ai/ai_model_registry.h`, `sql/ai/ai_model_registry.cc` | tenant/profile resolution and immutable config binding |
| `sql/ai/ai_runtime.h`, `sql/ai/ai_runtime.cc` | invocation orchestration and policy enforcement |
| `sql/ai/ai_provider_adapter.h` | provider-neutral adapter interface and registry |
| `sql/ai/ai_huawei_maas_adapter.cc` | MaaS V2 chat and embedding conversion |
| `sql/ai/ai_http_transport.cc` | libcurl transport, limits and redaction |
| `sql/ai/ai_vector_codec.cc` | embedding float-array validation and VECTOR encoding |
| `sql/ai/ai_audit.cc` | independent audit/metering writes |
| `sql/ai/item_ai_func.h`, `sql/ai/item_ai_func.cc` | the three native SQL Item implementations |
| `sql/item_create.cc` | native function registration |
| `sql/CMakeLists.txt` | server compilation and libcurl linkage |
| `sql/auth/dynamic_privileges_impl.cc` | dynamic privilege registration |
| `scripts/mysql_system_tables.sql` and upgrade script | protected system table lifecycle |
| `share/messages_to_clients.txt` | DB4AI error codes and messages |

## Tests and documentation

Default tests are mock-only. `mysql-test/suite/rds/t/ai_maas_*.test` covers
function arity, NULL, charset, option rejection, model resolution, permissions,
credential fail-closed behavior, Huawei request/response fixtures, usage,
timeout, non-2xx redaction, response size, no-final-content, length finish,
dimension mismatch and embedding-space mismatch. Vector tests extend the
existing `vidx_*` suite with an `AI_EMBEDDING` insertion and cosine-HNSW query.

The repository will also contain repeatable RAG, SQL-result analysis and
read-only DBA diagnosis examples, an opt-in MaaS smoke script that reads a
repository-external credential reference, known limits, performance/capacity
boundaries and a capability comparison with PolarDB MySQL. The smoke script is
not part of MTR and never emits a secret, prompt, response body or embedding.
