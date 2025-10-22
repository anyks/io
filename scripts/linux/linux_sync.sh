#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"/../..

if [[ -f scripts/linux/.env ]]; then
  # shellcheck disable=SC1091
  source scripts/linux/.env
else
  # shellcheck disable=SC1091
  source scripts/linux/.env.example
fi

# Strictly enforce SSH port 221 and required vars
: "${LINUX_SSH:?LINUX_SSH is required}"
: "${LINUX_SSH_PORT:?LINUX_SSH_PORT is required}"
: "${LINUX_DIR:?LINUX_DIR is required}"
if [[ "${LINUX_SSH_PORT}" != "221" ]]; then
  echo "ERROR: LINUX_SSH_PORT must be 221; got '${LINUX_SSH_PORT}'." >&2
  exit 2
fi

RSYNC_SSH="ssh -p ${LINUX_SSH_PORT} ${LINUX_SSH_OPTS}"

rsync -avz --delete -e "$RSYNC_SSH" \
  --exclude ".git/" \
  --exclude "build/" \
  --exclude "_install/" \
  --exclude "_deps/" \
  ./ "${LINUX_SSH}:${LINUX_DIR}/"

echo "Synced project to ${LINUX_SSH}:${LINUX_DIR}"
