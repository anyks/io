#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"/../..

if [[ -f scripts/solaris/.env ]]; then
  # shellcheck disable=SC1091
  source scripts/solaris/.env
else
  # shellcheck disable=SC1091
  source scripts/solaris/.env.example
fi

COUNT="${1:-500}"

ssh -p "${SOLARIS_SSH_PORT}" ${SOLARIS_SSH_OPTS} "${SOLARIS_SSH}" bash -s -- \
  "${SOLARIS_DIR}" \
  "${SOLARIS_BUILD}" \
  "${SOLARIS_CTEST}" \
  "${COUNT}" <<'REMOTE_SCRIPT'
set -euo pipefail
SOLARIS_DIR="$1"; SOLARIS_BUILD="$2"; SOLARIS_CTEST="$3"; COUNT="$4"; SOLARIS_CTEST_TIMEOUT="${5:-180}"
cd "$SOLARIS_DIR"
LOG="$SOLARIS_BUILD/ctest_stress_highload.log"
echo "[remote] Stress: NetHighload.ManyClientsEchoNoBlock x${COUNT} (seq). Log: $LOG"
"$SOLARIS_CTEST" --test-dir "$SOLARIS_BUILD" -R NetHighload.ManyClientsEchoNoBlock -j1 --repeat "until-fail:${COUNT}" --output-on-failure --timeout "$SOLARIS_CTEST_TIMEOUT" >>"$LOG" 2>&1
RC=$?
echo "[remote] stress exit code: $RC (see $LOG)"
exit "$RC"
REMOTE_SCRIPT
