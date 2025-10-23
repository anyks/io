#!/usr/bin/env bash
set -euo pipefail

# Sync current repo to Windows host (MSYS2 bash over SSH) using tar-over-ssh.
# Uses settings from scripts/windows/.env (WIN_HOST, WIN_PORT, WIN_USER, WIN_REPO).

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
  echo "[windows_sync] Please set WIN_HOST and WIN_USER in $ENV_FILE" >&2
  exit 1
fi

echo "[windows_sync] Target: $WIN_USER@$WIN_HOST:$WIN_PORT"
echo "[windows_sync] Remote repo dir: $REMOTE_DIR"

cd "$ROOT_DIR"

# Create remote dir and extract archive there
create_and_extract() {
  ssh -p "$WIN_PORT" "$WIN_USER@$WIN_HOST" \
    "mkdir -p '$REMOTE_DIR' && tar xzf - -C '$REMOTE_DIR'"
}

echo "[windows_sync] Creating archive and transferring..."
tar --exclude './.git' \
    --exclude './build' \
    --exclude './build/*' \
    --exclude './_install' \
    --exclude './_CPack_Packages' \
    --exclude './.DS_Store' \
    -czf - . | create_and_extract

echo "[windows_sync] Sync complete. Remote path: $REMOTE_DIR"
