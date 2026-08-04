# Task 4: Governed `AI_ANALYZE` RAG and diagnose output

## Interface semantics

- `model_name` is required and non-empty. `return_sources=true` is valid only
  for `mode='rag'` with `output_format='json'`.
- The runtime selects a fixed server-owned system prompt. SQL `task_text` and
  the supplied input are combined into the provider user message, so callers
  cannot replace the system role.
- RAG requires an object with a non-empty `question` and non-empty `sources`.
  Every source requires `source_id`, unsigned `chunk_id`, and non-empty
  `content`. Only `source_id` and `chunk_id` are retained for the response;
  source content is kept only in the in-memory request input.
- Diagnose accepts only a JSON-object evidence collection. Its system prompt
  constrains output to read-only causes, evidence, risks, and recommendations;
  it forbids actions and repair SQL.
- JSON mode returns runtime-created `content`, `model_name`, `config_version`,
  and usage counters, plus RAG source references when requested. It omits the
  provider response, reasoning text, and provider request body.

## Tests

- Red: `./mtr --suite=rds ai_maas_analysis ai_maas_rag` failed before the
  implementation because array diagnose evidence was accepted.
- Green: `cmake --build . --target mysqld ai_runtime-t -j4`,
  `./runtime_output_directory/ai_runtime-t`, and
  `./mtr --suite=rds ai_maas_analysis ai_maas_rag` all passed.
- MTR covers canonical JSON output, non-object diagnose rejection,
  `return_sources` rejection outside RAG JSON mode, RAG source provenance,
  and missing RAG sources.

## Risks

- The P0 RAG contract accepts unsigned numeric `chunk_id` values. Supporting
  string or composite chunk identifiers later would require extending the
  canonical source-reference type and JSON writer deliberately.
- The controlled prompts constrain the provider request but do not replace
  the caller's SQL-side tenant/access filtering; the existing RAG test keeps
  those predicates in SQL.

## Review round 1

- Diagnose now validates final content before either text or canonical JSON is
  returned. A conservative lexical policy rejects executable DML, DDL, and
  administrative repair sequences (including `INSERT INTO`, `UPDATE ... SET`,
  `DELETE FROM`, `ALTER/DROP/CREATE TABLE`, `TRUNCATE/REPAIR/OPTIMIZE TABLE`,
  and `SET GLOBAL`). It records the local `UNSAFE_OUTPUT` audit error and
  returns `k_unsafe_output`; analyze and RAG bypass this validator.
- The debug-only offline fixture emits `ALTER TABLE ...` only for the explicit
  `mtr_fixture_repair_sql` diagnose evidence marker. MTR proves that response
  is rejected while the existing evidence-only diagnose response succeeds.
- Adapter GUnit now compiles after the canonical request rename and asserts
  the serialized fixed system message is separate from the caller-derived
  user message. RAG MTR now compares the complete returned source array and
  rejects `return_sources` in RAG text mode.
- Fresh verification: `cmake --build . --target mysqld ai_runtime-t
  ai_huawei_maas_adapter-t -j4`; `./runtime_output_directory/ai_runtime-t`
  (4/4); `./runtime_output_directory/ai_huawei_maas_adapter-t` (10/10); and
  `./mtr --suite=rds --record ai_maas_analysis ai_maas_rag` (all passed).
- `./mtr --suite=rds ai_maas_contract` also passed, preserving the existing
  non-diagnose text `AI_ANALYZE` behavior.
- The lexical guard intentionally favors safety over prose fidelity. A
  diagnosis that quotes an executable repair statement is rejected rather
  than attempting fragile context-aware SQL interpretation.
