# AliSQL DB4AI Debug 明文凭据实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Debug AliSQL 中安全地从受控系统表读取 `PLAINTEXT_DEV` 凭据，并完成已授权的 Huawei MaaS embedding 与 V2 Chat 冒烟验证。

**Architecture:** Model registry 只解析非机密 Profile；credential resolver 在 dispatch 前按已冻结的 config id/version 读取明文 BLOB 并放入 `Secure_string`。Release 构建拒绝该 kind，生产 keyring `SECRET_REF` 路径不变；runtime、adapter、audit 和 metadata 都不持有或输出密钥。

**Tech Stack:** C++17、AliSQL system-table access、现有 `Secure_string`、GUnit、MTR、Debug mysqld、Huawei MaaS HTTPS JSON。

## Global Constraints

- 客户 SQL 接口不增加 API Key、endpoint、provider request JSON 或 adapter 参数。
- 仅 Debug 构建允许 `PLAINTEXT_DEV`；Release 返回 `k_credential_unavailable` 且不得 egress。
- 秘钥不得进入 `Ai_resolved_model`、`Ai_canonical_request`、metadata、audit、错误、测试结果、文档或 Git。
- 默认 GUnit/MTR 保持离线且确定性；真实 MaaS 只在所有离线验证成功后执行。
- 不提交 `build-debug/` 或用户现有 `Docs/db4ai/validation/`。

---

### Task 1: 可测试的明文凭据策略

**Files:**
- Modify: `sql/ai/ai_model_registry.h`
- Modify: `sql/ai/ai_model_registry.cc`
- Modify: `unittest/gunit/ai_model_registry-t.cc`

**Interfaces:**
- Consumes: `Secure_string`, `Ai_error`, credential kind string and plaintext BLOB value.
- Produces: a private resolver helper accepting `allow_plaintext_dev`, kind, value and `Secure_string *`; a test-only wrapper exposes this policy without system-table I/O.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(AiCredentialResolverTest, PlaintextDevRequiresDebugPolicy) {
  Secure_string credential;
  EXPECT_EQ(Ai_error::k_credential_unavailable,
            Ai_credential_resolver::ReadPlaintextDevForTest(
                false, "PLAINTEXT_DEV", "unit-token", &credential));
  EXPECT_TRUE(credential.empty());
}

TEST(AiCredentialResolverTest, PlaintextDevReturnsOnlyNonemptyValue) {
  Secure_string credential;
  EXPECT_EQ(Ai_error::k_ok,
            Ai_credential_resolver::ReadPlaintextDevForTest(
                true, "PLAINTEXT_DEV", "unit-token", &credential));
  EXPECT_EQ("unit-token", credential.view());
}
```

- [ ] **Step 2: Run test to verify RED**

Run: `cmake --build build-debug --target ai_model_registry-t --parallel 8 && build-debug/runtime_output_directory/ai_model_registry-t --gtest_filter=AiCredentialResolverTest.*`

Expected: compilation fails because `ReadPlaintextDevForTest` does not exist.

- [ ] **Step 3: Write minimal implementation**

Add a private helper returning `k_credential_unavailable` unless its boolean is true, kind equals `PLAINTEXT_DEV`, output is non-null and value is nonempty. On success call `Secure_string::Assign`. Expose `ReadPlaintextDevForTest` only under `EXTRA_CODE_FOR_UNIT_TESTING`; it delegates to the helper and creates no network or table dependency.

- [ ] **Step 4: Run test to verify GREEN**

Run: `cmake --build build-debug --target ai_model_registry-t --parallel 8 && build-debug/runtime_output_directory/ai_model_registry-t --gtest_filter=AiCredentialResolverTest.*`

Expected: both new cases pass.

- [ ] **Step 5: Commit**

```bash
git add sql/ai/ai_model_registry.h sql/ai/ai_model_registry.cc unittest/gunit/ai_model_registry-t.cc
git commit -m "feat: gate DB4AI plaintext dev credentials"
```

### Task 2: Debug system-table credential lookup

**Files:**
- Modify: `sql/ai/ai_model_registry.h`
- Modify: `sql/ai/ai_model_registry.cc`
- Modify: `sql/ai/ai_runtime_server.cc`
- Modify: `unittest/gunit/ai_model_registry-t.cc`

**Interfaces:**
- Consumes: `THD *`, immutable `Ai_resolved_model {config_id, config_version, credential_kind}`, `mysql.alisql_ai_model_config` and `Secure_string`.
- Produces: `Ai_credential_resolver::ReadSecret(THD *, const Ai_resolved_model &, Secure_string *)`, supporting keyring `SECRET_REF` and Debug-only `PLAINTEXT_DEV` without placing plaintext in a resolved model.

- [ ] **Step 1: Write the failing compile contract**

```cpp
TEST(AiModelRegistryTest, ResolvedModelCarriesOnlyCredentialMetadata) {
  Ai_resolved_model model;
  model.config_id = 9;
  model.config_version = 3;
  model.credential_kind = "PLAINTEXT_DEV";
  EXPECT_EQ(9U, model.config_id);
  EXPECT_EQ(3U, model.config_version);
}
```

Add a temporary local `model.api_key_plaintext` access while preparing the test and confirm compilation rejects it; remove that temporary access before committing. The committed test only verifies the non-secret projection.

- [ ] **Step 2: Implement exact-row lookup**

Change `ReadSecret` to receive `THD *`. For `PLAINTEXT_DEV`, compile the system-table path only when `NDEBUG` is not defined. Use `System_table_access` to scan `mysql.alisql_ai_model_config`; accept only a row whose config id and version equal the resolved values, is active, has kind `PLAINTEXT_DEV`, and has non-null/nonempty field 14 (`api_key_plaintext`). Pass that BLOB to Task 1's helper. Under `NDEBUG`, return `k_credential_unavailable` before table access. Preserve exact existing keyring behavior for `SECRET_REF`.

- [ ] **Step 3: Update runtime call sites**

```cpp
const Ai_error credential_error =
    credential_resolver.ReadSecret(thd, model, &credential);
```

Apply this to both `Ai_runtime::Embed` and `Ai_runtime::Analyze`. Do not copy credential into `Ai_resolved_model` or `Ai_canonical_request`; construct the adapter only from `credential.view()` after a successful lookup.

- [ ] **Step 4: Verify server link and registry regression**

Run: `cmake --build build-debug --target mysqld ai_model_registry-t --parallel 8 && build-debug/runtime_output_directory/ai_model_registry-t`

Expected: exit code 0; the server target links system-table code and the unit target remains offline.

- [ ] **Step 5: Commit**

```bash
git add sql/ai/ai_model_registry.h sql/ai/ai_model_registry.cc sql/ai/ai_runtime_server.cc unittest/gunit/ai_model_registry-t.cc
git commit -m "feat: read DB4AI plaintext dev credentials in debug"
```

### Task 3: Offline contract coverage and operations documentation

**Files:**
- Modify: `mysql-test/suite/rds/t/ai_maas_contract.test`
- Modify: `mysql-test/suite/rds/r/ai_maas_contract.result`
- Modify: `Docs/db4ai/alisql-maas-p0-operations-and-examples.md`
- Modify: `Docs/db4ai/alisql-maas-p0-validation-status.md`

**Interfaces:**
- Consumes: Debug `PLAINTEXT_DEV` profile configuration and `AI_EMBEDDING`/`AI_ANALYZE` contracts.
- Produces: no-egress regression evidence and operator instructions distinguishing Debug plaintext from production keyring configuration.

- [ ] **Step 1: Add a deterministic MTR case**

Insert a profile row with `credential_kind='PLAINTEXT_DEV'` and a harmless fixture value. Invoke `AI_EMBEDDING` with dimension `1`; the fixed bge-m3 1024-dimension guard must fail before credential resolution or transport. Record only the existing redacted dimension failure.

- [ ] **Step 2: Run MTR**

Run: `cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_contract`

Expected: exit code 0 with no HTTP request and no plaintext token in output.

- [ ] **Step 3: Update docs**

Document: `PLAINTEXT_DEV` is Debug/local-only, production uses `SECRET_REF`, and `api_key_plaintext` must not appear in SQL history, logs, support bundles or commits. Record the two Huawei paths `/v1/embeddings` and `/v2/chat/completions` and logical generation model `huawei/glm-5.2`.

- [ ] **Step 4: Commit**

```bash
git add mysql-test/suite/rds/t/ai_maas_contract.test mysql-test/suite/rds/r/ai_maas_contract.result Docs/db4ai/alisql-maas-p0-operations-and-examples.md Docs/db4ai/alisql-maas-p0-validation-status.md
git commit -m "test: cover DB4AI debug plaintext credentials"
```

### Task 4: Full offline verification and authorized MaaS smoke

**Files:**
- Modify: `Docs/db4ai/alisql-maas-p0-validation-status.md`

**Interfaces:**
- Consumes: Debug mysqld, two temporary `PLAINTEXT_DEV` profiles, explicit `AI_INVOKE`, the repository-external credential file and `scripts/db4ai_maas_smoke.sh`.
- Produces: only test statuses, embedding dimension and generation character count; no key, endpoint, prompt, completion or embedding.

- [ ] **Step 1: Run full offline verification before any cloud request**

```bash
cmake --build build-debug --target mysqld ai_types-t ai_model_registry-t ai_huawei_maas_adapter-t ai_runtime-t ai_vector_codec-t --parallel 8
build-debug/runtime_output_directory/ai_types-t
build-debug/runtime_output_directory/ai_model_registry-t
build-debug/runtime_output_directory/ai_huawei_maas_adapter-t
build-debug/runtime_output_directory/ai_runtime-t
build-debug/runtime_output_directory/ai_vector_codec-t
cd build-debug/mysql-test && ./mtr --suite=rds ai_maas_contract
```

Expected: all commands exit 0 and use only offline fixtures.

- [ ] **Step 2: Configure disposable local profiles**

With a local administrator session against Debug mysqld, insert embedding and generation rows with `credential_kind='PLAINTEXT_DEV'`, using the repository-external credential file, exact Huawei provider model names, 1024 dimension and approved endpoint paths. Bind them to a disposable tenant/account and grant only `AI_INVOKE`. Do not save generated SQL, shell history, MTR files or logs in this repository.

- [ ] **Step 3: Run explicit smoke**

Run `scripts/db4ai_maas_smoke.sh` with a local client command, disposable schema, `DB4AI_EMBEDDING_MODEL=huawei/bge-m3` and `DB4AI_GENERATION_MODEL=huawei/glm-5.2`. Capture only exit status, dimension and completion length.

Expected: dimension is 1024 and completion length is positive. On provider entitlement, model or protocol failure, record only classified outcome and HTTP category.

- [ ] **Step 4: Remove temporary plaintext rows**

Delete the two config rows, bindings and account mapping from the local Debug instance. Verify `SELECT COUNT(*)` for their config ids is zero; do not delete repository files.

- [ ] **Step 5: Record and commit sanitized evidence**

```bash
git add Docs/db4ai/alisql-maas-p0-validation-status.md
git commit -m "test: record DB4AI MaaS debug smoke evidence"
```

## Plan self-review

- Spec coverage: Tasks 1-2 implement Debug-only system-table retrieval without secret propagation; Task 3 preserves offline/no-egress coverage and documents operations; Task 4 runs the authorized external request only after fresh regression evidence and cleans up temporary data.
- Placeholder scan: the plan contains no deferred implementation markers or ambiguous handoffs.
- Type consistency: Task 2 defines the only changed resolver signature and both runtime call sites; later tasks consume no secret-bearing type.
