-- Enterprise manual RAG: production-shaped SQL without credentials or endpoints.
-- Before running, an AI_ADMIN has configured huawei/bge-m3 and huawei/glm-5.2
-- for the authenticated application's tenant and granted AI_INVOKE.

CREATE TABLE product_manual_chunk (
  tenant_id BIGINT UNSIGNED NOT NULL,
  source_id VARCHAR(64) NOT NULL,
  chunk_id INT NOT NULL,
  product_line VARCHAR(32) NOT NULL,
  access_label ENUM('internal', 'support') NOT NULL,
  content TEXT NOT NULL,
  embedding VECTOR(1024) NOT NULL,
  embedding_model VARCHAR(128) NOT NULL,
  embedding_config_version BIGINT UNSIGNED NOT NULL,
  embedding_space_id VARCHAR(128) NOT NULL,
  PRIMARY KEY (tenant_id, source_id, chunk_id),
  KEY ix_tenant_product (tenant_id, product_line, access_label),
  VECTOR INDEX ix_manual_embedding (embedding) m=16 distance=cosine,
  CONSTRAINT ck_embedding_contract CHECK
    (embedding_model = 'huawei/bge-m3' AND
     embedding_config_version = 1 AND
     embedding_space_id = 'huawei/bge-m3/current/1024/cosine')
) ENGINE=InnoDB;

INSERT INTO product_manual_chunk
  (tenant_id, source_id, chunk_id, product_line, access_label, content,
   embedding, embedding_model, embedding_config_version, embedding_space_id)
VALUES
  (42, 'manual-001', 1, 'gateway', 'support',
   'Check power and network indicators before resetting an offline gateway.',
   AI_EMBEDDING(
     'Check power and network indicators before resetting an offline gateway.',
     'huawei/bge-m3', 1024),
   'huawei/bge-m3', 1, 'huawei/bge-m3/current/1024/cosine');

SET @question = 'How should support handle an offline gateway?';
SET @query_embedding = AI_EMBEDDING(@question, 'huawei/bge-m3', 1024);

-- All authorization and corpus-selection predicates stay in SQL.
SELECT source_id, chunk_id, content,
       VEC_DISTANCE_COSINE(embedding, @query_embedding) AS distance
  FROM product_manual_chunk
 WHERE tenant_id = 42
   AND product_line = 'gateway'
   AND access_label = 'support'
   AND embedding_model = 'huawei/bge-m3'
   AND embedding_config_version = 1
   AND embedding_space_id = 'huawei/bge-m3/current/1024/cosine'
 ORDER BY VEC_DISTANCE_COSINE(embedding, @query_embedding)
 LIMIT 8;

-- Construct chunks from the selected rows in application code, retaining the
-- database-generated source_id/chunk_id alongside the generated answer.
SELECT AI_ANALYZE(
  'Answer using only the supplied chunks. State uncertainty when insufficient.',
  JSON_OBJECT('question', @question,
              'chunks', JSON_ARRAY(JSON_OBJECT('source_id', 'manual-001',
                                                'chunk_id', 1,
                                                'content', '...'))),
  JSON_OBJECT('model_name', 'huawei/glm-5.2', 'mode', 'rag',
              'output_format', 'json', 'return_sources', true,
              'max_output_tokens', 400, 'timeout_ms', 15000)) AS answer;
