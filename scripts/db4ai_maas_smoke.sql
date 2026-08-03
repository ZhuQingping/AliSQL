-- Real Huawei MaaS Embedding and Analyze smoke test.
--
-- Usage from the mysql client:
--   mysql> source /absolute/path/to/scripts/db4ai_maas_smoke.sql
--
-- This file makes billable MaaS calls. It never reads, stores, or prints an
-- API key. Configure active Profiles and server-side credentials first, and
-- run it as an account that has AI_INVOKE.

-- Edit these three values for the configured Profiles under test.
SET @db4ai_embedding_model = 'huawei/bge-m3';
SET @db4ai_generation_model = 'huawei/glm-5.2';
SET @db4ai_expected_embedding_dimension = 1024;

SELECT 'Embedding model metadata' AS test_step;
SELECT AI_MODEL_INFO(@db4ai_embedding_model) AS model_info;

SELECT 'Embedding' AS test_step;
SET @db4ai_embedding = AI_EMBEDDING(
  'db4ai real MaaS smoke probe', @db4ai_embedding_model,
  @db4ai_expected_embedding_dimension);
SELECT VECTOR_DIM(@db4ai_embedding) AS embedding_dimension,
       ROUND(VEC_DISTANCE_COSINE(@db4ai_embedding, @db4ai_embedding), 6)
         AS cosine_self_distance,
       ROUND(VEC_DISTANCE_EUCLIDEAN(@db4ai_embedding, @db4ai_embedding), 6)
         AS euclidean_self_distance;

SELECT 'Analyze' AS test_step;
SET @db4ai_analysis = AI_ANALYZE(
  '请使用中文简短确认已经收到测试数据。',
  JSON_OBJECT('probe', 'AliSQL real MaaS smoke', 'rows', 1),
  JSON_OBJECT('model_name', @db4ai_generation_model,
              'mode', 'summarize',
              'max_output_tokens', 32,
              'timeout_ms', 15000));
SELECT CHAR_LENGTH(@db4ai_analysis) AS analyze_length,
       @db4ai_analysis AS analysis_result;

SELECT IF(VECTOR_DIM(@db4ai_embedding) = @db4ai_expected_embedding_dimension
              AND CHAR_LENGTH(@db4ai_analysis) > 0,
          'PASS', 'FAIL') AS db4ai_smoke_result;
