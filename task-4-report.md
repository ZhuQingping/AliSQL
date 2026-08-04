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
