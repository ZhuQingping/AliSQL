-- Real Huawei MaaS Embedding and Analyze smoke test.
--
-- Usage from the mysql client:
--   mysql> source /absolute/path/to/scripts/db4ai_maas_smoke.sql
--
-- This file makes billable MaaS calls. It never reads, stores, or prints an
-- API key. Before starting mysqld, copy scripts/db4ai_maas_dev.cnf.example to
-- a private local file, replace the placeholder, and chmod that file to 0600.
-- Configure active Profiles through dbms_ai first, and run it as an account
-- that has AI_INVOKE.
--
-- One-time profile registration is an AI_ADMIN action. Do not use direct DML
-- against mysql.taurusdb_ai_model_config. Huawei development validation uses
-- the startup-only rds_api_key parameter, so no API key is passed to SQL:
--
-- CALL dbms_ai.register_model('huawei/bge-m3', 'TEXT_EMBEDDING', 'bge-m3',
--                             'SERVER_PARAMETER', '');
-- CALL dbms_ai.register_model('huawei/glm-5.2', 'TEXT_GENERATION', 'glm-5.2',
--                             'SERVER_PARAMETER', '');
--
-- This is development validation only. A future TaurusDB control-plane
-- encryption and rotation service replaces rds_api_key without changing the
-- SQL functions, model table, or dbms_ai procedure signatures.

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
  @db4ai_generation_model,
  CONCAT('请使用中文简短确认已经收到测试数据。只依据下面证据回答。',
         '\n证据：', JSON_PRETTY(JSON_OBJECT('probe', 'AliSQL real MaaS smoke',
                                               'rows', 1))),
  JSON_OBJECT(
              'max_output_tokens', 32,
              'timeout_ms', 60000));
SELECT CHAR_LENGTH(@db4ai_analysis) AS analyze_length,
       @db4ai_analysis AS analysis_result;

SELECT IF(VECTOR_DIM(@db4ai_embedding) = @db4ai_expected_embedding_dimension
              AND CHAR_LENGTH(@db4ai_analysis) > 0,
          'PASS', 'FAIL') AS db4ai_smoke_result;
