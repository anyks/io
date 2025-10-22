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

COUNT="${1:-300}"

ssh -p "${LINUX_SSH_PORT}" ${LINUX_SSH_OPTS} "${LINUX_SSH}" bash -s -- \
  "${LINUX_DIR}" \
  "${LINUX_BUILD}" \
  "${LINUX_CTEST}" \
  "${COUNT}" \
  "${LINUX_CTEST_TIMEOUT:-180}" <<'REMOTE_SCRIPT'
set -euo pipefail
LINUX_DIR="$1"; LINUX_BUILD="$2"; LINUX_CTEST="$3"; COUNT="$4"; LINUX_CTEST_TIMEOUT="$5"
cd "$LINUX_DIR"
LOG="$LINUX_BUILD/ctest_stress_highload.log"
echo "[remote] Stress: NetHighload.ManyClientsEchoNoBlock x${COUNT} (seq)" | tee "$LOG"
"$LINUX_CTEST" --test-dir "$LINUX_BUILD" -R NetHighload.ManyClientsEchoNoBlock -j1 --repeat "until-fail:${COUNT}" --output-on-failure --timeout "$LINUX_CTEST_TIMEOUT" 2>&1 | tee -a "$LOG"
RC=${PIPESTATUS[0]}
echo "[remote] stress exit code: $RC" | tee -a "$LOG"
exit "$RC"
REMOTE_SCRIPT
