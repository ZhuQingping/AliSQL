#!/usr/bin/env bash
# Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved.
#
# Explicit opt-in Huawei MaaS smoke check. This script never accepts or prints
# an API key. The selected logical models must be ACTIVE instance Profiles and
# their server-side credentials must be available. It is
# intentionally not called by MTR or CI.

set -euo pipefail

: "${DB4AI_RUN_REAL_MAAS:?Set DB4AI_RUN_REAL_MAAS=1 to authorize billable MaaS calls}"
: "${DB4AI_MYSQL:?Set DB4AI_MYSQL to a mysql client command/path}"
: "${DB4AI_DATABASE:?Set DB4AI_DATABASE to the target application schema}"

embedding_model="${DB4AI_EMBEDDING_MODEL:-huawei/bge-m3}"
generation_model="${DB4AI_GENERATION_MODEL:-huawei/glm-5.2}"
expected_dimension="${DB4AI_EXPECTED_EMBEDDING_DIMENSION:-1024}"

if [[ "${DB4AI_RUN_REAL_MAAS}" != "1" ]]; then
  echo "DB4AI_RUN_REAL_MAAS must be exactly 1" >&2
  exit 2
fi

case "${embedding_model}:${generation_model}" in
  *[!A-Za-z0-9_./:-]*)
    echo "DB4AI model names contain unsupported characters" >&2
    exit 2
    ;;
esac

case "${DB4AI_DATABASE}" in
  *[!A-Za-z0-9_]*|'')
    echo "DB4AI_DATABASE must be a simple schema identifier" >&2
    exit 2
    ;;
esac

case "${expected_dimension}" in
  *[!0-9]*|0|'')
    echo "DB4AI_EXPECTED_EMBEDDING_DIMENSION must be a positive integer" >&2
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

# Output is deliberately limited to a dimension and a character count.
probe_output="$("${DB4AI_MYSQL}" "${mysql_args[@]}" "${DB4AI_DATABASE}" <<SQL
SET @embedding = AI_EMBEDDING('${embedding_model}', 'db4ai smoke probe',
                              JSON_OBJECT('dimension', ${expected_dimension}));
SELECT VECTOR_DIM(@embedding) AS embedding_dimension;
SET @analysis = AI_ANALYZE(
  '${generation_model}', 'Respond with a short acknowledgement. Evidence: db4ai smoke probe.',
  JSON_OBJECT(
              'max_output_tokens', 32, 'timeout_ms', 60000));
SELECT CHAR_LENGTH(@analysis) AS analyze_length;
SELECT IF(VECTOR_DIM(@embedding) = ${expected_dimension}
              AND CHAR_LENGTH(@analysis) > 0,
          'PASS', 'FAIL');
SQL
)"

printf '%s\n' "${probe_output}"
if [[ "$(printf '%s\n' "${probe_output}" | tail -n 1)" != "PASS" ]]; then
  echo "DB4AI MaaS embedding/analyze smoke checks failed" >&2
  exit 1
fi
