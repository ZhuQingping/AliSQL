# DB4AI P0 Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete remaining DB4AI P0 governance, executable customer scenarios, and release evidence without changing the stable `AI_EMBEDDING` or `AI_ANALYZE` customer contract.

**Architecture:** Keep `sql/ai` as the only built-in Runtime/Provider Adapter path. Add a system-table audit sink with pre-egress creation and independent completion, then add deterministic offline fixtures for RAG, SQL analysis, and read-only diagnosis. Real MaaS remains explicit opt-in only.

**Tech Stack:** C++17, AliSQL system-table access, InnoDB system tables, dynamic privileges, native Item functions, VECTOR/HNSW, GUnit and MTR.

## Global Constraints

- Customer SQL never accepts API keys, endpoints, provider IDs, provider JSON, or Adapter names.
- Secrets, authorization headers, prompts, raw responses and full embeddings never enter audit, logs, tests, docs or SQL output.
- `PLAINTEXT_DEV` remains Debug-only; production requires `SECRET_REF`.
- Default GUnit/MTR are deterministic and do not call external HTTP.
- RAG keeps ordinary SQL tenant, security and scalar predicates; sources are database-derived.
- Diagnosis never executes SQL or performs a DBA repair.
- Preserve user-owned untracked `build-debug/` and `Docs/db4ai/validation/`.

---

### Task 1: Persistent redacted invocation audit

**Files:**
- Modify: `sql/ai/ai_audit.h`, `sql/ai/ai_runtime.h`, `sql/ai/ai_runtime_server.cc`, `sql/CMakeLists.txt`, `unittest/gunit/CMakeLists.txt`
- Create: `sql/ai/ai_audit.cc`, `unittest/gunit/ai_audit-t.cc`
- Modify: `scripts/mysql_system_tables.sql`, `scripts/mysql_system_tables_fix.sql`

**Interfaces:**
- Produces `Ai_audit_sink::Start(THD *, const Ai_audit_record &, uint64_t *)` and `Ai_audit_sink::Complete(THD *, uint64_t, const Ai_audit_record &)`, both returning `Ai_error`.
- `Ai_system_table_audit_sink` uses `mysql.alisql_ai_call_audit`; its projection has config id/version, tenant, capability, state, usage, provider request id, latency, HTTP status and redacted error only.

- [ ] **Step 1: Write failing GUnit lifecycle cases.**

Add `StartFailurePreventsDispatch`, `CompleteStoresOnlyTelemetry`, and `NoSecretFieldsExist`. Assert a failed `Start` returns `k_audit_unavailable` before fake-adapter dispatch and every persisted projection excludes input, task, credential and response fields.

- [ ] **Step 2: Run RED.**

Run `cmake --build build-debug --target ai_audit-t --parallel 8 && build-debug/runtime_output_directory/ai_audit-t`.

Expected: the target or audit lifecycle API is missing.

- [ ] **Step 3: Implement audit contract and table sink.**

Add `k_audit_unavailable`; insert `STARTED` before dispatch with server-owned `System_table_access`; update the same call id to `SUCCEEDED` or `FAILED` independently of the caller transaction. Add `completed_at`, `latency_ms`, redacted status/error fields and tenant/time/configuration indexes to bootstrap and upgrade DDL.

- [ ] **Step 4: Wire Runtime.**

After model resolution and before credential lookup in both `Embed` and `Analyze`, call `Start`; fail closed on error. Call `Complete` for credential, endpoint, adapter, response-validation and vector-codec outcomes exactly once.

- [ ] **Step 5: Run GREEN and commit.**

Run `cmake --build build-debug --target mysqld ai_audit-t ai_runtime-t --parallel 8 && build-debug/runtime_output_directory/ai_audit-t && build-debug/runtime_output_directory/ai_runtime-t`.

Commit: `git commit -m "feat: persist DB4AI redacted audit lifecycle"`.

### Task 2: Authorized sanitized audit read surface

**Files:**
- Modify: `sql/ai/item_ai_func.h`, `sql/ai/item_ai_func.cc`, `sql/item_create.cc`
- Create: `mysql-test/suite/rds/t/ai_maas_governance.test`, `mysql-test/suite/rds/r/ai_maas_governance.result`
- Modify: `Docs/db4ai/alisql-maas-p0-operations-and-examples.md`, `Docs/db4ai/alisql-maas-p0-validation-status.md`

**Interfaces:**
- Produces `AI_AUDIT_INFO([limit])`, with `limit` in `[1,100]`; callers require `AI_AUDIT_VIEWER` or `AI_ADMIN`.
- The JSON output includes only allowlisted audit fields and has no endpoint, credential ref, input, task, content or provider body.

- [ ] **Step 1: Write failing MTR privilege/redaction cases.**

Create a user with `AI_INVOKE` only and assert `AI_AUDIT_INFO()` returns `ER_SPECIFIC_ACCESS_DENIED_ERROR`. Grant `AI_AUDIT_VIEWER`, insert a deterministic audit fixture, and assert state/config/version/usage/status appear while sensitive key names do not. Assert `AI_ADMIN` implies audit read access.

- [ ] **Step 2: Run RED.**

Run `cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_governance`.

Expected: `AI_AUDIT_INFO` is not registered.

- [ ] **Step 3: Implement bounded Item read.**

Implement `Item_func_ai_audit_info`, register it in `sql/item_create.cc`, validate the optional integer limit, check the existing dynamic privileges, read allowlisted columns through system-table access, and serialize a RapidJSON array. Do not add raw table DML for ordinary users; document Profile/binding changes as controlled `AI_ADMIN` operations.

- [ ] **Step 4: Run GREEN and commit.**

Run `cmake --build build-debug --target mysqld --parallel 8 && cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_contract ai_maas_governance`.

Commit: `git commit -m "feat: expose sanitized DB4AI audit metadata"`.

### Task 3: Offline end-to-end RAG, analysis and diagnosis evidence

**Files:**
- Create: `mysql-test/suite/rds/t/ai_maas_rag.test`, `mysql-test/suite/rds/r/ai_maas_rag.result`, `mysql-test/suite/rds/t/ai_maas_analysis.test`, `mysql-test/suite/rds/r/ai_maas_analysis.result`
- Create: `Docs/db4ai/examples/rag_product_manual.sql`, `Docs/db4ai/alisql-maas-rag-analysis-diagnosis.md`
- Modify: test-only Runtime seam files and `Docs/db4ai/alisql-maas-p0-validation-status.md`

**Interfaces:**
- Test-only fixture Profiles return a fixed 1024-dimensional vector and final content for explicit MTR names only; they are compiled out of production Runtime paths.
- RAG table contains tenant/source/chunk, business filters, `VECTOR(1024)`, profile version and embedding-space id.

- [ ] **Step 1: Write failing RAG MTR.**

Create a product-manual chunk table, `VECTOR INDEX`, two-tenant `VEC_FROMTEXT` rows, and retrieval SQL with tenant/product/access/space/version predicates. Assert tenant 42 never receives tenant 77 sources, mismatched space/version fails, and a `mode='rag'` result is paired with SQL-generated sources.

- [ ] **Step 2: Establish RED and add only a test fixture seam.**

Run `cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_rag`; it must fail before fixtures. Add a server-test-only fake transport/Profile path that never accepts a user endpoint or credential and preserve production fail-closed behavior.

- [ ] **Step 3: Add analysis and diagnosis MTR.**

Use bounded aggregated JSON for `mode='analyze'` and SQL digest/EXPLAIN/rows/latency/lock/IO JSON for `mode='diagnose'`. Assert nonempty final content, rejected provider-private options, and no automatic repair claim.

- [ ] **Step 4: Write executable customer documentation.**

Document import/chunking, vector insertion, HNSW creation, safe retrieval, source assembly, RAG output, aggregate analysis, evidence-only diagnosis, expected output shape, failure boundaries and capacity limits.

- [ ] **Step 5: Run GREEN and commit.**

Run `cmake --build build-debug --target mysqld --parallel 8 && cd build-debug/mysql-test && ./mtr --suite=rds vidx_func ai_maas_rag ai_maas_analysis ai_maas_contract ai_maas_governance`.

Commit: `git commit -m "test: add governed DB4AI RAG and analysis evidence"`.

### Task 4: P0 evidence audit and release handoff

**Files:**
- Create: `Docs/db4ai/alisql-vs-polardb-ai-capability.md`
- Modify: `Docs/db4ai/alisql-maas-p0-validation-status.md`, `Docs/db4ai/alisql-maas-p0-low-level-design.md`

**Interfaces:**
- Produces a requirement-to-evidence matrix linking each P0 acceptance item to code, GUnit/MTR, opt-in smoke, or explicit limitation.

- [ ] **Step 1: Write the evidence and comparison matrix.**

Separate observed PolarDB PREDICT/AI-node evidence from unverified claims and cite the external validation document. Mark every absent capability as a future limitation.

- [ ] **Step 2: Run release verification and secret scan.**

Run `git diff --check`; run the Task 1-3 GUnit/MTR command set; then scan tracked source/docs/tests for API-key assignments and Authorization values, excluding user-owned generated directories.

- [ ] **Step 3: Commit and push.**

Commit: `git commit -m "docs: complete DB4AI P0 evidence audit"`; push `git push zhuqingping ai_maas`.

## Plan self-review

- Tasks 1-2 close persistent audit and authorized-read gaps.
- Task 3 covers the original goal’s executable RAG, SQL analysis and read-only diagnosis acceptance criteria without egress.
- Task 4 prevents unsupported PolarDB or production claims from being presented as verified capabilities.
