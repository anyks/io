#!/usr/bin/env bash
set -euo pipefail

# MSYS2 bash watcher: pull origin/main and rebuild on new commits
# Usage: ./scripts/windows/msys2/watch_build.sh [Debug|Release]

CONFIG=${1:-Debug}
INTERVAL=${INTERVAL:-15}
BUILD_WIN=${BUILD_WIN:-/e/io}

cd "$(git rev-parse --show-toplevel)"

echo "[io][watch] Start watcher: interval=${INTERVAL}s, config=${CONFIG}, build_root=${BUILD_WIN}"

git fetch origin main

while true; do
  LOCAL=$(git rev-parse HEAD || echo "")
  REMOTE=$(git rev-parse origin/main || echo "")
  if [[ -n "$REMOTE" && "$LOCAL" != "$REMOTE" ]]; then
    echo "[io][watch] New commit: ${LOCAL} -> ${REMOTE}"
    git reset --hard origin/main
    ./scripts/windows/msys2/build.sh "$CONFIG" || {
      echo "[io][watch] Build failed. Will retry on next tick." >&2
    }
  fi
  sleep "$INTERVAL"
  git fetch origin main >/dev/null 2>&1 || true
done
