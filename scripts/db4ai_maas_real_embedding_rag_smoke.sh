#!/usr/bin/env bash
# Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved.
#
# Explicit opt-in, billable Huawei MaaS validation for the SQL paths covered by
# ai_maas_embedding.test and the STORED-generated-column case in
# ai_maas_rag.test. It never reads, accepts, prints, or writes an API key.
# The database must already contain the active model Profile and AI_INVOKE
# grant, and mysqld must have started from the private 0600 configuration
# described in scripts/db4ai_maas_dev.cnf.example.

set -euo pipefail

: "${DB4AI_RUN_REAL_MAAS:?Set DB4AI_RUN_REAL_MAAS=1 to authorize billable MaaS calls}"
: "${DB4AI_MYSQL:?Set DB4AI_MYSQL to the mysql client path}"
: "${DB4AI_DATABASE:?Set DB4AI_DATABASE to the target application schema}"
: "${DB4AI_EMBEDDING_MODEL:?Set DB4AI_EMBEDDING_MODEL to an active logical model name}"
: "${DB4AI_EXPECTED_EMBEDDING_DIMENSION:?Set DB4AI_EXPECTED_EMBEDDING_DIMENSION to the expected returned dimension}"

if [[ "${DB4AI_RUN_REAL_MAAS}" != "1" ]]; then
  echo "DB4AI_RUN_REAL_MAAS must be exactly 1" >&2
  exit 2
fi

case "${DB4AI_DATABASE}" in
  *[!A-Za-z0-9_]*|'')
    echo "DB4AI_DATABASE must be a simple schema identifier" >&2
    exit 2
    ;;
esac

case "${DB4AI_EMBEDDING_MODEL}" in
  *[!A-Za-z0-9_./:-]*|'')
    echo "DB4AI_EMBEDDING_MODEL contains unsupported characters" >&2
    exit 2
    ;;
esac

case "${DB4AI_EXPECTED_EMBEDDING_DIMENSION}" in
  *[!0-9]*|0|'')
    echo "DB4AI_EXPECTED_EMBEDDING_DIMENSION must be a positive integer" >&2
    exit 2
    ;;
esac

distance="${DB4AI_VECTOR_DISTANCE:-cosine}"
case "${distance}" in
  cosine|euclidean) ;;
  *)
    echo "DB4AI_VECTOR_DISTANCE must be cosine or euclidean" >&2
    exit 2
    ;;
esac

expected_top_id="${DB4AI_RAG_EXPECTED_TOP_ID:-3}"
case "${expected_top_id}" in
  *[!0-9]*|0|'')
    echo "DB4AI_RAG_EXPECTED_TOP_ID must be a positive integer" >&2
    exit 2
    ;;
esac

mysql_user="${DB4AI_MYSQL_USER:-root}"
case "${mysql_user}" in
  *[!A-Za-z0-9_]*|'')
    echo "DB4AI_MYSQL_USER contains unsupported characters" >&2
    exit 2
    ;;
esac

mysql_args=(--batch --skip-column-names "--user=${mysql_user}")
if [[ -n "${DB4AI_MYSQL_SOCKET:-}" ]]; then
  mysql_args+=("--socket=${DB4AI_MYSQL_SOCKET}")
else
  mysql_host="${DB4AI_MYSQL_HOST:-127.0.0.1}"
  mysql_port="${DB4AI_MYSQL_PORT:-3306}"
  case "${mysql_host}" in
    *[!A-Za-z0-9.-]*|'')
      echo "DB4AI_MYSQL_HOST contains unsupported characters" >&2
      exit 2
      ;;
  esac
  case "${mysql_port}" in
    *[!0-9]*|0|'')
      echo "DB4AI_MYSQL_PORT must be a positive integer" >&2
      exit 2
      ;;
  esac
  mysql_args+=("--host=${mysql_host}" "--port=${mysql_port}")
fi

embedding_table="db4ai_live_embedding_probe"
knowledge_table="db4ai_live_knowledge_base"

run_mysql() {
  "${DB4AI_MYSQL}" "${mysql_args[@]}" "${DB4AI_DATABASE}" "$@"
}

cleanup() {
  run_mysql --execute "DROP TABLE IF EXISTS ${knowledge_table}; DROP TABLE IF EXISTS ${embedding_table};" \
    >/dev/null 2>&1 || true
}
trap cleanup EXIT

# Remove only the two names reserved by this script. The EXIT trap repeats this
# cleanup if a provider call, assertion, or client connection fails.
cleanup

probe_output="$(run_mysql <<SQL
SET transaction_isolation = 'READ-COMMITTED';
SET @model_name = '${DB4AI_EMBEDDING_MODEL}';
SET @expected_dimension = ${DB4AI_EXPECTED_EMBEDDING_DIMENSION};

SET @direct_embedding = AI_EMBEDDING(
  'AliSQL real provider embedding smoke probe', @model_name, @expected_dimension);
SELECT VECTOR_DIM(@direct_embedding) AS direct_dimension,
       ROUND(VEC_DISTANCE_COSINE(@direct_embedding, @direct_embedding), 6)
         AS cosine_self_distance,
       ROUND(VEC_DISTANCE_EUCLIDEAN(@direct_embedding, @direct_embedding), 6)
         AS euclidean_self_distance;

CREATE TABLE ${embedding_table} (
  id INT NOT NULL PRIMARY KEY,
  embedding VECTOR(${DB4AI_EXPECTED_EMBEDDING_DIMENSION}) NOT NULL,
  VECTOR INDEX ix_embedding (embedding) m=3 distance=${distance}
) ENGINE=InnoDB;
INSERT INTO ${embedding_table} VALUES (1, @direct_embedding);
SELECT id, ROUND(VEC_DISTANCE(embedding, @direct_embedding), 6) AS indexed_distance
  FROM ${embedding_table} FORCE INDEX (ix_embedding)
 ORDER BY VEC_DISTANCE(embedding, @direct_embedding), id;

CREATE TABLE ${knowledge_table} (
  id INT AUTO_INCREMENT PRIMARY KEY,
  doc TEXT NOT NULL,
  category VARCHAR(32) NOT NULL DEFAULT 'general',
  vec VECTOR(${DB4AI_EXPECTED_EMBEDDING_DIMENSION})
    AS (AI_EMBEDDING(doc, '${DB4AI_EMBEDDING_MODEL}',
                     ${DB4AI_EXPECTED_EMBEDDING_DIMENSION})) STORED,
  VECTOR INDEX ix_knowledge_vec (vec) m=3 distance=${distance}
) ENGINE=InnoDB;

INSERT INTO ${knowledge_table} (doc) VALUES
  ('TaurusDB 与 MySQL 兼容，并为特定业务负载提供更高吞吐。'),
  ('数据库实例的存储容量可扩展到 128 TB。'),
  ('最多可添加 15 个只读副本，以分担主节点的读取压力。'),
  ('快照可在数秒内完成，并支持按时间点恢复。');
SELECT COUNT(*) INTO @stored_dimension_rows
  FROM ${knowledge_table}
 WHERE VECTOR_DIM(vec) = @expected_dimension;

UPDATE ${knowledge_table}
   SET doc = '最多可添加 15 个只读副本，以分担主数据库节点的读取压力。'
 WHERE id = 3;

SET @saved_binlog_row_image = @@SESSION.binlog_row_image;
SET SESSION binlog_row_image = 'MINIMAL';
UPDATE ${knowledge_table} SET category = 'operations' WHERE id = 3;
SET SESSION binlog_row_image = @saved_binlog_row_image;

SET @rag_query = AI_EMBEDDING(
  '如何通过只读副本分担主数据库的读取压力？', @model_name, @expected_dimension);
SELECT id INTO @top_id
  FROM ${knowledge_table} FORCE INDEX (ix_knowledge_vec)
 ORDER BY VEC_DISTANCE(vec, @rag_query), id
 LIMIT 1;
SELECT @top_id AS rag_top_id,
       @stored_dimension_rows AS stored_dimension_rows;

SELECT IF(VECTOR_DIM(@direct_embedding) = @expected_dimension
              AND @stored_dimension_rows = 4
              AND @top_id = ${expected_top_id},
          'PASS', 'FAIL');
SQL
)"

printf '%s\n' "${probe_output}"
if [[ "$(printf '%s\n' "${probe_output}" | tail -n 1)" != "PASS" ]]; then
  echo "DB4AI real embedding/RAG smoke checks failed" >&2
  exit 1
fi
