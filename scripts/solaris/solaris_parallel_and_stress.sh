#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"/../..

if [[ -f scripts/solaris/.env ]]; then
  # shellcheck disable=SC1091
  source scripts/solaris/.env
else
  # shellcheck disable=SC1091
  source scripts/solaris/.env.example
fi

PARALLEL="${1:-4}"
COUNT="${2:-300}"

ssh -p "${SOLARIS_SSH_PORT}" ${SOLARIS_SSH_OPTS} "${SOLARIS_SSH}" bash -s -- \
  "${SOLARIS_DIR}" \
  "${SOLARIS_BUILD}" \
  "${SOLARIS_CTEST}" \
  "${PARALLEL}" \
  "${COUNT}" \
  "${SOLARIS_CTEST_TIMEOUT:-180}" \
  "${SOLARIS_STRESS_WALL:-900}" \
  "${SOLARIS_STRESS_N:-}" \
  "${SOLARIS_STRESS_M:-}" <<'REMOTE_SCRIPT'
set -euo pipefail
SOLARIS_DIR="$1"; SOLARIS_BUILD="$2"; SOLARIS_CTEST="$3"; PARALLEL="$4"; COUNT="$5"; SOLARIS_CTEST_TIMEOUT="$6"; SOLARIS_STRESS_WALL="$7"; SOLARIS_STRESS_N="${8:-}"; SOLARIS_STRESS_M="${9:-}"
cd "$SOLARIS_DIR"

LOG_DIR="$SOLARIS_BUILD"
echo "[remote] Parallel phase: ${PARALLEL} instance(s)" | tee -a "$LOG_DIR/ctest_parallel_summary.log"

declare -a PIDS=()
for i in $(seq 1 "$PARALLEL"); do
  LOG_FILE="$LOG_DIR/ctest_parallel_${i}.log"
  echo "[remote] starting instance #$i -> $LOG_FILE" | tee -a "$LOG_DIR/ctest_parallel_summary.log"
  (
    "$SOLARIS_CTEST" --test-dir "$SOLARIS_BUILD" -R NetHighload.ManyClientsEchoNoBlock -j1 --output-on-failure --timeout "$SOLARIS_CTEST_TIMEOUT"
  ) >"$LOG_FILE" 2>&1 &
  PIDS+=("$!")
done

PAR_STATUS=0
for idx in "${!PIDS[@]}"; do
  pid="${PIDS[$idx]}"; inst=$((idx+1))
  if ! wait "$pid"; then
    echo "[remote][error] instance ${inst} failed" | tee -a "$LOG_DIR/ctest_parallel_summary.log"
    PAR_STATUS=1
  else
    echo "[remote][ok] instance ${inst} passed" | tee -a "$LOG_DIR/ctest_parallel_summary.log"
  fi
done

if [[ "$PAR_STATUS" -ne 0 ]]; then
  echo "[remote][fail] Parallel phase had failures. See ctest_parallel_*.log" | tee -a "$LOG_DIR/ctest_parallel_summary.log"
  exit 1
fi

STRESS_LOG="$LOG_DIR/ctest_stress_highload.log"
echo "[remote] Stress phase: NetHighload.ManyClientsEchoNoBlock x${COUNT} (seq). Log: $STRESS_LOG; wall ${SOLARIS_STRESS_WALL}s"
# Allow tuning test payload via env to keep runtime bounded
ENV_PREFIX=""
if [[ -n "$SOLARIS_STRESS_N" ]]; then ENV_PREFIX+="IO_STRESS_N=$SOLARIS_STRESS_N "; fi
if [[ -n "$SOLARIS_STRESS_M" ]]; then ENV_PREFIX+="IO_STRESS_M=$SOLARIS_STRESS_M "; fi
 # Start stress in this shell to make 'wait' work reliably across shells
 if [[ -n "$ENV_PREFIX" ]]; then
   eval ${ENV_PREFIX} "$SOLARIS_CTEST" --test-dir "$SOLARIS_BUILD" -R NetHighload.ManyClientsEchoNoBlock -j1 --repeat "until-fail:${COUNT}" --output-on-failure --timeout "$SOLARIS_CTEST_TIMEOUT" >>"$STRESS_LOG" 2>&1 &
 else
   "$SOLARIS_CTEST" --test-dir "$SOLARIS_BUILD" -R NetHighload.ManyClientsEchoNoBlock -j1 --repeat "until-fail:${COUNT}" --output-on-failure --timeout "$SOLARIS_CTEST_TIMEOUT" >>"$STRESS_LOG" 2>&1 &
 fi
 STRESS_PID=$!
START=$(date +%s)
RC=0
if [[ -n "$STRESS_PID" ]]; then
  while kill -0 "$STRESS_PID" 2>/dev/null; do
    NOW=$(date +%s)
    ELAP=$((NOW-START))
    if [[ "$ELAP" -ge "$SOLARIS_STRESS_WALL" ]]; then
      echo "[remote][warn] stress exceeded wall ${SOLARIS_STRESS_WALL}s; sending SIGTERM to pid $STRESS_PID" | tee -a "$STRESS_LOG"
      kill "$STRESS_PID" || true
      sleep 10
      if kill -0 "$STRESS_PID" 2>/dev/null; then
        echo "[remote][warn] SIGTERM ineffective; sending SIGKILL" | tee -a "$STRESS_LOG"
        kill -9 "$STRESS_PID" || true
      fi
      RC=124
      break
    fi
    sleep 2
  done
  if [[ "$RC" -eq 0 ]]; then
    if ! wait "$STRESS_PID"; then RC=$?; fi
  fi
else
  echo "[remote][error] cannot determine stress pid" | tee -a "$STRESS_LOG"
  RC=1
fi
echo "[remote] stress exit code: $RC (see $STRESS_LOG)"
exit "$RC"
REMOTE_SCRIPT
