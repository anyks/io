#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"/../..

if [[ -f scripts/solaris/.env ]]; then
  source scripts/solaris/.env
else
  source scripts/solaris/.env.example
fi

ssh -p "${SOLARIS_SSH_PORT}" ${SOLARIS_SSH_OPTS} "${SOLARIS_SSH}" bash -s -- \
  "${SOLARIS_DIR}" \
  "${SOLARIS_BUILD}" \
  "${SOLARIS_CTEST}" <<'REMOTE_SCRIPT'
set -euo pipefail
SOLARIS_DIR="$1"; SOLARIS_BUILD="$2"; SOLARIS_CTEST="$3"
cd "$SOLARIS_DIR"
"$SOLARIS_CTEST" --test-dir "$SOLARIS_BUILD" -N | sed -n '1,200p'
REMOTE_SCRIPT
