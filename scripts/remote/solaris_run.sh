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

# Подготовка .env для solaris-скриптов
cat > scripts/solaris/.env <<EOF
SOLARIS_SSH=${SOLARIS_SSH}
SOLARIS_SSH_PORT=${SOLARIS_SSH_PORT:-22}
SOLARIS_DIR=${SOLARIS_DIR}
SOLARIS_BUILD=build/sol
SOLARIS_SSH_OPTS=-o StrictHostKeyChecking=no
SOLARIS_EVENTPORTS=ON
EOF

echo "[solaris] sync"
bash scripts/solaris/solaris_sync.sh

echo "[solaris] Event Ports: configure/build/test/stress"
bash scripts/solaris/solaris_configure.sh
bash scripts/solaris/solaris_build.sh
bash scripts/solaris/solaris_test.sh || true
bash scripts/solaris/solaris_parallel_and_stress.sh 4 200 || true

echo "[solaris] switch to /dev/poll"
sed -i '' 's/^SOLARIS_EVENTPORTS=.*/SOLARIS_EVENTPORTS=OFF/' scripts/solaris/.env || true
bash scripts/solaris/solaris_configure.sh
bash scripts/solaris/solaris_build.sh
bash scripts/solaris/solaris_test.sh || true
bash scripts/solaris/solaris_parallel_and_stress.sh 4 200 || true

echo "[solaris] done"
