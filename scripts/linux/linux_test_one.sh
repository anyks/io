#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"/../..

if [[ -f scripts/linux/.env ]]; then
  source scripts/linux/.env
else
  source scripts/linux/.env.example
fi

# Strictly enforce SSH port 221 and required vars
: "${LINUX_SSH:?LINUX_SSH is required}"
: "${LINUX_SSH_PORT:?LINUX_SSH_PORT is required}"
: "${LINUX_DIR:?LINUX_DIR is required}"
: "${LINUX_BUILD:?LINUX_BUILD is required}"
if [[ "${LINUX_SSH_PORT}" != "221" ]]; then
  echo "ERROR: LINUX_SSH_PORT must be 221; got '${LINUX_SSH_PORT}'." >&2
  exit 2
fi

PATTERN=${1:-}
if [[ -z "$PATTERN" ]]; then
  echo "Usage: $0 <gtest_regex>" >&2
  exit 2
fi

ssh -p "${LINUX_SSH_PORT}" ${LINUX_SSH_OPTS} "${LINUX_SSH}" bash -s -- \
  "${LINUX_DIR}" \
  "${LINUX_BUILD}" \
  "${LINUX_CTEST}" \
  "$PATTERN" \
  "${LINUX_CTEST_TIMEOUT:-180}" <<'REMOTE_SCRIPT'
set -euo pipefail
LINUX_DIR="$1"; LINUX_BUILD="$2"; LINUX_CTEST="$3"; PATTERN="$4"; LINUX_CTEST_TIMEOUT="$5"
cd "$LINUX_DIR"
LOG="$LINUX_BUILD/ctest_${PATTERN//[^A-Za-z0-9_.-]/_}.log"
echo "[remote] Running ctest -R '$PATTERN' in $LINUX_BUILD. Log: $LOG"
set +e
"$LINUX_CTEST" --test-dir "$LINUX_BUILD" -R "$PATTERN" --output-on-failure -j1 --timeout "$LINUX_CTEST_TIMEOUT" >"$LOG" 2>&1
RC=$?
set -e
if [[ "$RC" -ne 0 ]]; then
  echo "[remote] tail of log ($LOG):"
  tail -n 200 "$LOG" || true
fi
echo "[remote] ctest exit code: $RC (see $LOG)"
exit "$RC"
REMOTE_SCRIPT
