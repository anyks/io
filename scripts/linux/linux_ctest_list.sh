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

ssh -p "${LINUX_SSH_PORT}" ${LINUX_SSH_OPTS} "${LINUX_SSH}" bash -s -- \
  "${LINUX_DIR}" \
  "${LINUX_BUILD}" \
  "${LINUX_CTEST}" <<'REMOTE_SCRIPT'
set -euo pipefail
LINUX_DIR="$1"; LINUX_BUILD="$2"; LINUX_CTEST="$3"
cd "$LINUX_DIR"
"$LINUX_CTEST" --test-dir "$LINUX_BUILD" -N | sed -n '1,200p'
REMOTE_SCRIPT
