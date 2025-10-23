#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT_DIR"

echo "[all] Linux"
bash scripts/remote/linux_run.sh || true

echo "[all] Windows (MSYS2)"
bash scripts/remote/windows_run.sh || true

echo "[all] Solaris"
bash scripts/remote/solaris_run.sh || true

echo "[all] done"
