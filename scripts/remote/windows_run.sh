#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT_DIR"

ENV_FILE="scripts/remote/.env"
if [[ ! -f "$ENV_FILE" ]]; then
  echo "Missing $ENV_FILE. Copy from .env.example and fill in." >&2
  exit 1
fi
source "$ENV_FILE"

SSH_OPTS=("-o" "StrictHostKeyChecking=no")
if [[ -n "${SSH_KEY:-}" ]]; then
  SSH_OPTS+=("-i" "$SSH_KEY")
fi

ZIP_LOCAL="io-src.zip"
echo "[windows] archive sources (no .git/build)"
git archive --format=zip HEAD -o "$ZIP_LOCAL"
echo "[windows] upload sources to $WINDOWS_SSH:$WINDOWS_DIR\io.zip"
scp ${SSH_KEY:+-i "$SSH_KEY"} "$ZIP_LOCAL" "$WINDOWS_SSH:$WINDOWS_DIR\\io.zip"
rm -f "$ZIP_LOCAL"

echo "[windows] expand and build via MSYS2"
ssh "${SSH_OPTS[@]}" "$WINDOWS_SSH" 'powershell -Command "\
  if (-Not (Test-Path C:\\io\\src)) { New-Item -ItemType Directory -Path C:\\io\\src | Out-Null }; \
  Expand-Archive -Force C:\\io\\io.zip C:\\io\\src; \
  & C:\\msys64\\usr\\bin\\bash -lc \"set -e; export PATH=/mingw64/bin:/usr/bin:$PATH; cd /c/io/src; cmake -S . -B /c/io/build -G Ninja -DIO_BUILD_TESTS=ON -DIO_BUILD_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Release; cmake --build /c/io/build -j; IO_STRESS_N=32 IO_STRESS_REPEATS=200 IO_STRESS_PAYLOAD=32768 ctest --test-dir /c/io/build -R WinIocp.HighloadLargePayload --output-on-failure --repeat until-fail:20 --timeout 900 || true; IO_STRESS_N=24 IO_STRESS_REPEATS=120 IO_STRESS_PAYLOAD=32768 ctest --test-dir /c/io/build -R WinIocp.WriteFloodServerToClients --output-on-failure --repeat until-fail:10 --timeout 900 || true\" \
"'

echo "[windows] done"
