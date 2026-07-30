#!/usr/bin/env bash
# Copyright (c) 2026, Alibaba and/or its affiliates. All rights reserved.
#
# Explicit opt-in Huawei MaaS smoke check. This script never accepts or prints
# an API key. The selected logical models must already be bound to the target
# tenant and their SECRET_REF values must be available through the server
# keyring. It is intentionally not called by MTR or CI.

set -euo pipefail

: "${DB4AI_MYSQL:?Set DB4AI_MYSQL to a mysql client command/path}"
: "${DB4AI_DATABASE:?Set DB4AI_DATABASE to the target application schema}"

embedding_model="${DB4AI_EMBEDDING_MODEL:-huawei/bge-m3}"
generation_model="${DB4AI_GENERATION_MODEL:-huawei/glm-5.2}"

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

# Output is deliberately limited to a dimension and a character count.
"${DB4AI_MYSQL}" --batch --skip-column-names "${DB4AI_DATABASE}" <<SQL
SELECT VECTOR_DIM(AI_EMBEDDING('db4ai smoke probe', '${embedding_model}', 1024));
SELECT CHAR_LENGTH(AI_ANALYZE(
  'Respond with a short acknowledgement.', 'db4ai smoke probe',
  JSON_OBJECT('model_name', '${generation_model}', 'mode', 'summarize',
              'max_output_tokens', 32, 'timeout_ms', 15000)));
SQL
