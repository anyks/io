#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"/../..

if [[ -f scripts/solaris/.env ]]; then
  source scripts/solaris/.env
else
  source scripts/solaris/.env.example
fi

ssh -tt -p "${SOLARIS_SSH_PORT}" ${SOLARIS_SSH_OPTS} "${SOLARIS_SSH}" bash -lc "'
set -euo pipefail
cd "${SOLARIS_DIR}"
${SOLARIS_CTEST} --test-dir "${SOLARIS_BUILD}" --output-on-failure -j2
'"
