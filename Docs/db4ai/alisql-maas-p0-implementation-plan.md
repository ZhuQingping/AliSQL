# AliSQL DB4AI MaaS P0 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver AliSQL built-in `AI_EMBEDDING` and `AI_ANALYZE` functions backed by a secure, mock-tested Huawei MaaS runtime, with governed vector/HNSW RAG and auditable analysis and DBA-diagnosis examples.

**Architecture:** New `sql/ai` code owns provider-neutral canonical request and response types, model resolution, credentials, transport, adapters, vector encoding and audit. Native Item functions only validate SQL arguments and render canonical results; existing VECTOR/HNSW code remains the sole vector/index implementation.

**Tech Stack:** C++17, AliSQL native Item functions, MySQL JSON DOM, server libcurl, server keyring reader, InnoDB system tables, dynamic privileges, GUnit and MTR.

## Global Constraints

- Customer SQL never accepts an API key, endpoint, provider request JSON, provider model ID or adapter name.
- P0 customer functions are exactly `AI_EMBEDDING(text [, model_name [, dimension]])` and `AI_ANALYZE(task_text, input_value [, options_json])`.
- Default tests are deterministic and offline; live Huawei MaaS verification is explicit opt-in only.
- `huawei/bge-m3` is fixed at 1024 dimensions and any other requested dimension fails before HTTP dispatch.
- `AI_ANALYZE` returns final content only; HTTP 2xx without final content, reasoning-only output and `finish_reason=length` are errors.
- Provider routing uses provider, capability, provider model/revision and endpoint type, never provider alone.
- Secrets, authorization headers, raw prompts, raw responses and complete embeddings never enter SQL parameters, logs, audit rows or test output.
- RAG retrieval always retains tenant/security/scalar predicates and database-derived source references; AI functions never execute SQL or DBA repairs.
- A persisted embedding binds config ID/version, embedding space, model/revision, dimension and distance metric; different spaces do not share an HNSW index.

---

## File structure

| File | Responsibility |
|---|---|
| `sql/ai/ai_types.h/.cc` | Canonical request/response, error classification and usage values. |
| `sql/ai/ai_model_registry.h/.cc` | Protected model/tenant table access and immutable resolved configuration. |
| `sql/ai/ai_runtime.h/.cc` | Permission, option policy, dispatch and response completion policy. |
| `sql/ai/ai_provider_adapter.h` | Adapter and transport test seam. |
| `sql/ai/ai_huawei_maas_adapter.cc` | Huawei embedding and V2 chat conversion. |
| `sql/ai/ai_http_transport.cc` | HTTPS/libcurl limits, headers and redacted failure handling. |
| `sql/ai/ai_vector_codec.cc` | JSON float arrays to native VECTOR binary format. |
| `sql/ai/ai_audit.cc` | Independent call state and usage persistence. |
| `sql/ai/item_ai_func.h/.cc` | `AI_EMBEDDING`, `AI_ANALYZE`, `AI_MODEL_INFO` native Item functions. |
| `scripts/mysql_system_tables*.sql` | AI system table create and upgrade definitions. |
| `mysql-test/suite/rds/t/ai_maas_*.test` | Offline MTR function, security, vector and RAG evidence. |
| `unittest/gunit/ai_*-t.cc` | Pure C++ parser, adapter and vector codec tests. |

### Task 1: Canonical runtime contracts and offline test seam

**Files:**
- Create: `sql/ai/ai_types.h`, `sql/ai/ai_types.cc`, `sql/ai/ai_provider_adapter.h`, `unittest/gunit/ai_types-t.cc`
- Modify: `sql/CMakeLists.txt`, `unittest/gunit/CMakeLists.txt`

**Interfaces:**
- Produces: `enum class Ai_capability`, `enum class Ai_error`, `struct Ai_usage`, `struct Ai_resolved_model`, `struct Ai_canonical_request`, `struct Ai_canonical_response`, and `class Ai_provider_adapter`.
- Consumed by: model registry, Huawei adapter, runtime, audit and native functions.

- [ ] **Step 1: Write failing canonical-contract GUnit cases.**

```cpp
TEST(AiTypesTest, RejectsReasoningOnlyCompletion) {
  Ai_canonical_response response{};
  response.reasoning_present = true;
  EXPECT_EQ(Ai_error::k_incomplete_output, response.ValidateChatCompletion());
}

TEST(AiTypesTest, AcceptsNonEmptyFinalCompletion) {
  Ai_canonical_response response{};
  response.final_content = "answer";
  response.finish_reason = "stop";
  EXPECT_EQ(Ai_error::k_ok, response.ValidateChatCompletion());
}
```

- [ ] **Step 2: Run the new target and confirm it fails because the contract is absent.**

Run: `ninja -C build-debug ai_types-t`

Expected: compilation failure naming `Ai_canonical_response`.

- [ ] **Step 3: Implement the provider-neutral contract.**

```cpp
enum class Ai_capability { k_text_embedding, k_text_generation };
enum class Ai_error { k_ok, k_invalid_options, k_incomplete_output,
                      k_dimension_mismatch, k_timeout, k_provider_error };

class Ai_provider_adapter {
 public:
  virtual ~Ai_provider_adapter() = default;
  virtual Ai_error Execute(const Ai_canonical_request &request,
                           Ai_canonical_response *response) = 0;
};
```

`ValidateChatCompletion()` must reject an empty final content, a length finish,
or any response not marked complete. Usage fields must default to zero and IDs
must be optional, never secret-bearing values.

- [ ] **Step 4: Build and run the GUnit target.**

Run: `ninja -C build-debug ai_types-t && build-debug/runtime_output_directory/ai_types-t`

Expected: exit code 0 and both completion-policy tests pass.

- [ ] **Step 5: Commit the isolated runtime contract.**

```bash
git add sql/ai/ai_types.* sql/ai/ai_provider_adapter.h sql/CMakeLists.txt \
  unittest/gunit/ai_types-t.cc unittest/gunit/CMakeLists.txt
git commit -m "feat: add DB4AI canonical runtime contracts"
```

### Task 2: Protected model, tenant and credential configuration

**Files:**
- Create: `sql/ai/ai_model_registry.h`, `sql/ai/ai_model_registry.cc`, `unittest/gunit/ai_model_registry-t.cc`
- Modify: `scripts/mysql_system_tables.sql`, `scripts/mysql_system_tables_fix.sql`, `sql/auth/dynamic_privileges_impl.cc`, `sql/CMakeLists.txt`, `unittest/gunit/CMakeLists.txt`

**Interfaces:**
- Consumes: `Ai_capability`, `Ai_error`, `Ai_resolved_model`.
- Produces: `Ai_model_registry::Resolve(THD *, std::string_view, Ai_capability, Ai_resolved_model *)` and `Ai_credential_resolver::ReadSecret(const Ai_resolved_model &, Secure_string *)`.

- [ ] **Step 1: Add failing registry fixture tests.**

```cpp
TEST(AiModelRegistryTest, ResolvesActiveTenantProfileAndFreezesVersion) {
  auto profile = MakeProfile(42, "huawei/bge-m3", "TEXT_EMBEDDING", 7);
  EXPECT_EQ(Ai_error::k_ok, registry.ResolveForTest(42, profile.model_name,
                                                    Ai_capability::k_text_embedding, &out));
  EXPECT_EQ(7U, out.config_version);
}

TEST(AiModelRegistryTest, RejectsBgeM3DimensionOtherThan1024) {
  auto profile = MakeBgeM3Profile();
  EXPECT_EQ(Ai_error::k_dimension_mismatch,
            registry.ValidateDimension(profile, 512));
}
```

- [ ] **Step 2: Run the registry target and confirm the new API is missing.**

Run: `ninja -C build-debug ai_model_registry-t`

Expected: compilation failure naming `Ai_model_registry`.

- [ ] **Step 3: Add protected system tables and dynamic privileges.**

Create `mysql.alisql_ai_model_config`, `mysql.alisql_ai_tenant_binding` and
`mysql.alisql_ai_call_audit` using InnoDB and the `mysql` tablespace. Register
`AI_INVOKE`, `AI_ADMIN` and `AI_AUDIT_VIEWER`. The model table contains only
credential references in production; `api_key_plaintext` is nullable and is
accepted only for `PLAINTEXT_DEV` on development instances.

- [ ] **Step 4: Implement model resolution and keyring retrieval.**

`Resolve()` must select an active tenant row before an explicitly allowed tenant
`0` fallback, match the requested capability, retain config ID/version and
validate endpoint type and dimensions. `SECRET_REF` reads the existing keyring
reader service; missing keyring, missing secret, disabled profile and an
unmapped capability return fail-closed errors.

- [ ] **Step 5: Run unit and bootstrap checks.**

Run: `ninja -C build-debug ai_model_registry-t && build-debug/runtime_output_directory/ai_model_registry-t`

Expected: exit code 0; test results cover version freezing, inactive profile,
tenant isolation, keyring missing and the fixed 1024-dimensional profile.

- [ ] **Step 6: Commit configuration and authorization support.**

```bash
git add sql/ai/ai_model_registry.* scripts/mysql_system_tables*.sql \
  sql/auth/dynamic_privileges_impl.cc sql/CMakeLists.txt \
  unittest/gunit/ai_model_registry-t.cc unittest/gunit/CMakeLists.txt
git commit -m "feat: add DB4AI model configuration and privileges"
```

### Task 3: HTTP transport and Huawei MaaS Adapter

**Files:**
- Create: `sql/ai/ai_http_transport.h`, `sql/ai/ai_http_transport.cc`, `sql/ai/ai_huawei_maas_adapter.h`, `sql/ai/ai_huawei_maas_adapter.cc`, `unittest/gunit/ai_huawei_maas_adapter-t.cc`
- Modify: `sql/CMakeLists.txt`, `unittest/gunit/CMakeLists.txt`

**Interfaces:**
- Consumes: `Ai_provider_adapter`, `Ai_canonical_request`, `Ai_canonical_response`, `Ai_resolved_model`.
- Produces: `Ai_http_transport::PostJson(...)`, `Huawei_maas_adapter::Execute(...)`.

- [ ] **Step 1: Write deterministic fake-transport tests.**

```cpp
TEST(HuaweiMaaSTest, EmbeddingRequestUsesResolvedProviderModel) {
  auto response = ExecuteFixture("embedding_ok.json", EmbeddingRequest("text"));
  EXPECT_EQ(1024U, response.embeddings.front().size());
}

TEST(HuaweiMaaSTest, ChatReasoningOnlyIsIncompleteOutput) {
  auto response = ExecuteFixture("chat_reasoning_only.json", ChatRequest());
  EXPECT_EQ(Ai_error::k_incomplete_output, response.error);
}
```

- [ ] **Step 2: Run the adapter target and confirm it fails before implementation.**

Run: `ninja -C build-debug ai_huawei_maas_adapter-t`

Expected: compilation failure naming `Huawei_maas_adapter`.

- [ ] **Step 3: Implement endpoint safety and libcurl transport.**

Require HTTPS, host/port allowlist validation and a 1 MiB capped response
collector. Configure connect and total timeouts from the resolved policy. Build
Bearer authorization only in memory. Convert non-2xx, malformed JSON, timeout
and response-limit failures into redacted `Ai_error` values.

- [ ] **Step 4: Implement Huawei adapters from canonical objects.**

Embedding sends resolved provider model, input and float encoding, parses
`data[].embedding`, verifies a returned vector for every requested input and
checks exactly 1024 values for `bge-m3`. V2 chat maps canonical task/input and
configured defaults to its request, parses usage, request ID, final content and
finish reason, and never exposes reasoning.

- [ ] **Step 5: Run adapter tests.**

Run: `ninja -C build-debug ai_huawei_maas_adapter-t && build-debug/runtime_output_directory/ai_huawei_maas_adapter-t`

Expected: exit code 0; fixtures prove no network use, final-content validation,
token parsing, non-2xx redaction, timeout and bge-m3 dimension rejection.

- [ ] **Step 6: Commit the transport and Huawei adapter.**

```bash
git add sql/ai/ai_http_transport.* sql/ai/ai_huawei_maas_adapter.* \
  sql/CMakeLists.txt unittest/gunit/ai_huawei_maas_adapter-t.cc \
  unittest/gunit/CMakeLists.txt
git commit -m "feat: add Huawei MaaS DB4AI provider adapter"
```

### Task 4: Runtime policy, audit and metering

**Files:**
- Create: `sql/ai/ai_runtime.h`, `sql/ai/ai_runtime.cc`, `sql/ai/ai_audit.h`, `sql/ai/ai_audit.cc`, `unittest/gunit/ai_runtime-t.cc`
- Modify: `sql/CMakeLists.txt`, `unittest/gunit/CMakeLists.txt`

**Interfaces:**
- Consumes: model registry, credential resolver, adapter registry, canonical types.
- Produces: `Ai_runtime::Embed(...)`, `Ai_runtime::Analyze(...)`, `Ai_audit_sink::Create(...)`, `Ai_audit_sink::Complete(...)`.

- [ ] **Step 1: Write failing policy tests.**

```cpp
TEST(AiRuntimeTest, RejectsProviderPrivateOptions) {
  EXPECT_EQ(Ai_error::k_invalid_options,
            runtime.ParseAnalyzeOptions(R"({"temperature":0})", &options));
}

TEST(AiRuntimeTest, RecordsReasoningTokensButNeverReasoningText) {
  auto audit = RunFixtureWithReasoning();
  EXPECT_EQ(17U, audit.reasoning_tokens);
  EXPECT_FALSE(audit.Contains("reasoning_content"));
}
```

- [ ] **Step 2: Run the target and confirm the policy implementation is absent.**

Run: `ninja -C build-debug ai_runtime-t`

Expected: compilation failure naming `Ai_runtime`.

- [ ] **Step 3: Implement option policy and capability dispatch.**

Accept only the six documented stable fields, require JSON object input, enforce
configured max-output and timeout ceilings, and select the adapter using the
resolved Profile's provider, capability, revision and endpoint type. Check
`AI_INVOKE` before resolver/transport work.

- [ ] **Step 4: Implement independent audit lifecycle.**

Before dispatch, create a `CREATED` record in an internal transaction. On
success/failure, update the same request with resolved model/config, provider
request ID, usage, latency, HTTP state and error class. Never persist secret,
prompt, response, endpoint URL or reasoning content. If the initial write
fails, return a fail-closed audit error and do not dispatch.

- [ ] **Step 5: Run runtime tests.**

Run: `ninja -C build-debug ai_runtime-t && build-debug/runtime_output_directory/ai_runtime-t`

Expected: exit code 0; tests prove option rejection, privilege denial, audit
fail-closed behavior, usage preservation across caller rollback and incomplete
chat behavior.

- [ ] **Step 6: Commit runtime governance.**

```bash
git add sql/ai/ai_runtime.* sql/ai/ai_audit.* sql/CMakeLists.txt \
  unittest/gunit/ai_runtime-t.cc unittest/gunit/CMakeLists.txt
git commit -m "feat: add DB4AI runtime policy and audit metering"
```

### Task 5: Native SQL functions and VECTOR codec

**Files:**
- Create: `sql/ai/ai_vector_codec.h`, `sql/ai/ai_vector_codec.cc`, `sql/ai/item_ai_func.h`, `sql/ai/item_ai_func.cc`, `unittest/gunit/ai_vector_codec-t.cc`
- Modify: `sql/item_create.cc`, `sql/CMakeLists.txt`, `unittest/gunit/CMakeLists.txt`, `share/messages_to_clients.txt`

**Interfaces:**
- Consumes: `Ai_runtime::Embed`, `Ai_runtime::Analyze`, resolved model metadata.
- Produces: native functions `AI_EMBEDDING`, `AI_ANALYZE`, `AI_MODEL_INFO`; `Ai_vector_codec::Encode(std::span<const float>, String *)`.

- [ ] **Step 1: Write failing codec and Item-resolution tests.**

```cpp
TEST(AiVectorCodecTest, EncodesLittleEndianFloatVector) {
  String vector;
  ASSERT_EQ(Ai_error::k_ok, Ai_vector_codec::Encode({1.0F, 2.0F}, &vector));
  EXPECT_EQ(8U, vector.length());
}

TEST(AiVectorCodecTest, RejectsNaNAndUnexpectedDimension) {
  EXPECT_EQ(Ai_error::k_dimension_mismatch,
            Ai_vector_codec::Validate({NAN}, 1024));
}
```

- [ ] **Step 2: Run the target and confirm it fails before codec creation.**

Run: `ninja -C build-debug ai_vector_codec-t`

Expected: compilation failure naming `Ai_vector_codec`.

- [ ] **Step 3: Implement native Items and return contracts.**

`Item_func_ai_embedding` accepts one to three string/integer parameters, calls
the runtime, uses `set_data_type_vector()` and returns the native binary float
representation accepted by `VECTOR(N)`. `Item_func_ai_analyze` accepts two or
three text/JSON arguments, returns utf8mb4 final content or controlled JSON.
`Item_func_ai_model_info` returns only sanitized metadata. Add explicit server
error messages for invalid DB4AI options, incomplete output, configuration,
authorization, timeout and dimension/space mismatch.

- [ ] **Step 4: Register functions and verify direct vector compatibility.**

Add the three names to `sql/item_create.cc`. Verify that
`VECTOR_DIM(AI_EMBEDDING(...))`, inserting into `VECTOR(1024)`, and
`VEC_DISTANCE_COSINE(column, AI_EMBEDDING(...))` use the result without a
string-to-vector conversion.

- [ ] **Step 5: Run codec and server build checks.**

Run: `ninja -C build-debug ai_vector_codec-t mysqld && build-debug/runtime_output_directory/ai_vector_codec-t`

Expected: exit code 0; server links and codec tests cover native layout,
non-finite values, fixed dimension and direct VECTOR compatibility.

- [ ] **Step 6: Commit native SQL surface.**

```bash
git add sql/ai/ai_vector_codec.* sql/ai/item_ai_func.* sql/item_create.cc \
  sql/CMakeLists.txt share/messages_to_clients.txt \
  unittest/gunit/ai_vector_codec-t.cc unittest/gunit/CMakeLists.txt
git commit -m "feat: add DB4AI native SQL functions"
```

### Task 6: Offline MTR coverage for functions, configuration and errors

**Files:**
- Create: `mysql-test/suite/rds/t/ai_maas_functions.test`, `mysql-test/suite/rds/r/ai_maas_functions.result`, `mysql-test/suite/rds/t/ai_maas_security.test`, `mysql-test/suite/rds/r/ai_maas_security.result`
- Modify: `mysql-test/suite/rds/suite.opt` only if the suite needs a static mock endpoint fixture

**Interfaces:**
- Consumes: native functions and a runtime test-only deterministic adapter registration hook.
- Produces: MTR proof that normal test execution never performs network I/O.

- [ ] **Step 1: Write MTR expectations before enabling the mock.**

```sql
--error ER_AI_MODEL_CONFIGURATION
SELECT AI_EMBEDDING('offline');
--error ER_AI_ACCESS_DENIED
SELECT AI_ANALYZE('summarize', 'offline');
```

- [ ] **Step 2: Run MTR and record the expected missing-feature failure.**

Run: `cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_functions`

Expected: failure because the test or functions do not yet exist.

- [ ] **Step 3: Add deterministic model Profiles and adapter fixtures.**

Fixtures must cover 1024-dimensional embeddings, normal chat content,
reasoning-only response, length finish, non-2xx error, malformed embedding,
timeout and response-size failure. The fixture names and result files contain no
secret, endpoint, raw prompt, raw response or complete embedding.

- [ ] **Step 4: Add customer-contract and security MTR cases.**

Cover one/two/three embedding arguments, `NULL`, invalid dimension, options
whitelist, final-content behavior, JSON source return, model capability mismatch,
inactive Profile, `AI_INVOKE` denial, `AI_AUDIT_VIEWER` visibility, keyring
missing fail-closed and redacted errors.

- [ ] **Step 5: Run MTR suites.**

Run: `cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_functions ai_maas_security`

Expected: both tests pass with no external connection attempt.

- [ ] **Step 6: Commit offline MTR evidence.**

```bash
git add mysql-test/suite/rds/t/ai_maas_*.test mysql-test/suite/rds/r/ai_maas_*.result
git commit -m "test: cover DB4AI MaaS native functions offline"
```

### Task 7: Vector/HNSW RAG and source provenance evidence

**Files:**
- Create: `mysql-test/suite/rds/t/ai_maas_rag.test`, `mysql-test/suite/rds/r/ai_maas_rag.result`, `Docs/db4ai/alisql-maas-rag-tutorial.md`, `Docs/db4ai/examples/rag_product_manual.sql`
- Modify: `Docs/db4ai/alisql-maas-p0-low-level-design.md` only to link completed evidence

**Interfaces:**
- Consumes: `AI_EMBEDDING`, `AI_ANALYZE(mode='rag')`, VECTOR/HNSW and fixture model Profiles.
- Produces: repeatable tenant-filtered retrieval and answer-with-sources demonstration.

- [ ] **Step 1: Write a failing RAG MTR case.**

```sql
CREATE TABLE ai_doc_chunks (
  tenant_id BIGINT NOT NULL,
  source_id VARCHAR(128) NOT NULL,
  chunk_id BIGINT NOT NULL,
  service_tier VARCHAR(32) NOT NULL,
  content TEXT NOT NULL,
  embedding VECTOR(1024) NOT NULL,
  embedding_config_id BIGINT NOT NULL,
  embedding_config_version BIGINT NOT NULL,
  embedding_space_id VARCHAR(128) NOT NULL,
  embedding_dimension INT NOT NULL,
  PRIMARY KEY (tenant_id, source_id, chunk_id),
  VECTOR INDEX embedding_hnsw (embedding) DISTANCE=cosine
);
```

- [ ] **Step 2: Run the MTR case and confirm the missing AI path fails.**

Run: `cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_rag`

Expected: failure before the SQL functions/fixture Profile exist.

- [ ] **Step 3: Implement the fixture-backed RAG flow.**

Insert two tenants and two service tiers, embed chunks through
`AI_EMBEDDING`, and query only `tenant_id=42`, one service tier and one
embedding space. Aggregate the recalled rows into `AI_ANALYZE` input with
source/chunk IDs. The mock response must return only those database-provided
source IDs.

- [ ] **Step 4: Add mismatch and cross-tenant assertions.**

Attempt a query with another `embedding_space_id` and expect the space-mismatch
error. Verify result JSON has no tenant-99 source when tenant 42 is queried.

- [ ] **Step 5: Run vector and RAG MTR.**

Run: `cd build-debug/mysql-test && ./mtr --suite=rds vidx_func ai_maas_rag`

Expected: exit code 0 and RAG evidence proves vector write, cosine retrieval,
tenant/scalar filtering and provenance.

- [ ] **Step 6: Commit the RAG tutorial and regression test.**

```bash
git add mysql-test/suite/rds/t/ai_maas_rag.test mysql-test/suite/rds/r/ai_maas_rag.result \
  Docs/db4ai/alisql-maas-rag-tutorial.md Docs/db4ai/examples/rag_product_manual.sql \
  Docs/db4ai/alisql-maas-p0-low-level-design.md
git commit -m "feat: add governed DB4AI RAG example"
```

### Task 8: SQL-result analysis, DBA read-only diagnosis and documentation

**Files:**
- Create: `mysql-test/suite/rds/t/ai_maas_analysis.test`, `mysql-test/suite/rds/r/ai_maas_analysis.result`, `Docs/db4ai/alisql-maas-analysis-and-diagnosis.md`, `Docs/db4ai/alisql-maas-security-and-operations.md`, `Docs/db4ai/alisql-vs-polardb-ai-capability.md`

**Interfaces:**
- Consumes: `AI_ANALYZE` modes `analyze`, `diagnose`, `rag` and offline fixtures.
- Produces: repeatable customer-value examples and a source-backed comparison.

- [ ] **Step 1: Write failing analysis/diagnosis MTR cases.**

```sql
SELECT AI_ANALYZE('Explain weekly revenue movement',
                  JSON_ARRAY(JSON_OBJECT('week', '2026-W30', 'sales', 80)),
                  JSON_OBJECT('mode', 'analyze'));
SELECT AI_ANALYZE('Diagnose only from supplied evidence',
                  JSON_OBJECT('sql_text', 'SELECT ...', 'rows_examined', 9000,
                              'latency_ms', 1200),
                  JSON_OBJECT('mode', 'diagnose', 'output_format', 'json'));
```

- [ ] **Step 2: Run MTR and confirm the test fails before fixtures are present.**

Run: `cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_analysis`

Expected: failure before `AI_ANALYZE` fixture support exists.

- [ ] **Step 3: Implement fixture responses and assertions.**

Require analysis output to be final content. Require diagnose JSON to contain
`reason`, `evidence`, `recommendation` and `risk`; fixtures must not contain an
executable repair statement. Test that provider-private options and automatic
SQL/parameter actions are rejected.

- [ ] **Step 4: Write operational and comparison documentation.**

Document data egress, privileges, credential-reference provisioning, audit
fields, token semantics, timeout/rate/response limits, model migration,
failure classification and the opt-in-only boundary. The PolarDB comparison
links each claim to this repository's tests/docs or the recorded external
validation, and marks unavailable work as future work.

- [ ] **Step 5: Run the analysis MTR suite.**

Run: `cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_analysis`

Expected: exit code 0 with both analysis and evidence-only diagnosis outputs.

- [ ] **Step 6: Commit examples and documentation.**

```bash
git add mysql-test/suite/rds/t/ai_maas_analysis.test mysql-test/suite/rds/r/ai_maas_analysis.result \
  Docs/db4ai/alisql-maas-analysis-and-diagnosis.md \
  Docs/db4ai/alisql-maas-security-and-operations.md \
  Docs/db4ai/alisql-vs-polardb-ai-capability.md
git commit -m "docs: add DB4AI analysis and diagnosis evidence"
```

### Task 9: Opt-in MaaS smoke, full validation and release audit

**Files:**
- Create: `Docs/db4ai/tools/huawei_maas_smoke.py`, `Docs/db4ai/alisql-maas-p0-validation.md`
- Modify: `Docs/db4ai/alisql-maas-p0-low-level-design.md`, `Docs/db4ai/alisql-maas-rag-tutorial.md`

**Interfaces:**
- Consumes: deployed server configuration with a keyring credential reference or explicitly development-only credential setup.
- Produces: sanitized manual evidence; it is not an MTR dependency.

- [ ] **Step 1: Write the smoke script's no-secret output contract.**

```python
assert "Authorization" not in rendered_output
assert "api_key" not in rendered_output.lower()
print({"model": model, "http_status": status, "dimension": dimension,
       "has_final_content": has_final_content, "usage": usage, "latency_ms": latency})
```

- [ ] **Step 2: Run the script without opt-in and confirm it refuses to call MaaS.**

Run: `python3 Docs/db4ai/tools/huawei_maas_smoke.py`

Expected: exit code 2 with an instruction requiring `--run-live` and a secure
credential reference; no request is sent.

- [ ] **Step 3: Implement explicit live gating.**

Require `--run-live`, a preconfigured database credential reference and a
small fixed input. Run one bge-m3 embedding and one chat call, asserting 1024
dimensions and non-empty final content. Emit model/config version, status,
dimension, usage and latency only.

- [ ] **Step 4: Execute default regression verification.**

Run: `cmake --build build-debug --parallel 8 && cd build-debug/mysql-test && ./mtr --suite=rds vidx_func ai_maas_functions ai_maas_security ai_maas_rag ai_maas_analysis`

Expected: exit code 0; no default test requires internet access.

- [ ] **Step 5: Execute the live smoke only with explicit operator authorization.**

Run: `python3 Docs/db4ai/tools/huawei_maas_smoke.py --run-live --credential-ref <preconfigured-reference>`

Expected: the sanitized summary shows a 1024-dimensional embedding and a chat
response with non-empty final content. If entitlement, region or account access
fails, record the classified failure without weakening default-test results.

- [ ] **Step 6: Complete the release evidence document and commit.**

```bash
git add Docs/db4ai/tools/huawei_maas_smoke.py Docs/db4ai/alisql-maas-p0-validation.md \
  Docs/db4ai/alisql-maas-p0-low-level-design.md Docs/db4ai/alisql-maas-rag-tutorial.md
git commit -m "test: add opt-in Huawei MaaS smoke validation"
```

## Plan self-review

| Requirement | Planned evidence |
|---|---|
| Stable AI SQL and provider-neutral adapter selection | Tasks 1, 3, 4 and 5 |
| Configuration versions, credentials, tenant and permissions | Task 2 and Task 6 |
| Final-content-only generation, usage and error categories | Tasks 1, 3, 4 and 6 |
| Native VECTOR/HNSW compatibility and embedding-space safety | Tasks 5 and 7 |
| RAG with tenant/scalar filters and provenance | Task 7 |
| SQL analysis and DBA read-only diagnosis | Task 8 |
| Default offline tests and opt-in live verification | Tasks 6 and 9 |
| Operations, limits and PolarDB comparison | Tasks 8 and 9 |

The plan contains no provider secret, production endpoint or real MaaS request.
