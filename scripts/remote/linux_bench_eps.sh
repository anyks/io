#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT_DIR"

# Source-of-truth: scripts/linux/.env (required). Optionally merge root .env.
ENV_FILE="scripts/linux/.env"
if [[ ! -f "$ENV_FILE" ]]; then
  echo "Missing $ENV_FILE. Copy from scripts/linux/.env (template) and fill in." >&2
  exit 1
fi
source "$ENV_FILE"
[[ -f ./.env ]] && source ./.env || true

SSH_OPTS=("-o" "StrictHostKeyChecking=no")
if [[ -n "${LINUX_SSH_OPTS:-}" ]]; then
  # Split LINUX_SSH_OPTS by whitespace into array and append
  read -r -a _EXTRA_OPTS <<< "${LINUX_SSH_OPTS}"
  SSH_OPTS+=("${_EXTRA_OPTS[@]}")
fi
if [[ -n "${LINUX_SSH_PORT:-}" ]]; then
  SSH_OPTS+=("-p" "$LINUX_SSH_PORT")
fi
if [[ -n "${SSH_KEY:-}" ]]; then
  SSH_OPTS+=("-i" "$SSH_KEY")
fi

remote() { ssh "${SSH_OPTS[@]}" "$LINUX_SSH" "$@"; }

echo "[bench/linux] rsync sources to $LINUX_SSH:$LINUX_DIR"
RSYNC_SSH="ssh ${SSH_OPTS[*]}"
rsync -az -e "$RSYNC_SSH" --delete --exclude '.git' --exclude 'build' ./ "$LINUX_SSH:$LINUX_DIR"/

echo "[bench/linux] ensure deps (non-interactive)"
remote "set +e; sudo -n apt-get update || true; sudo -n apt-get install -y cmake ninja-build g++ liburing-dev || true; true"

# Parameters (override via env)
BACKEND=${IO_BACKEND:-epoll}   # epoll | io_uring
CLIENTS=${IO_BENCH_CLIENTS:-100}
MSGS=${IO_BENCH_MSGS:-10000}
MIN_MB=${IO_BENCH_MIN_MB:-1}
MAX_MB=${IO_BENCH_MAX_MB:-5}
PIPELINE=${IO_BENCH_PIPELINE:-16}
TIMEOUT=${IO_BENCH_TIMEOUT:-86400000}
SEED=${IO_BENCH_SEED:-42}

BUILD_SUBDIR="bench-epoll"
IOURING_FLAG="-DIO_WITH_IOURING=OFF"
if [[ "$BACKEND" == "io_uring" ]]; then
  BUILD_SUBDIR="bench-iouring"
  IOURING_FLAG="-DIO_WITH_IOURING=ON"
fi

DATASET_DIR=${IO_BENCH_DATASET_DIR:-"$LINUX_DIR/build/$BUILD_SUBDIR/dataset100_1_5"}
DATASET_COUNT=${IO_DATASET_COUNT:-200}
DATASET_MIN_MB=${IO_DATASET_MIN_MB:-1}
DATASET_MAX_MB=${IO_DATASET_MAX_MB:-5}
DATASET_TOTAL_MB=${IO_DATASET_TOTAL_MB:-100}

echo "[bench/linux] target=$LINUX_SSH port=${LINUX_SSH_PORT:-22} backend=$BACKEND"
echo "[bench/linux][$BACKEND] configure Release with benchmarks"
EXTRA_VERBOSE=""
# Optional explicit verbose override via env IO_VERBOSE={0,1}
if [[ "${IO_VERBOSE:-0}" == "1" ]]; then
  if [[ "$BACKEND" == "io_uring" ]]; then EXTRA_VERBOSE="-DIO_ENABLE_IOURING_VERBOSE=ON"; fi
fi
remote "set -e; cd '$LINUX_DIR'; if command -v g++ >/dev/null 2>&1; then export CXX=g++; export CC=gcc; elif command -v clang++ >/dev/null 2>&1; then export CXX=clang++; export CC=clang; fi; if command -v ninja >/dev/null 2>&1; then cmake -S . -B build/$BUILD_SUBDIR -G Ninja -DIO_BUILD_TESTS=ON -DIO_BUILD_BENCHMARKS=ON $IOURING_FLAG $EXTRA_VERBOSE -DCMAKE_BUILD_TYPE=Release; else cmake -S . -B build/$BUILD_SUBDIR -G 'Unix Makefiles' -DIO_BUILD_TESTS=ON -DIO_BUILD_BENCHMARKS=ON $IOURING_FLAG $EXTRA_VERBOSE -DCMAKE_BUILD_TYPE=Release; fi"

echo "[bench/linux][$BACKEND] build"
remote "set -e; cd '$LINUX_DIR'; cmake --build build/$BUILD_SUBDIR -j"

echo "[bench/linux] ensure dataset at $DATASET_DIR (<=${DATASET_TOTAL_MB}MB)"
remote "set -e; cd '$LINUX_DIR'; mkdir -p '$DATASET_DIR'; if [ -z \"$(ls -A '$DATASET_DIR' 2>/dev/null)\" ]; then build/$BUILD_SUBDIR/gen_dataset '$DATASET_DIR' '$DATASET_COUNT' '$DATASET_MIN_MB' '$DATASET_MAX_MB' '$SEED' '$DATASET_TOTAL_MB'; else echo 'dataset exists'; fi"

echo "[bench/linux][$BACKEND] run EPS benchmark ($((CLIENTS*MSGS)) msgs)"
LOG_NAME="bench_${BACKEND}.log"
# Optionally pass progress period and trace client overrides to remote env
EXTRA_ENV=""
if [[ -n "${IO_BENCH_PROGRESS_SEC:-}" ]]; then EXTRA_ENV+=" IO_BENCH_PROGRESS_SEC='${IO_BENCH_PROGRESS_SEC}'"; fi
if [[ -n "${IO_BENCH_TRACE_CLIENT:-}" ]]; then EXTRA_ENV+=" IO_BENCH_TRACE_CLIENT='${IO_BENCH_TRACE_CLIENT}'"; fi
remote "set -e; cd '$LINUX_DIR'; IO_BENCH_DATASET_DIR='$DATASET_DIR' IO_BENCH_MIN_MB='$MIN_MB' IO_BENCH_MAX_MB='$MAX_MB' IO_BENCH_CLIENTS='$CLIENTS' IO_BENCH_MSGS='$MSGS' IO_BENCH_PIPELINE='$PIPELINE' IO_BENCH_TIMEOUT='$TIMEOUT' IO_BENCH_SEED='$SEED' $EXTRA_ENV build/$BUILD_SUBDIR/io_tests --gtest_filter=EpsBenchmark.LoadEchoRandomLarge 2>&1 | tee $LOG_NAME"

echo "[bench/linux] fetch results"
scp ${SSH_KEY:+-i "$SSH_KEY"} ${LINUX_SSH_PORT:+-P "$LINUX_SSH_PORT"} "$LINUX_SSH:$LINUX_DIR/$LOG_NAME" ./ || true

echo "[bench/linux] done"
