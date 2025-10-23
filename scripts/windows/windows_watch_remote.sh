#!/usr/bin/env bash
set -euo pipefail

# Start the MSYS2 bash watcher on the Windows host in background via SSH
# Relies on scripts/windows/.env for WIN_HOST, WIN_PORT, WIN_USER, WIN_REPO, CONFIG

ROOT_DIR=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
ENV_FILE="$ROOT_DIR/scripts/windows/.env"
if [[ -f "$ENV_FILE" ]]; then
  # shellcheck disable=SC1090
  source "$ENV_FILE"
fi

WIN_HOST=${WIN_HOST:-}
WIN_USER=${WIN_USER:-}
WIN_PORT=${WIN_PORT:-22}
if [[ "${WIN_REPO:-}" == /Users/* ]]; then
  REMOTE_DIR=/e/io/src
else
  REMOTE_DIR=${WIN_REPO:-/e/io/src}
fi
CONFIG=${CONFIG:-Debug}

if [[ -z "$WIN_HOST" || -z "$WIN_USER" ]]; then
  echo "[windows_watch_remote] Please set WIN_HOST and WIN_USER in $ENV_FILE" >&2
  exit 1
fi

SSH_OPTS=(-o ConnectTimeout=10 -o ServerAliveInterval=30 -o ServerAliveCountMax=3)
echo "[windows_watch_remote] Starting watcher on $WIN_USER@$WIN_HOST:$WIN_PORT"
echo "[windows_watch_remote] Repo: $REMOTE_DIR  Config: $CONFIG"

ssh "${SSH_OPTS[@]}" -p "$WIN_PORT" "$WIN_USER@$WIN_HOST" \
  "C:/msys64/usr/bin/bash.exe -lc 'cd \"$REMOTE_DIR\" && \
    if [ -d .git ] && git config --get remote.origin.url | grep -q '^git@github.com:'; then \
      git remote set-url origin https://github.com/anyks/io.git; \
    fi; \
    pgrep -f watch_build.sh >/dev/null 2>&1 && pkill -f watch_build.sh || true; \
    nohup env BUILD_TIMEOUT=900 ./scripts/windows/msys2/watch_build.sh \"$CONFIG\" > watch_build.log 2>&1 & echo STARTED; sleep 1; sed -n \"1,30p\" watch_build.log'"

echo "[windows_watch_remote] Done. Tail remote log with:"
echo "  ssh -p $WIN_PORT $WIN_USER@$WIN_HOST 'tail -f $REMOTE_DIR/watch_build.log'"
