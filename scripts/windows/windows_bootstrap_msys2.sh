#!/usr/bin/env bash
set -euo pipefail

# Install required MSYS2 MinGW64 packages on the Windows host (non-interactive)

ROOT_DIR=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
ENV_FILE="$ROOT_DIR/scripts/windows/.env"
if [[ -f "$ENV_FILE" ]]; then
  # shellcheck disable=SC1090
  source "$ENV_FILE"
fi

WIN_HOST=${WIN_HOST:-}
WIN_USER=${WIN_USER:-}
WIN_PORT=${WIN_PORT:-22}

if [[ -z "$WIN_HOST" || -z "$WIN_USER" ]]; then
  echo "[windows_bootstrap_msys2] Please set WIN_HOST and WIN_USER in $ENV_FILE" >&2
  exit 1
fi

SSH_OPTS=(-o ConnectTimeout=10 -o ServerAliveInterval=30 -o ServerAliveCountMax=3)

echo "[windows_bootstrap_msys2] Installing MSYS2 packages on $WIN_USER@$WIN_HOST:$WIN_PORT"
ssh "${SSH_OPTS[@]}" -p "$WIN_PORT" "$WIN_USER@$WIN_HOST" \
  "C:/msys64/usr/bin/bash.exe -lc 'set -e; pacman -Sy --noconfirm; pacman -S --needed --noconfirm \
    mingw-w64-x86_64-toolchain \
    mingw-w64-x86_64-cmake \
    mingw-w64-x86_64-ninja \
    coreutils; echo BOOTSTRAP_OK'"
