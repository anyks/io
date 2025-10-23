#!/usr/bin/env bash
set -euo pipefail

# MSYS2 bash watcher: pull origin/main and rebuild on new commits
# Usage: ./scripts/windows/msys2/watch_build.sh [Debug|Release]

# Load env if present
ROOT_DIR=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
ENV_FILE="$ROOT_DIR/scripts/windows/.env"
if [[ -f "$ENV_FILE" ]]; then
  # shellcheck disable=SC1090
  source "$ENV_FILE"
fi

CONFIG=${1:-${CONFIG:-Debug}}
INTERVAL=${INTERVAL:-15}
BUILD_WIN=${BUILD_WIN:-/e/io}
BUILD_TIMEOUT=${BUILD_TIMEOUT:-900}
FETCH_TIMEOUT=${FETCH_TIMEOUT:-30}
RESET_TIMEOUT=${RESET_TIMEOUT:-30}
LOOP_LOG_EVERY=${LOOP_LOG_EVERY:-1}

tick=0
now() { date '+%Y-%m-%d %H:%M:%S'; }

cd "$ROOT_DIR"

echo "[io][watch] Start watcher: interval=${INTERVAL}s, config=${CONFIG}, build_root=${BUILD_WIN}"

# Initial fetch with timeout (best-effort)
if command -v timeout >/dev/null 2>&1; then
  timeout -k 5s "${FETCH_TIMEOUT}s" git fetch origin main || true
else
  git fetch origin main || true
fi

while true; do
  # Read SHAs with small timeouts
  if command -v timeout >/dev/null 2>&1; then
    LOCAL=$(timeout 5s git rev-parse HEAD || echo "")
    REMOTE=$(timeout 5s git rev-parse origin/main || echo "")
  else
    LOCAL=$(git rev-parse HEAD || echo "")
    REMOTE=$(git rev-parse origin/main || echo "")
  fi

  tick=$((tick+1))
  if (( LOOP_LOG_EVERY>0 && tick % LOOP_LOG_EVERY == 0 )); then
    echo "[io][watch][$(now)] tick=$tick local=${LOCAL:0:8} remote=${REMOTE:0:8}"
    # heartbeat file
    echo "$(now) tick=$tick local=$LOCAL remote=$REMOTE" > watch_heartbeat.txt 2>/dev/null || true
  fi
  if [[ -n "$REMOTE" && "$LOCAL" != "$REMOTE" ]]; then
    echo "[io][watch] New commit: ${LOCAL} -> ${REMOTE}"
    if command -v timeout >/dev/null 2>&1; then
      timeout -k 5s "${RESET_TIMEOUT}s" git reset --hard origin/main || {
        echo "[io][watch] WARN: git reset timed out after ${RESET_TIMEOUT}s" >&2
        continue
      }
    else
      git reset --hard origin/main || {
        echo "[io][watch] WARN: git reset failed" >&2
        continue
      }
    fi
    BUILD_TIMEOUT="$BUILD_TIMEOUT" ./scripts/windows/msys2/build.sh "$CONFIG" || {
      echo "[io][watch] Build failed. Will retry on next tick." >&2
    }
  fi
  sleep "$INTERVAL"
  if command -v timeout >/dev/null 2>&1; then
    timeout -k 5s "${FETCH_TIMEOUT}s" git fetch origin main >/dev/null 2>&1 || true
  else
    git fetch origin main >/dev/null 2>&1 || true
  fi
done
