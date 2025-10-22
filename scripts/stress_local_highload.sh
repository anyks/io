#!/usr/bin/env bash
set -euo pipefail

# Local stress runner for NetHighload.ManyClientsEchoNoBlock
# Usage:
#   ./scripts/stress_local_highload.sh [PARALLEL_INSTANCES] [STRESS_REPEATS]
# Env overrides:
#   BUILD_DIR (default: ./build/debug)
#   TEST_REGEX (default: ^NetHighload\.ManyClientsEchoNoBlock$)
#   LOG_DIR (default: $BUILD_DIR/stress)

BUILD_DIR="${BUILD_DIR:-./build/debug}"
TEST_REGEX="${TEST_REGEX:-^NetHighload\\.ManyClientsEchoNoBlock$}"
PARALLEL="${1:-4}"
STRESS="${2:-300}"
LOG_DIR="${LOG_DIR:-${BUILD_DIR}/stress}"

mkdir -p "$LOG_DIR"

echo "[info] Using BUILD_DIR=$BUILD_DIR"
echo "[info] Test regex: $TEST_REGEX"
echo "[info] Logs: $LOG_DIR"

echo "[phase] Parallel: ${PARALLEL} instance(s)"
pids=()
for i in $(seq 1 "$PARALLEL"); do
  logfile="${LOG_DIR}/parallel_${i}.log"
  echo "[info] starting instance #$i -> $logfile"
  (
    ctest --test-dir "$BUILD_DIR" \
          -R "$TEST_REGEX" \
          -j1 \
          --output-on-failure
  ) >"$logfile" 2>&1 &
  pids+=("$!")
done

status=0
summary_log="${LOG_DIR}/parallel_summary.log"
: > "$summary_log"
for idx in "${!pids[@]}"; do
  pid="${pids[$idx]}"
  inst=$((idx + 1))
  if ! wait "$pid"; then
    echo "[error] instance ${inst} failed" | tee -a "$summary_log"
    status=1
  else
    echo "[ok] instance ${inst} passed" | tee -a "$summary_log"
  fi
done

if [[ "$status" -ne 0 ]]; then
  echo "[fail] Parallel phase had failures. See ${LOG_DIR}/parallel_*.log"
  exit 1
fi

echo "[phase] Stress: until-fail:${STRESS} (sequential)"
stress_log="${LOG_DIR}/stress_until_fail_${STRESS}.log"
if ctest --test-dir "$BUILD_DIR" \
         -R "$TEST_REGEX" \
         --repeat until-fail:"$STRESS" \
         -j1 \
         --output-on-failure | tee "$stress_log"; then
  echo "[ok] Stress phase completed ${STRESS} successful run(s)" | tee -a "$stress_log"
else
  echo "[fail] Stress phase encountered a failure before reaching ${STRESS} runs" | tee -a "$stress_log"
  exit 1
fi

exit 0
