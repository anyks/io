#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"/../..

if [[ -f scripts/solaris/.env ]]; then
  source scripts/solaris/.env
else
  source scripts/solaris/.env.example
fi

# Optional: run in background (outputs to log on remote)
MODE=${1:-foreground} # values: foreground|background

ssh -p "${SOLARIS_SSH_PORT}" ${SOLARIS_SSH_OPTS:-} "${SOLARIS_SSH}" bash -s -- \
  "${SOLARIS_DIR}" \
  "${SOLARIS_BUILD}" \
  "${IO_BENCH_CLIENTS:-20}" \
  "${IO_BENCH_MSGS:-2000}" \
  "${IO_BENCH_PIPELINE:-8}" \
  "${IO_BENCH_PROGRESS_SEC:-5}" \
  "${IO_BENCH_TIMEOUT:-3600000}" \
  "${IO_BENCH_MIN_MB:-1}" \
  "${IO_BENCH_MAX_MB:-5}" \
  "${IO_BENCH_SEED:-42}" \
  "${IO_DATASET_COUNT:-200}" \
  "${IO_DATASET_MIN_MB:-1}" \
  "${IO_DATASET_MAX_MB:-5}" \
  "${IO_DATASET_TOTAL_MB:-100}" \
  "$MODE" <<'REMOTE_SCRIPT'
set -euo pipefail
SOLARIS_DIR="$1"; SOLARIS_BUILD="$2"; C="$3"; M="$4"; P="$5"; PROG="$6"; T="$7"; MIN_MB="$8"; MAX_MB="$9"; SEED="${10}"; DS_COUNT="${11}"; DS_MIN_MB="${12}"; DS_MAX_MB="${13}"; DS_TOTAL_MB="${14}"; MODE="${15}"
cd "$SOLARIS_DIR"
cd "$SOLARIS_BUILD"
DATASET_DIR="$PWD/dataset100_1_5"
# Ensure dataset exists (same shape as Linux bench)
mkdir -p "$DATASET_DIR"
if [ -z "$(ls -A "$DATASET_DIR" 2>/dev/null)" ]; then
  if [ -x "./gen_dataset" ]; then
    echo "[remote] generating dataset at $DATASET_DIR (count=$DS_COUNT, ${DS_MIN_MB}-${DS_MAX_MB}MB, total=${DS_TOTAL_MB}MB, seed=$SEED)"
    ./gen_dataset "$DATASET_DIR" "$DS_COUNT" "$DS_MIN_MB" "$DS_MAX_MB" "$SEED" "$DS_TOTAL_MB"
  else
    echo "[remote] gen_dataset not found; proceeding without dataset" >&2
  fi
fi
export IO_BENCH_CLIENTS="$C" IO_BENCH_MSGS="$M" IO_BENCH_PIPELINE="$P" IO_BENCH_PROGRESS_SEC="$PROG" IO_BENCH_TIMEOUT="$T"
export IO_BENCH_MIN_MB="$MIN_MB" IO_BENCH_MAX_MB="$MAX_MB" IO_BENCH_SEED="$SEED"
export IO_BENCH_DATASET_DIR="$DATASET_DIR"
CMD="./io_tests --gtest_filter=EpsBenchmark.LoadEchoRandomLarge --gtest_brief=1"
if [[ "$MODE" == "background" ]]; then
  LOG="eps_eventports_$(date +%Y%m%d_%H%M%S).log"
  echo "[remote] starting background benchmark, log=$LOG"
  nohup bash -c "$CMD" > "$LOG" 2>&1 &
  echo "[remote] pid=$!"
else
  echo "[remote] running foreground benchmark"
  exec bash -c "$CMD"
fi
REMOTE_SCRIPT
