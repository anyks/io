#!/usr/bin/env bash
set -euo pipefail

# Benchmark EPS across backends, aggregating results into a table.
# Runs on Linux for epoll and io_uring. Accepts IO_BENCH_* env vars to control load.

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT_DIR"

BUILD_DIR_EPOLL=build/bench-epoll
BUILD_DIR_IOURING=build/bench-iouring
# Which backends to run: comma-separated in BACKENDS (epoll,io_uring). Default: both.
BACKENDS=${BACKENDS:-epoll,io_uring}
DATASET_DIR_DEFAULT="$ROOT_DIR/build/bench-dataset"
DATASET_DIR="${IO_BENCH_DATASET_DIR:-$DATASET_DIR_DEFAULT}"

ensure_dataset() {
  local dir="$1"
  local gen_bin="$2"
  local count="${IO_DATASET_COUNT:-60}"
  local min_mb="${IO_DATASET_MIN_MB:-1}"
  local max_mb="${IO_DATASET_MAX_MB:-10}"
  local seed="${IO_DATASET_SEED:-42}"
  local total_mb="${IO_DATASET_TOTAL_MB:-100}"

  if [[ -n "${IO_BENCH_DATASET_DIR:-}" ]]; then
    echo "[bench] Using user-provided dataset: $IO_BENCH_DATASET_DIR"
    return 0
  fi
  if [[ ! -d "$dir" || -z "$(ls -A "$dir" 2>/dev/null)" ]]; then
    mkdir -p "$dir"
    echo "[bench] Generate dataset into $dir (count=$count, ${min_mb}-${max_mb}MB, seed=$seed, cap=${total_mb}MB)"
    "$gen_bin" "$dir" "$count" "$min_mb" "$max_mb" "$seed" "$total_mb"
  else
    echo "[bench] Reusing existing dataset at $dir"
  fi
}

EPOLL_OUT=""
if [[ ",$BACKENDS," == *",epoll,"* ]]; then
  echo "[bench] Configure epoll"
  cmake -S . -B "$BUILD_DIR_EPOLL" -G Ninja -DIO_BUILD_TESTS=ON -DIO_BUILD_BENCHMARKS=ON -DIO_WITH_IOURING=OFF -DCMAKE_BUILD_TYPE=Release
  echo "[bench] Build epoll"
  cmake --build "$BUILD_DIR_EPOLL" -j
  echo "[bench] Ensure dataset (<=100MB)"
  ensure_dataset "$DATASET_DIR" "$BUILD_DIR_EPOLL/gen_dataset"
  export IO_BENCH_DATASET_DIR="$DATASET_DIR"
  echo "[bench] Run epoll benchmark"
  EPOLL_OUT=$("$BUILD_DIR_EPOLL/io_tests" --gtest_filter=EpsBenchmark.LoadEchoRandomLarge 2>&1 | tee /dev/stderr | grep -E "^EPS_RESULT ") || true
fi

IOURING_OUT=""
if [[ ",$BACKENDS," == *",io_uring,"* ]]; then
  echo "[bench] Configure io_uring"
  cmake -S . -B "$BUILD_DIR_IOURING" -G Ninja -DIO_BUILD_TESTS=ON -DIO_BUILD_BENCHMARKS=ON -DIO_WITH_IOURING=ON -DCMAKE_BUILD_TYPE=Release
  echo "[bench] Build io_uring"
  cmake --build "$BUILD_DIR_IOURING" -j
  echo "[bench] Run io_uring benchmark"
  IOURING_OUT=$("$BUILD_DIR_IOURING/io_tests" --gtest_filter=EpsBenchmark.LoadEchoRandomLarge 2>&1 | tee /dev/stderr | grep -E "^EPS_RESULT ") || true
fi

echo
echo "[bench] Summary (CSV)"
echo "backend,clients,msgs_per_client,min_mb,max_mb,pipeline,duration_s,eps,bytes_total"
for line in "$EPOLL_OUT" "$IOURING_OUT"; do
  if [[ -n "$line" ]]; then
    # Convert key=value pairs to CSV fields in fixed order
    # shellcheck disable=SC2001
    backend=$(echo "$line" | sed -n 's/.*backend=\([^ ]*\).*/\1/p')
    clients=$(echo "$line" | sed -n 's/.*clients=\([^ ]*\).*/\1/p')
    msgs=$(echo "$line" | sed -n 's/.*msgs_per_client=\([^ ]*\).*/\1/p')
    min_mb=$(echo "$line" | sed -n 's/.*min_mb=\([^ ]*\).*/\1/p')
    max_mb=$(echo "$line" | sed -n 's/.*max_mb=\([^ ]*\).*/\1/p')
    pipeline=$(echo "$line" | sed -n 's/.*pipeline=\([^ ]*\).*/\1/p')
    duration=$(echo "$line" | sed -n 's/.*duration_s=\([^ ]*\).*/\1/p')
    eps=$(echo "$line" | sed -n 's/.*eps=\([^ ]*\).*/\1/p')
    bytes=$(echo "$line" | sed -n 's/.*bytes_total=\([^ ]*\).*/\1/p')
    echo "$backend,$clients,$msgs,$min_mb,$max_mb,$pipeline,$duration,$eps,$bytes"
  fi
done

echo
echo "[bench] Done"
