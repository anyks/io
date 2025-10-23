#!/usr/bin/env bash
set -euo pipefail

# Stop the MSYS2 watch_build.sh on the Windows host (if running)

ROOT_DIR=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
ENV_FILE="$ROOT_DIR/scripts/windows/.env"
if [[ -f "$ENV_FILE" ]]; then
  # shellcheck disable=SC1090
  source "$ENV_FILE"
fi

WIN_HOST=${WIN_HOST:-}
WIN_USER=${WIN_USER:-}
WIN_PORT=${WIN_PORT:-22}
REMOTE_DIR=${WIN_REPO:-/e/io/src}

if [[ -z "$WIN_HOST" || -z "$WIN_USER" ]]; then
  echo "[windows_stop_watcher] Please set WIN_HOST and WIN_USER in $ENV_FILE" >&2
  exit 1
fi

SSH_OPTS=(-o ConnectTimeout=10 -o ServerAliveInterval=30 -o ServerAliveCountMax=3)

echo "[windows_stop_watcher] Killing watcher on $WIN_USER@$WIN_HOST:$WIN_PORT"
ssh "${SSH_OPTS[@]}" -p "$WIN_PORT" "$WIN_USER@$WIN_HOST" \
  "C:/msys64/usr/bin/bash.exe -lc 'pgrep -f scripts/windows/msys2/watch_build.sh >/dev/null 2>&1 && pkill -f scripts/windows/msys2/watch_build.sh || true; echo KILLED'"
