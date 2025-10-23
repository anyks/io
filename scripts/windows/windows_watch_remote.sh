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
REMOTE_DIR=${WIN_REPO:-/e/io/src}
CONFIG=${CONFIG:-Debug}

if [[ -z "$WIN_HOST" || -z "$WIN_USER" ]]; then
  echo "[windows_watch_remote] Please set WIN_HOST and WIN_USER in $ENV_FILE" >&2
  exit 1
fi

echo "[windows_watch_remote] Starting watcher on $WIN_USER@$WIN_HOST:$WIN_PORT"
echo "[windows_watch_remote] Repo: $REMOTE_DIR  Config: $CONFIG"

ssh -p "$WIN_PORT" "$WIN_USER@$WIN_HOST" \
  "cd '$REMOTE_DIR' && nohup ./scripts/windows/msys2/watch_build.sh '$CONFIG' > watch_build.log 2>&1 & echo STARTED: \
  \
  \"\$!\" && sleep 1 && tail -n +1 -q watch_build.log | sed -n '1,5p'"

echo "[windows_watch_remote] Done. Tail remote log with:"
echo "  ssh -p $WIN_PORT $WIN_USER@$WIN_HOST 'tail -f $REMOTE_DIR/watch_build.log'"
