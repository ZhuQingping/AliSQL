# TaurusDB MaaS P0 Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete the governed Huawei MaaS P0 path with a simple `dbms_ai` model-management package, explicit model selection, controlled Analyze semantics, durable audit behavior, and repeatable validation.

**Architecture:** Keep `mysql.taurusdb_ai_model_config` as the only internal control-plane table and expose four native procedures in `dbms_ai`. The runtime resolves only an explicitly named active model and derives fixed Huawei endpoints from capability. `AI_ANALYZE` receives canonical mode/options, applies a server-owned prompt, and returns database-composed JSON where RAG sources come from filtered SQL evidence rather than the model.

**Tech Stack:** AliSQL C++17 server, native package procedures, MySQL dynamic privileges, system-table handler access, libcurl, RapidJSON, MTR, GUnit.

## Global Constraints

- P0 supports only Huawei MaaS standard text embedding and V2 Chat endpoints.
- No new AI system table, tenant table, per-model grant, default model, audit SQL query, cloud-network resource, async task, streaming, or multi-provider adapter.
- Every AI invocation explicitly names its model: `AI_EMBEDDING(text, model_name[, dimension])`; `AI_ANALYZE` requires `options_json.model_name`.
- `dbms_ai` only offers `register_model`, `update_model`, `delete_model`, and `show_models`; all require `AI_ADMIN`.
- `PLAINTEXT_DEV` remains Debug/development-only. Release runtime requires `SECRET_REF`.
- Never print API keys, Secret references, Authorization headers, request bodies, model responses, or raw embeddings to SQL errors or audit files.
- Default MTR remains offline and deterministic. Real MaaS tests run only through explicit sourceable SQL scripts.

---

## File Structure

- Create `sql/ai/ai_model_admin.h`: native-procedure declarations and `dbms_ai` schema constant.
- Create `sql/ai/ai_model_admin.cc`: parameter validation, `AI_ADMIN` checks, system-table mutation/read helpers, and four native procedure implementations.
- Modify `sql/package/package_cache.cc`: register four `dbms_ai` native procedures.
- Modify `sql/CMakeLists.txt`: link the new model-admin implementation into `sql_main`.
- Modify `sql/ai/ai_model_registry.{h,cc}`: derive fixed Huawei endpoints, resolve only named active profiles, remove default/tenant/distance-metric runtime dependencies, and share table constants with the management implementation.
- Modify `sql/ai/item_ai_func.cc`: reject omitted embedding model names and absent Analyze `model_name` before audit or network access.
- Modify `sql/ai/ai_types.h`, `sql/ai/ai_runtime.{h,cc}`, `sql/ai/ai_runtime_server.cc`, `sql/ai/ai_huawei_maas_adapter.cc`: carry controlled Analyze request/response metadata, validate RAG/diagnose input, construct server-owned system prompts, and compose canonical JSON results.
- Modify `sql/ai/ai_file_audit.cc` and `sql/ai/ai_runtime_server.cc`: classify terminal-audit write failure, emit a redacted server warning, and retain missing-terminal = `UNKNOWN` semantics.
- Create/modify MTR tests under `mysql-test/suite/rds/t/` and `.result` files for model management, explicit model selection, Analyze contract, and audit failures.
- Modify `Docs/db4ai/taurusdb-maas-p0-high-level-design.md` and `Docs/db4ai/taurusdb-maas-p0-low-level-design.md` only when implementation changes an approved contract.

## Task 1: Add Failing Native-Procedure Contract Tests

**Files:**
- Create: `mysql-test/suite/rds/t/ai_maas_model_admin.test`
- Create: `mysql-test/suite/rds/r/ai_maas_model_admin.result`
- Modify: `mysql-test/suite/rds/t/ai_maas_contract.test`
- Modify: `mysql-test/suite/rds/r/ai_maas_contract.result`

**Consumes:** Existing `AI_ADMIN`, `AI_INVOKE`, fixture profiles, and native package test conventions.

**Produces:** Failing acceptance tests for the only supported model-management API and for explicit model names.

- [ ] **Step 1: Write the failing management-package MTR test**

  Add calls with the exact accepted contract:

  ```sql
  GRANT AI_ADMIN ON *.* TO 'root'@'localhost';
  CALL dbms_ai.register_model('mtr/fixture-embedding', 'TEXT_EMBEDDING',
                              'fixture-embedding', 'SECRET_REF', 'mtr/not-read');
  CALL dbms_ai.show_models();
  CALL dbms_ai.update_model('mtr/fixture-embedding', 'TEXT_EMBEDDING',
                            'fixture-embedding-v2', 'SECRET_REF', 'mtr/not-read');
  CALL dbms_ai.delete_model('mtr/fixture-embedding', 'TEXT_EMBEDDING');
  ```

  Add a second connection with only table DML and assert it is rejected without `AI_ADMIN`. Assert `show_models()` has columns `MODEL_NAME`, `CAPABILITY`, `PROVIDER_MODEL_NAME`, `DIMENSION`, and `CONFIG_VERSION`, and does not expose credential, endpoint, or status columns.

- [ ] **Step 2: Verify the management test fails for the missing package**

  Run: `cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_model_admin`

  Expected: FAIL because `dbms_ai.register_model` does not exist.

- [ ] **Step 3: Add failing explicit-model assertions**

  In `ai_maas_contract.test`, assert `AI_EMBEDDING('text')` and `AI_ANALYZE('task', JSON_OBJECT('x', 1))` fail locally; retain successful fixture calls that provide a model name.

- [ ] **Step 4: Verify the explicit-model assertions fail against current behavior**

  Run: `cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_contract`

  Expected: FAIL because current runtime still resolves a default profile.

- [ ] **Step 5: Commit the red test contract**

  ```bash
  git add mysql-test/suite/rds/t/ai_maas_model_admin.test \
          mysql-test/suite/rds/r/ai_maas_model_admin.result \
          mysql-test/suite/rds/t/ai_maas_contract.test \
          mysql-test/suite/rds/r/ai_maas_contract.result
  git commit -m "test(db4ai): define model admin package contract"
  ```

## Task 2: Implement the Simplified `dbms_ai` Model Control Plane

**Files:**
- Create: `sql/ai/ai_model_admin.h`
- Create: `sql/ai/ai_model_admin.cc`
- Modify: `sql/package/package_cache.cc`
- Modify: `sql/CMakeLists.txt`
- Modify: `sql/ai/ai_model_registry.{h,cc}`
- Test: `mysql-test/suite/rds/t/ai_maas_model_admin.test`

**Consumes:** Task 1’s procedure signatures and the existing 21-field `taurusdb_ai_model_config` upgrade-compatible schema.

**Produces:** Four native `dbms_ai` calls and a resolver that accepts only explicitly named active Huawei profiles.

- [ ] **Step 1: Implement a model-control service with no customer Endpoint or status input**

  Define a request type:

  ```cpp
  struct Ai_model_admin_request {
    std::string model_name;
    Ai_capability capability;
    std::string provider_model_name;
    std::string credential_mode;
    Secure_string credential_value;
  };
  ```

  Validate non-empty values; accept only `TEXT_EMBEDDING`, `TEXT_GENERATION`, `PLAINTEXT_DEV`, and `SECRET_REF`; derive provider `huawei`, `BEARER_API_KEY`, Endpoint, and dimension internally. Reject a non-`bge-m3` embedding profile and a non-1024 embedding request in P0.

- [ ] **Step 2: Implement system-table atomic lifecycle operations**

  Use a dedicated `System_table_access` subclass with write locks. `register_model` fails if any profile already uses the logical name/capability. `update_model` marks the active row `DISABLED`, writes a copied successor with `config_version + 1` and `ACTIVE`, and commits both changes together. `delete_model` marks all matching rows `RETIRED`. On any handler error rollback the internal transaction and return a redacted SQL error.

- [ ] **Step 3: Implement `show_models` as a safe result set**

  Scan only active rows and emit exactly the five approved metadata columns. Do not load `api_key_plaintext` into the result, and do not write credential values to server diagnostics.

- [ ] **Step 4: Implement and register native package procedures**

  Add `Ai_model_admin_proc_base` and four `Sql_cmd_admin_proc` subclasses. Override `check_access(THD *)` to require the `AI_ADMIN` dynamic privilege rather than `SUPER`. Register them under the new `dbms_ai` schema in `package_context_init()` and add `ai_model_admin.cc` to `sql/CMakeLists.txt`.

- [ ] **Step 5: Remove default resolution and derive Huawei endpoints in the resolver**

  Make `Ai_model_registry::Resolve()` reject an empty model name. Replace table-supplied Endpoint use with a capability-to-constant mapping. Keep `status` internal: only `ACTIVE` resolves. Do not populate tenant, distance metric, or default-model fields in the resolved request path.

- [ ] **Step 6: Build and verify Task 1 is green**

  Run:

  ```bash
  cmake --build build-debug --target mysqld -j 8
  cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_model_admin ai_maas_contract
  ```

  Expected: both tests PASS; `show_models()` contains no sensitive columns; non-admin calls fail.

- [ ] **Step 7: Commit the control-plane implementation**

  ```bash
  git add sql/ai/ai_model_admin.h sql/ai/ai_model_admin.cc \
          sql/ai/ai_model_registry.h sql/ai/ai_model_registry.cc \
          sql/package/package_cache.cc sql/CMakeLists.txt \
          mysql-test/suite/rds/t/ai_maas_model_admin.test \
          mysql-test/suite/rds/r/ai_maas_model_admin.result \
          mysql-test/suite/rds/t/ai_maas_contract.test \
          mysql-test/suite/rds/r/ai_maas_contract.result
  git commit -m "feat(db4ai): add simplified dbms_ai model management"
  ```

## Task 3: Enforce Explicit Model Selection at the SQL Boundary

**Files:**
- Modify: `sql/ai/item_ai_func.cc`
- Modify: `mysql-test/suite/rds/t/ai_maas_embedding.test`
- Modify: `mysql-test/suite/rds/r/ai_maas_embedding.result`
- Modify: `mysql-test/suite/rds/t/ai_maas_analysis.test`
- Modify: `mysql-test/suite/rds/r/ai_maas_analysis.result`

**Consumes:** Task 2’s named-only resolver.

**Produces:** Local input errors before audit, credentials, or HTTP when the model is omitted.

- [ ] **Step 1: Write the failing MTR cases**

  Assert these are rejected before fixture execution:

  ```sql
  --error ER_WRONG_ARGUMENTS
  SELECT AI_EMBEDDING('missing model');
  --error ER_WRONG_ARGUMENTS
  SELECT AI_ANALYZE('task', JSON_OBJECT('rows', 1));
  --error ER_WRONG_ARGUMENTS
  SELECT AI_ANALYZE('task', JSON_OBJECT('rows', 1), JSON_OBJECT('mode', 'analyze'));
  ```

- [ ] **Step 2: Run the tests to observe the missing enforcement**

  Run: `cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_embedding ai_maas_analysis`

  Expected: FAIL because omitted models currently reach default resolution.

- [ ] **Step 3: Require the model arguments in `item_ai_func.cc`**

  Require two or three arguments for `AI_EMBEDDING`; require three arguments for `AI_ANALYZE`; require a non-empty string `options.model_name` after parsing. Return `ER_WRONG_ARGUMENTS` before creating `Ai_runtime` or its audit sink.

- [ ] **Step 4: Re-run focused MTR**

  Run: `cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_embedding ai_maas_analysis ai_maas_contract`

  Expected: PASS.

- [ ] **Step 5: Commit the explicit-model boundary**

  ```bash
  git add sql/ai/item_ai_func.cc mysql-test/suite/rds/t/ai_maas_embedding.test \
          mysql-test/suite/rds/r/ai_maas_embedding.result \
          mysql-test/suite/rds/t/ai_maas_analysis.test \
          mysql-test/suite/rds/r/ai_maas_analysis.result
  git commit -m "feat(db4ai): require explicit AI model selection"
  ```

## Task 4: Make `AI_ANALYZE` Return Governed JSON and RAG Sources

**Files:**
- Modify: `sql/ai/ai_types.h`
- Modify: `sql/ai/ai_runtime.h`
- Modify: `sql/ai/ai_runtime.cc`
- Modify: `sql/ai/ai_runtime_server.cc`
- Modify: `sql/ai/ai_huawei_maas_adapter.cc`
- Modify: `sql/ai/item_ai_func.cc`
- Modify: `mysql-test/suite/rds/t/ai_maas_analysis.test`
- Modify: `mysql-test/suite/rds/r/ai_maas_analysis.result`
- Modify: `mysql-test/suite/rds/t/ai_maas_rag.test`
- Modify: `mysql-test/suite/rds/r/ai_maas_rag.result`

**Consumes:** Explicit model selection and existing canonical Huawei V2 Chat adapter.

**Produces:** Server-owned system prompts, valid canonical JSON output, and source provenance attached from SQL evidence.

- [ ] **Step 1: Write failing RAG and diagnose tests**

  Add a RAG fixture invocation with JSON input and assert the result contains only the supplied `source_id`/`chunk_id` values:

  ```sql
  SELECT JSON_EXTRACT(AI_ANALYZE(
    '根据资料回答问题。',
    JSON_OBJECT('question', '如何处理网关离线？', 'sources', JSON_ARRAY(
      JSON_OBJECT('source_id', 'manual-42', 'chunk_id', 1,
                  'content', 'Tenant 42 gateway procedure.'))),
    JSON_OBJECT('model_name', 'mtr/fixture-chat', 'mode', 'rag',
                'output_format', 'json', 'return_sources', true)),
    '$.sources[0].source_id');
  ```

  Add failures for missing `sources`, `return_sources` outside RAG JSON mode, and `diagnose` input that is not a JSON object.

- [ ] **Step 2: Run MTR to prove the current text-only result fails**

  Run: `cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_analysis ai_maas_rag`

  Expected: FAIL because current code ignores `mode`, `output_format`, and `return_sources`.

- [ ] **Step 3: Parse and validate canonical Analyze input**

  Extend `Ai_analyze_options` with a required non-empty `model_name`. For RAG, parse the input object into safe source references and model context; reject absent/malformed sources. For diagnose, reject non-object evidence. Keep task text and source content in request memory only.

- [ ] **Step 4: Build server-owned Chat messages and canonical output**

  Add `BuildAnalyzeSystemPrompt(mode)` with fixed prompts for generic analysis, RAG, and diagnose. Adapter receives the fixed system message and a user message containing task plus input. When JSON output is requested, compose the JSON in Runtime from final content, resolved model/configuration, parsed usage, and SQL-provided source IDs; do not parse or return provider-private JSON.

- [ ] **Step 5: Verify RAG and diagnose safety cases**

  Run: `cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_analysis ai_maas_rag`

  Expected: PASS; fixture RAG sources equal input sources and diagnosis remains final content only.

- [ ] **Step 6: Commit Analyze governance**

  ```bash
  git add sql/ai/ai_types.h sql/ai/ai_runtime.h sql/ai/ai_runtime.cc \
          sql/ai/ai_runtime_server.cc sql/ai/ai_huawei_maas_adapter.cc \
          sql/ai/item_ai_func.cc mysql-test/suite/rds/t/ai_maas_analysis.test \
          mysql-test/suite/rds/r/ai_maas_analysis.result \
          mysql-test/suite/rds/t/ai_maas_rag.test \
          mysql-test/suite/rds/r/ai_maas_rag.result
  git commit -m "feat(db4ai): govern analyze RAG and diagnose output"
  ```

## Task 5: Complete Audit Failure Semantics and Regression Coverage

**Files:**
- Modify: `sql/ai/ai_file_audit.cc`
- Modify: `sql/ai/ai_runtime_server.cc`
- Modify: `sql/ai/item_ai_func.cc`
- Modify: `unittest/gunit/ai_file_audit-t.cc`
- Modify: `mysql-test/suite/rds/t/ai_maas_governance.test`
- Modify: `mysql-test/suite/rds/r/ai_maas_governance.result`

**Consumes:** Existing two-phase JSONL sink.

**Produces:** Fail-closed pre-egress handling and auditable missing-terminal semantics without leaking request data.

- [ ] **Step 1: Write failing tests for terminal audit failure classification**

  Extend GUnit with a sink that succeeds on `Start()` and fails on `Complete()`. Assert Runtime returns `k_audit_unavailable`, preserves the original `call_id`, and never serializes credential or request text. Add MTR coverage that a non-admin cannot set `ai_invoke_audit` and that no SESSION scope exists.

- [ ] **Step 2: Run the focused GUnit target to observe the missing behavior**

  Run: `cmake --build build-debug --target merge_small_tests-t -j 8 && build-debug/runtime_output_directory/merge_small_tests-t --gtest_filter='AiFileAuditSinkTest.*'`

  Expected: FAIL for the terminal-failure classification assertion.

- [ ] **Step 3: Add redacted server-side failure reporting**

  When `Complete()` fails after a successful start, return `k_audit_unavailable` and log only call ID, capability, logical model name, and `AUDIT_TERMINAL_WRITE_FAILED`. Do not attempt to claim a durable terminal event. Document that log collectors classify a started call lacking terminal event as `UNKNOWN`.

- [ ] **Step 4: Re-run GUnit and MTR governance tests**

  Run:

  ```bash
  cmake --build build-debug --target merge_small_tests-t -j 8
  build-debug/runtime_output_directory/merge_small_tests-t --gtest_filter='AiFileAuditSinkTest.*'
  cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_governance
  ```

  Expected: PASS.

- [ ] **Step 5: Commit audit completion semantics**

  ```bash
  git add sql/ai/ai_file_audit.cc sql/ai/ai_runtime_server.cc \
          sql/ai/item_ai_func.cc unittest/gunit/ai_file_audit-t.cc \
          mysql-test/suite/rds/t/ai_maas_governance.test \
          mysql-test/suite/rds/r/ai_maas_governance.result
  git commit -m "fix(db4ai): report incomplete audit terminal events"
  ```

## Task 6: Run P0 Regression, Real Smoke, and Update Documentation

**Files:**
- Modify: `Docs/db4ai/taurusdb-maas-p0-high-level-design.md`
- Modify: `Docs/db4ai/taurusdb-maas-p0-low-level-design.md`
- Modify: `scripts/db4ai_maas_smoke.sql`
- Modify: `scripts/db4ai_maas_real_embedding_rag_smoke.sql`

**Consumes:** Tasks 1-5.

**Produces:** A sourceable real-provider workflow matching the new `dbms_ai` package and verification evidence.

- [ ] **Step 1: Update real smoke SQL to use `dbms_ai.register_model`**

  Remove all direct system-table model DML from the scripts. Add commented examples for development `PLAINTEXT_DEV` registration and production `SECRET_REF` registration. Keep API Key values as user-provided inputs; never embed a real key in the repository.

- [ ] **Step 2: Run the full offline DB4AI MTR suite**

  Run:

  ```bash
  cmake --build build-debug --target mysqld merge_small_tests-t -j 8
  cd build-debug/mysql-test && ./mtr --suite=rds \
    ai_maas_embedding ai_maas_analysis ai_maas_contract ai_maas_governance ai_maas_rag ai_maas_model_admin
  ```

  Expected: all selected tests PASS without network traffic.

- [ ] **Step 3: Run sourceable real smoke only with authorized credentials**

  On the configured local validation instance:

  ```sql
  CREATE DATABASE IF NOT EXISTS db4ai_validation;
  USE db4ai_validation;
  SET GLOBAL vidx_disabled = OFF;
  source /absolute/path/scripts/db4ai_maas_smoke.sql;
  source /absolute/path/scripts/db4ai_maas_real_embedding_rag_smoke.sql;
  ```

  Expected: each script prints `PASS` and removes its temporary tables. If no authorized credential is present, record the skipped real call; do not substitute a fabricated result.

- [ ] **Step 4: Perform release build validation**

  Run the repository’s configured Release build script or CMake build for `mysqld`. Expected: link succeeds with the `dbms_ai` package and no Debug-only fixture dependency.

- [ ] **Step 5: Update HLD verification state and commit**

  Update only completed behavior and known target-environment verification boundaries. Then run:

  ```bash
  git diff --check
  git add Docs/db4ai/taurusdb-maas-p0-high-level-design.md \
          Docs/db4ai/taurusdb-maas-p0-low-level-design.md \
          scripts/db4ai_maas_smoke.sql scripts/db4ai_maas_real_embedding_rag_smoke.sql
  git commit -m "docs(db4ai): document P0 governed MaaS validation"
  ```

## Plan Review

- P0 model management, explicit selection, Analyze semantics, audit semantics, offline tests, real smoke, and documentation each map to one task.
- The plan deliberately excludes multi-provider, async, streaming, multimodal, quotas, and cloud VPC changes.
- Every code task begins with a failing test and ends with focused verification before commit.
- All public interfaces are defined in the design and repeated in Task 1; no task relies on an unspecified API.
