-- Real Huawei MaaS Embedding and STORED RAG smoke test.
--
-- Usage from the mysql client:
--   mysql> source /absolute/path/to/scripts/db4ai_maas_real_embedding_rag_smoke.sql
-- First select a dedicated validation schema, for example:
--   mysql> CREATE DATABASE IF NOT EXISTS db4ai_validation;
--   mysql> USE db4ai_validation;
-- VECTOR columns and indexes require the following instance/session setup:
--   mysql> SET GLOBAL vidx_disabled = OFF;
--
-- This file makes billable MaaS calls. It creates and drops only
-- db4ai_live_embedding_probe and db4ai_live_knowledge_base in the current
-- schema. The caller needs AI_INVOKE, CREATE, DROP, and permission to set
-- SESSION binlog_row_image. It never reads, stores, or prints an API key.
-- An administrator must first set GLOBAL rds_ai_maas = ON; the default is OFF.
--
-- Register the embedding model once through dbms_ai as an AI_ADMIN account;
-- do not modify mysql.ai_model_config directly. The Huawei API key is managed
-- separately by the TaurusDB control plane through protected rds_api_key:
--
-- CALL dbms_ai.register_model('huawei/bge-m3', 'TEXT_EMBEDDING', 'huawei',
--   'bge-m3', 'https://api.modelarts-maas.com/v1/embeddings', 1024, '{}');
-- CALL dbms_ai.register_model('huawei/glm-5.2', 'TEXT_GENERATION', 'huawei',
--   'glm-5.2', 'https://api.modelarts-maas.com/v2/chat/completions', 0, '{}');

-- Edit these values for the configured Embedding Profile under test.
SET @db4ai_embedding_model = 'huawei/bge-m3';
SET @db4ai_generation_model = 'huawei/glm-5.2';
SET @db4ai_expected_embedding_dimension = 1024;
SET @db4ai_vector_distance = 'cosine';

-- VECTOR indexes require READ-COMMITTED in the current AliSQL implementation.
SET SESSION transaction_isolation = 'READ-COMMITTED';

-- Clean up leftovers from a previous interrupted run. No other tables change.
DROP TABLE IF EXISTS db4ai_live_knowledge_base;
DROP TABLE IF EXISTS db4ai_live_embedding_probe;

SELECT 'Embedding model metadata' AS test_step;
SELECT AI_MODEL_INFO(@db4ai_embedding_model) AS model_info;

SELECT 'Direct embedding and vector index' AS test_step;
SET @db4ai_direct_embedding = AI_EMBEDDING(
  @db4ai_embedding_model, 'AliSQL real provider embedding smoke probe',
  JSON_OBJECT('dimension', @db4ai_expected_embedding_dimension));
SELECT VECTOR_DIM(@db4ai_direct_embedding) AS direct_dimension,
       ROUND(VEC_DISTANCE_COSINE(@db4ai_direct_embedding,
                                 @db4ai_direct_embedding), 6)
         AS cosine_self_distance,
       ROUND(VEC_DISTANCE_EUCLIDEAN(@db4ai_direct_embedding,
                                    @db4ai_direct_embedding), 6)
         AS euclidean_self_distance;

SET @db4ai_sql = CONCAT(
  'CREATE TABLE db4ai_live_embedding_probe ('
  'id INT NOT NULL PRIMARY KEY, '
  'embedding VECTOR(', @db4ai_expected_embedding_dimension, ') NOT NULL, '
  'VECTOR INDEX ix_embedding (embedding) m=3 distance=',
  @db4ai_vector_distance, ') ENGINE=InnoDB');
PREPARE db4ai_stmt FROM @db4ai_sql;
EXECUTE db4ai_stmt;
DEALLOCATE PREPARE db4ai_stmt;

INSERT INTO db4ai_live_embedding_probe VALUES (1, @db4ai_direct_embedding);
SELECT id,
       ROUND(VEC_DISTANCE(embedding, @db4ai_direct_embedding), 6)
         AS indexed_distance
  FROM db4ai_live_embedding_probe FORCE INDEX (ix_embedding)
 ORDER BY VEC_DISTANCE(embedding, @db4ai_direct_embedding), id;

SELECT 'STORED generated vector and RAG retrieval' AS test_step;
SET @db4ai_sql = CONCAT(
  'CREATE TABLE db4ai_live_knowledge_base ('
  'id INT AUTO_INCREMENT PRIMARY KEY, '
  'tenant_id BIGINT UNSIGNED NOT NULL, '
  'source_id VARCHAR(64) NOT NULL, '
  'chunk_id INT UNSIGNED NOT NULL, '
  'doc TEXT NOT NULL, '
  'category VARCHAR(32) NOT NULL DEFAULT ''general'', '
  'access_label VARCHAR(32) NOT NULL DEFAULT ''support'', '
  'vec VECTOR(', @db4ai_expected_embedding_dimension, ') AS '
  '(AI_EMBEDDING(', QUOTE(@db4ai_embedding_model), ', doc, JSON_OBJECT(''dimension'', ',
  @db4ai_expected_embedding_dimension, '))) STORED, '
  'VECTOR INDEX ix_knowledge_vec (vec) m=3 distance=',
  @db4ai_vector_distance, ') ENGINE=InnoDB');
PREPARE db4ai_stmt FROM @db4ai_sql;
EXECUTE db4ai_stmt;
DEALLOCATE PREPARE db4ai_stmt;

INSERT INTO db4ai_live_knowledge_base
  (tenant_id, source_id, chunk_id, doc, category, access_label) VALUES
  (42, 'taurusdb-guide', 1,
   'TaurusDB 与 MySQL 兼容，并为特定业务负载提供更高吞吐。', 'product', 'support'),
  (42, 'taurusdb-guide', 2,
   '数据库实例的存储容量可扩展到 128 TB。', 'product', 'support'),
  (42, 'taurusdb-guide', 3,
   '最多可添加 15 个只读副本，以分担主节点的读取压力。', 'operations', 'support'),
  (42, 'taurusdb-guide', 4,
   '快照可在数秒内完成，并支持按时间点恢复。', 'backup', 'support'),
  (77, 'private-guide', 1,
   '租户 77 的私有运维过程。', 'operations', 'private');

SELECT COUNT(*) INTO @db4ai_stored_dimension_rows
  FROM db4ai_live_knowledge_base
 WHERE tenant_id = 42
   AND VECTOR_DIM(vec) = @db4ai_expected_embedding_dimension;
SELECT tenant_id, source_id, chunk_id, VECTOR_DIM(vec) AS vector_dimension
  FROM db4ai_live_knowledge_base
 ORDER BY tenant_id, source_id, chunk_id;

-- Changing the document must regenerate its vector.
UPDATE db4ai_live_knowledge_base
   SET doc = '最多可添加 15 个只读副本，以分担主数据库节点的读取压力。'
 WHERE tenant_id = 42 AND source_id = 'taurusdb-guide' AND chunk_id = 3;

-- Changing only a management field must not require a new document vector.
SET @db4ai_saved_binlog_row_image = @@SESSION.binlog_row_image;
SET SESSION binlog_row_image = 'MINIMAL';
UPDATE db4ai_live_knowledge_base
   SET category = 'verified'
 WHERE tenant_id = 42 AND source_id = 'taurusdb-guide' AND chunk_id = 3;
SET SESSION binlog_row_image = @db4ai_saved_binlog_row_image;

SET @db4ai_rag_query = AI_EMBEDDING(
  @db4ai_embedding_model, '如何通过只读副本分担主数据库的读取压力？',
  JSON_OBJECT('dimension', @db4ai_expected_embedding_dimension));
SELECT id INTO @db4ai_top_id
  FROM db4ai_live_knowledge_base FORCE INDEX (ix_knowledge_vec)
 WHERE tenant_id = 42
   AND access_label = 'support'
 ORDER BY VEC_DISTANCE(vec, @db4ai_rag_query), id
 LIMIT 1;
SELECT tenant_id, source_id, chunk_id, doc, category,
       ROUND(VEC_DISTANCE(vec, @db4ai_rag_query), 6) AS distance
  FROM db4ai_live_knowledge_base FORCE INDEX (ix_knowledge_vec)
 WHERE tenant_id = 42
   AND access_label = 'support'
 ORDER BY VEC_DISTANCE(vec, @db4ai_rag_query), id
 LIMIT 2;

-- Build RAG evidence only from the SQL-filtered, vector-retrieved rows.
SELECT JSON_OBJECT(
         'question', '如何通过只读副本分担主数据库的读取压力？',
         'sources', JSON_ARRAYAGG(JSON_OBJECT('source_id', source_id,
                                               'chunk_id', chunk_id,
                                               'content', doc)))
  INTO @db4ai_rag_input
  FROM (
    SELECT source_id, chunk_id, doc
      FROM db4ai_live_knowledge_base FORCE INDEX (ix_knowledge_vec)
     WHERE tenant_id = 42
       AND access_label = 'support'
     ORDER BY VEC_DISTANCE(vec, @db4ai_rag_query), id
     LIMIT 2
  ) AS db4ai_permitted_rag_sources;
SELECT JSON_ARRAYAGG(JSON_OBJECT('source_id', source_id,
                                 'chunk_id', chunk_id))
  INTO @db4ai_expected_rag_sources
  FROM (
    SELECT source_id, chunk_id
      FROM db4ai_live_knowledge_base FORCE INDEX (ix_knowledge_vec)
     WHERE tenant_id = 42
       AND access_label = 'support'
     ORDER BY VEC_DISTANCE(vec, @db4ai_rag_query), id
     LIMIT 2
  ) AS db4ai_permitted_rag_source_ids;
SET @db4ai_rag_result = AI_ANALYZE(
  @db4ai_generation_model,
  CONCAT('仅使用下面已授权的数据库来源回答问题，并用中文简洁作答。',
         '资料不足时明确说明。\n\n证据：\n', JSON_PRETTY(@db4ai_rag_input)),
  JSON_OBJECT(
              'max_output_tokens', 25600,
              'timeout_ms', 60000));
SET @db4ai_rag_answer = @db4ai_rag_result;

SELECT @db4ai_top_id AS rag_top_id,
       @db4ai_stored_dimension_rows AS stored_dimension_rows,
       @db4ai_rag_answer AS rag_answer,
       @db4ai_expected_rag_sources AS rag_sources;
SELECT IF(VECTOR_DIM(@db4ai_direct_embedding) =
              @db4ai_expected_embedding_dimension
              AND @db4ai_stored_dimension_rows = 4
              AND @db4ai_rag_answer IS NOT NULL
              AND CHAR_LENGTH(@db4ai_rag_answer) > 0
              AND JSON_LENGTH(@db4ai_expected_rag_sources) > 0,
          'PASS', 'FAIL') AS db4ai_embedding_rag_smoke_result;

-- Normal completion cleanup. If execution stops earlier, run these two lines.
DROP TABLE IF EXISTS db4ai_live_knowledge_base;
DROP TABLE IF EXISTS db4ai_live_embedding_probe;
