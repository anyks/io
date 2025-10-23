#!/usr/bin/env bash
set -euo pipefail

# Run ctest on the Windows host under a clean MSYS2 MINGW64 environment (env -i)
# Usage examples:
#   bash scripts/windows/windows_ctest_remote.sh                    # run all tests
#   bash scripts/windows/windows_ctest_remote.sh -R WinIocp.* -j4   # pass-thru args

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
BUILD_DIR=${BUILD_WIN:-/e/io}/build/mingw64-${CONFIG:-Release}

if [[ -z "$WIN_HOST" || -z "$WIN_USER" ]]; then
  echo "[windows_ctest_remote] Please set WIN_HOST and WIN_USER in $ENV_FILE" >&2
  exit 1
fi

SSH_OPTS=(-o ConnectTimeout=10 -o ServerAliveInterval=30 -o ServerAliveCountMax=3)

echo "[windows_ctest_remote] Host=$WIN_USER@$WIN_HOST:$WIN_PORT BuildDir=$BUILD_DIR"

CTEST_ARGS=("$@")

ssh "${SSH_OPTS[@]}" -p "$WIN_PORT" "$WIN_USER@$WIN_HOST" \
  "C:/msys64/usr/bin/env.exe -i MSYSTEM=MINGW64 CHERE_INVOKING=1 PATH=/mingw64/bin:/usr/bin C:/msys64/usr/bin/bash.exe -lc 'set -e; cd "$BUILD_DIR"; echo Running: ctest ${CTEST_ARGS[*]@Q}; ctest "${CTEST_ARGS[@]}"'"

echo "[windows_ctest_remote] Done with exit code $?"
