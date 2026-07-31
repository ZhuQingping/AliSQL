#!/usr/bin/env bash
# Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved.
#
# Explicit opt-in real-provider embedding smoke check. This script never reads,
# accepts, or prints an API key. The database server must already contain an
# active model profile, its protected credential, a tenant binding, and grants
# for the connecting database account. It is not called by MTR or CI.

set -euo pipefail

: "${DB4AI_MYSQL:?Set DB4AI_MYSQL to the mysql client path}"
: "${DB4AI_DATABASE:?Set DB4AI_DATABASE to the target application schema}"
: "${DB4AI_PROVIDER_LABEL:?Set DB4AI_PROVIDER_LABEL (for example huawei, bailian, or volcengine-ark)}"
: "${DB4AI_EMBEDDING_MODEL:?Set DB4AI_EMBEDDING_MODEL to an active logical model name}"
: "${DB4AI_EXPECTED_EMBEDDING_DIMENSION:?Set DB4AI_EXPECTED_EMBEDDING_DIMENSION to the expected returned dimension}"

case "${DB4AI_DATABASE}" in
  *[!A-Za-z0-9_]*|'')
    echo "DB4AI_DATABASE must be a simple schema identifier" >&2
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

case "${DB4AI_PROVIDER_LABEL}" in
  *[!A-Za-z0-9_.-]*|'')
    echo "DB4AI_PROVIDER_LABEL contains unsupported characters" >&2
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

probe_output="$("${DB4AI_MYSQL}" "${mysql_args[@]}" "${DB4AI_DATABASE}" <<SQL
SET @provider_label = '${DB4AI_PROVIDER_LABEL}';
SET @model_name = '${DB4AI_EMBEDDING_MODEL}';
SET @expected_dimension = ${DB4AI_EXPECTED_EMBEDDING_DIMENSION};

SELECT CONCAT('provider=', @provider_label, ', model=', @model_name);
SELECT JSON_UNQUOTE(JSON_EXTRACT(AI_MODEL_INFO(@model_name), '$.model_name')),
       JSON_EXTRACT(AI_MODEL_INFO(@model_name), '$.config_id'),
       JSON_EXTRACT(AI_MODEL_INFO(@model_name), '$.dimension');
SET @embedding = AI_EMBEDDING(
  'AliSQL real provider embedding smoke probe', @model_name, @expected_dimension);
SET @returned_dimension = VECTOR_DIM(@embedding);
SET @self_distance = VEC_DISTANCE_COSINE(@embedding, @embedding);
SET @audit = AI_AUDIT_INFO(100);
SET @audit_is_sanitized =
  JSON_CONTAINS_PATH(@audit, 'one', '$[0].endpoint', '$[0].credential_ref', '$[0].input') = 0;
SELECT CONCAT('returned_dimension=', @returned_dimension,
              ', self_distance=', @self_distance,
              ', audit_sanitized=', @audit_is_sanitized);
SELECT IF(@returned_dimension = @expected_dimension
              AND ABS(@self_distance) < 0.000001
              AND @audit_is_sanitized,
          'PASS', 'FAIL');
SQL
)"

printf '%s\n' "${probe_output}"
if [[ "$(printf '%s\n' "${probe_output}" | tail -n 1)" != "PASS" ]]; then
  echo "DB4AI embedding smoke checks failed" >&2
  exit 1
fi
