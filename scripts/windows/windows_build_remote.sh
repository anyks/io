#!/usr/bin/env bash
set -euo pipefail

# Run a one-shot build on the Windows host (MSYS2 bash) — no git watcher involved
# Usage: bash scripts/windows/windows_build_remote.sh [Debug|Release]

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
CONFIG=${1:-${CONFIG:-Debug}}
BUILD_TIMEOUT=${BUILD_TIMEOUT:-900}

if [[ -z "$WIN_HOST" || -z "$WIN_USER" ]]; then
  echo "[windows_build_remote] Please set WIN_HOST and WIN_USER in $ENV_FILE" >&2
  exit 1
fi

SSH_OPTS=(-o ConnectTimeout=10 -o ServerAliveInterval=30 -o ServerAliveCountMax=3)

echo "[windows_build_remote] Host=$WIN_USER@$WIN_HOST:$WIN_PORT Repo=$REMOTE_DIR Config=$CONFIG Timeout=${BUILD_TIMEOUT}s"

# Build in foreground to stream output here; also write a remote log
ssh "${SSH_OPTS[@]}" -p "$WIN_PORT" "$WIN_USER@$WIN_HOST" \
  "C:/msys64/usr/bin/bash.exe -lc 'set -euo pipefail; export MSYSTEM=MINGW64; export CHERE_INVOKING=1; export PATH=/mingw64/bin:/usr/bin:\$PATH; \
    which cmake || true; cmake --version || true; \
    cd \"$REMOTE_DIR\" && : > build_remote.log; \
    ( env BUILD_TIMEOUT=$BUILD_TIMEOUT ./scripts/windows/msys2/build.sh \"$CONFIG\" ) 2>&1 | tee -a build_remote.log'"

echo "[windows_build_remote] Build finished with exit code $?"
