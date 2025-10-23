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

echo "[linux] rsync sources to $LINUX_SSH:$LINUX_DIR"
rsync -az --delete --exclude '.git' --exclude 'build' ./ "$LINUX_SSH:$LINUX_DIR"/

remote() {
  ssh "${SSH_OPTS[@]}" "$LINUX_SSH" "$@"
}

echo "[linux] install deps (non-interactive; skipped if not permitted)"
remote "set +e; sudo -n apt-get update || true; sudo -n apt-get install -y cmake ninja-build g++ liburing-dev || true; true"

echo "[linux] clean build directories"
remote "set -e; mkdir -p '$LINUX_DIR'; cd '$LINUX_DIR'; rm -rf build/epoll build/iouring || true"

echo "[linux][epoll] configure/build/test (clean)"
remote "set -e; cd '$LINUX_DIR'; GEN='-G Ninja'; command -v ninja >/dev/null 2>&1 || GEN='-G \"Unix Makefiles\"'; if command -v g++ >/dev/null 2>&1; then export CXX=g++; export CC=gcc; elif command -v clang++ >/dev/null 2>&1; then export CXX=clang++; export CC=clang; fi; cmake -S . -B build/epoll \$GEN -DIO_BUILD_TESTS=ON -DIO_WITH_IOURING=OFF -DCMAKE_BUILD_TYPE=Debug; cmake --build build/epoll -j; ctest --test-dir build/epoll -j4 --output-on-failure || true"

echo "[linux][epoll] stress"
remote "set -e; cd '$LINUX_DIR'; IO_STRESS_N=${IO_STRESS_N:-300} IO_STRESS_M=${IO_STRESS_M:-20000} bash scripts/stress_matrix.sh Debug || true; tar czf epoll-stress-logs.tgz build/**/stress || true"

echo "[linux][iouring] configure/build/test (clean)"
remote "set -e; cd '$LINUX_DIR'; GEN='-G Ninja'; command -v ninja >/dev/null 2>&1 || GEN='-G \"Unix Makefiles\"'; if command -v g++ >/dev/null 2>&1; then export CXX=g++; export CC=gcc; elif command -v clang++ >/dev/null 2>&1; then export CXX=clang++; export CC=clang; fi; cmake -S . -B build/iouring \$GEN -DIO_BUILD_TESTS=ON -DIO_WITH_IOURING=ON -DCMAKE_BUILD_TYPE=Debug; cmake --build build/iouring -j; ctest --test-dir build/iouring -j4 --output-on-failure || true"

echo "[linux][iouring] stress"
remote "set -e; cd '$LINUX_DIR'; IO_STRESS_N=${IO_STRESS_N:-300} IO_STRESS_M=${IO_STRESS_M:-20000} bash scripts/stress_matrix.sh Debug || true; tar czf iouring-stress-logs.tgz build/**/stress || true"

echo "[linux] fetch logs"
scp ${SSH_KEY:+-i "$SSH_KEY"} "$LINUX_SSH:$LINUX_DIR/epoll-stress-logs.tgz" ./ || true
scp ${SSH_KEY:+-i "$SSH_KEY"} "$LINUX_SSH:$LINUX_DIR/iouring-stress-logs.tgz" ./ || true

echo "[linux] done"
