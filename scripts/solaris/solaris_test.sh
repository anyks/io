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
  "${SOLARIS_CTEST}" <<'REMOTE_SCRIPT'
set -euo pipefail
SOLARIS_DIR="$1"; SOLARIS_BUILD="$2"; SOLARIS_CTEST="$3"
cd "$SOLARIS_DIR"
LOG="$SOLARIS_BUILD/ctest_last.log"
echo "[remote] Running ctest in $SOLARIS_BUILD (full verbose). Log: $LOG"
set +e
"$SOLARIS_CTEST" --test-dir "$SOLARIS_BUILD" --output-on-failure -j2 -VV 2>&1 | tee "$LOG"
CTEST_RC=${PIPESTATUS[0]}
set -e
echo "[remote] ctest exit code: $CTEST_RC"
exit "$CTEST_RC"
REMOTE_SCRIPT
