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
--
-- Register the embedding model once through dbms_ai as an AI_ADMIN account;
-- do not modify mysql.taurusdb_ai_model_config directly. For a Debug/development
-- instance, replace <DEVELOPMENT_API_KEY> only in an interactive session:
--
-- CALL dbms_ai.register_model('huawei/bge-m3', 'TEXT_EMBEDDING', 'bge-m3',
--                             'PLAINTEXT_DEV', '<DEVELOPMENT_API_KEY>');
--
-- For a production/Release instance, use an existing keyring/CSMS reference:
--
-- CALL dbms_ai.register_model('huawei/bge-m3', 'TEXT_EMBEDDING', 'bge-m3',
--                             'SECRET_REF', '<EXISTING_SECRET_REFERENCE>');

-- Edit these values for the configured Embedding Profile under test.
SET @db4ai_embedding_model = 'huawei/bge-m3';
SET @db4ai_expected_embedding_dimension = 1024;
SET @db4ai_vector_distance = 'cosine';
SET @db4ai_expected_top_id = 3;

-- VECTOR indexes require READ-COMMITTED in the current AliSQL implementation.
SET SESSION transaction_isolation = 'READ-COMMITTED';

-- Clean up leftovers from a previous interrupted run. No other tables change.
DROP TABLE IF EXISTS db4ai_live_knowledge_base;
DROP TABLE IF EXISTS db4ai_live_embedding_probe;

SELECT 'Embedding model metadata' AS test_step;
SELECT AI_MODEL_INFO(@db4ai_embedding_model) AS model_info;

SELECT 'Direct embedding and vector index' AS test_step;
SET @db4ai_direct_embedding = AI_EMBEDDING(
  'AliSQL real provider embedding smoke probe', @db4ai_embedding_model,
  @db4ai_expected_embedding_dimension);
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
  'doc TEXT NOT NULL, '
  'category VARCHAR(32) NOT NULL DEFAULT ''general'', '
  'vec VECTOR(', @db4ai_expected_embedding_dimension, ') AS '
  '(AI_EMBEDDING(doc, ', QUOTE(@db4ai_embedding_model), ', ',
  @db4ai_expected_embedding_dimension, ')) STORED, '
  'VECTOR INDEX ix_knowledge_vec (vec) m=3 distance=',
  @db4ai_vector_distance, ') ENGINE=InnoDB');
PREPARE db4ai_stmt FROM @db4ai_sql;
EXECUTE db4ai_stmt;
DEALLOCATE PREPARE db4ai_stmt;

INSERT INTO db4ai_live_knowledge_base (doc) VALUES
  ('TaurusDB 与 MySQL 兼容，并为特定业务负载提供更高吞吐。'),
  ('数据库实例的存储容量可扩展到 128 TB。'),
  ('最多可添加 15 个只读副本，以分担主节点的读取压力。'),
  ('快照可在数秒内完成，并支持按时间点恢复。');

SELECT COUNT(*) INTO @db4ai_stored_dimension_rows
  FROM db4ai_live_knowledge_base
 WHERE VECTOR_DIM(vec) = @db4ai_expected_embedding_dimension;
SELECT id, VECTOR_DIM(vec) AS vector_dimension
  FROM db4ai_live_knowledge_base
 ORDER BY id;

-- Changing the document must regenerate its vector.
UPDATE db4ai_live_knowledge_base
   SET doc = '最多可添加 15 个只读副本，以分担主数据库节点的读取压力。'
 WHERE id = 3;

-- Changing only a management field must not require a new document vector.
SET @db4ai_saved_binlog_row_image = @@SESSION.binlog_row_image;
SET SESSION binlog_row_image = 'MINIMAL';
UPDATE db4ai_live_knowledge_base
   SET category = 'operations'
 WHERE id = 3;
SET SESSION binlog_row_image = @db4ai_saved_binlog_row_image;

SET @db4ai_rag_query = AI_EMBEDDING(
  '如何通过只读副本分担主数据库的读取压力？', @db4ai_embedding_model,
  @db4ai_expected_embedding_dimension);
SELECT id INTO @db4ai_top_id
  FROM db4ai_live_knowledge_base FORCE INDEX (ix_knowledge_vec)
 ORDER BY VEC_DISTANCE(vec, @db4ai_rag_query), id
 LIMIT 1;
SELECT id, doc, category,
       ROUND(VEC_DISTANCE(vec, @db4ai_rag_query), 6) AS distance
  FROM db4ai_live_knowledge_base FORCE INDEX (ix_knowledge_vec)
 ORDER BY VEC_DISTANCE(vec, @db4ai_rag_query), id
 LIMIT 2;

SELECT @db4ai_top_id AS rag_top_id,
       @db4ai_stored_dimension_rows AS stored_dimension_rows;
SELECT IF(VECTOR_DIM(@db4ai_direct_embedding) =
              @db4ai_expected_embedding_dimension
              AND @db4ai_stored_dimension_rows = 4
              AND @db4ai_top_id = @db4ai_expected_top_id,
          'PASS', 'FAIL') AS db4ai_embedding_rag_smoke_result;

-- Normal completion cleanup. If execution stops earlier, run these two lines.
DROP TABLE IF EXISTS db4ai_live_knowledge_base;
DROP TABLE IF EXISTS db4ai_live_embedding_probe;
