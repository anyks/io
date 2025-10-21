#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"/../..

if [[ -f scripts/solaris/.env ]]; then
  source scripts/solaris/.env
else
  source scripts/solaris/.env.example
fi

ssh -tt -p "${SOLARIS_SSH_PORT}" ${SOLARIS_SSH_OPTS} "${SOLARIS_SSH}" bash -s -- \
  "${SOLARIS_DIR}" \
  "${SOLARIS_BUILD}" \
  "${SOLARIS_CMAKE}" \
  "${SOLARIS_JOBS}" <<'REMOTE_SCRIPT'
set -euo pipefail
SOLARIS_DIR="$1"; SOLARIS_BUILD="$2"; SOLARIS_CMAKE="$3"; SOLARIS_JOBS="$4"
cd "$SOLARIS_DIR"
"$SOLARIS_CMAKE" --build "$SOLARIS_BUILD" -j"$SOLARIS_JOBS"
REMOTE_SCRIPT

echo "Built on ${SOLARIS_SSH} in ${SOLARIS_DIR}/${SOLARIS_BUILD}"
