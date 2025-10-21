#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"/../..

if [[ -f scripts/solaris/.env ]]; then
  source scripts/solaris/.env
else
  source scripts/solaris/.env.example
fi

PATTERN=${1:-}
if [[ -z "$PATTERN" ]]; then
  echo "Usage: $0 <gtest_regex>" >&2
  exit 2
fi

ssh -tt -p "${SOLARIS_SSH_PORT}" ${SOLARIS_SSH_OPTS} "${SOLARIS_SSH}" bash -s -- \
  "${SOLARIS_DIR}" \
  "${SOLARIS_BUILD}" \
  "${SOLARIS_CTEST}" \
  "$PATTERN" <<'REMOTE_SCRIPT'
set -euo pipefail
SOLARIS_DIR="$1"; SOLARIS_BUILD="$2"; SOLARIS_CTEST="$3"; PATTERN="$4"
cd "$SOLARIS_DIR"
LOG="$SOLARIS_BUILD/ctest_${PATTERN//[^A-Za-z0-9_.-]/_}.log"
echo "[remote] Running ctest -R '$PATTERN' in $SOLARIS_BUILD. Log: $LOG"
set +e
"$SOLARIS_CTEST" --test-dir "$SOLARIS_BUILD" -R "$PATTERN" --output-on-failure -VV -j1 2>&1 | tee "$LOG"
RC=${PIPESTATUS[0]}
set -e
echo "[remote] ctest exit code: $RC"
exit "$RC"
REMOTE_SCRIPT
