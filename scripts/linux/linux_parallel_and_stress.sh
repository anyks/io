#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"/../..

if [[ -f scripts/linux/.env ]]; then
  # shellcheck disable=SC1091
  source scripts/linux/.env
else
  # shellcheck disable=SC1091
  source scripts/linux/.env.example
fi

# Strictly enforce SSH port 221 and required vars
: "${LINUX_SSH:?LINUX_SSH is required}"
: "${LINUX_SSH_PORT:?LINUX_SSH_PORT is required}"
: "${LINUX_DIR:?LINUX_DIR is required}"
: "${LINUX_BUILD:?LINUX_BUILD is required}"
if [[ "${LINUX_SSH_PORT}" != "221" ]]; then
  echo "ERROR: LINUX_SSH_PORT must be 221; got '${LINUX_SSH_PORT}'." >&2
  exit 2
fi

PARALLEL="${1:-4}"
COUNT="${2:-200}"

ssh -p "${LINUX_SSH_PORT}" ${LINUX_SSH_OPTS} "${LINUX_SSH}" bash -s -- \
  "${LINUX_DIR}" \
  "${LINUX_BUILD}" \
  "${LINUX_CTEST}" \
  "${PARALLEL}" \
  "${COUNT}" \
  "${LINUX_CTEST_TIMEOUT:-180}" \
  "${LINUX_STRESS_WALL:-600}" \
  "${LINUX_STRESS_N:-}" \
  "${LINUX_STRESS_M:-}" <<'REMOTE_SCRIPT'
set -euo pipefail
LINUX_DIR="$1"; LINUX_BUILD="$2"; LINUX_CTEST="$3"; PARALLEL="$4"; COUNT="$5"; LINUX_CTEST_TIMEOUT="$6"; LINUX_STRESS_WALL="$7"; LINUX_STRESS_N="${8:-}"; LINUX_STRESS_M="${9:-}"
cd "$LINUX_DIR"

LOG_DIR="$LINUX_BUILD"
echo "[remote] Parallel phase: ${PARALLEL} instance(s)" | tee -a "$LOG_DIR/ctest_parallel_summary.log"

declare -a PIDS=()
for i in $(seq 1 "$PARALLEL"); do
  LOG_FILE="$LOG_DIR/ctest_parallel_${i}.log"
  echo "[remote] starting instance #$i -> $LOG_FILE" | tee -a "$LOG_DIR/ctest_parallel_summary.log"
  (
    "$LINUX_CTEST" --test-dir "$LINUX_BUILD" -R NetHighload.ManyClientsEchoNoBlock -j1 --output-on-failure --timeout "$LINUX_CTEST_TIMEOUT"
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
echo "[remote] Stress phase: NetHighload.ManyClientsEchoNoBlock x${COUNT} (seq). Log: $STRESS_LOG; wall ${LINUX_STRESS_WALL}s"
ENV_PREFIX=""
if [[ -n "$LINUX_STRESS_N" ]]; then ENV_PREFIX+="IO_STRESS_N=$LINUX_STRESS_N "; fi
if [[ -n "$LINUX_STRESS_M" ]]; then ENV_PREFIX+="IO_STRESS_M=$LINUX_STRESS_M "; fi
# Start stress in the current shell (not a subshell) to allow 'wait' on the PID
if [[ -n "$ENV_PREFIX" ]]; then
  eval ${ENV_PREFIX} "$LINUX_CTEST" --test-dir "$LINUX_BUILD" -R NetHighload.ManyClientsEchoNoBlock -j1 --repeat "until-fail:${COUNT}" --output-on-failure --timeout "$LINUX_CTEST_TIMEOUT" >>"$STRESS_LOG" 2>&1 &
else
  "$LINUX_CTEST" --test-dir "$LINUX_BUILD" -R NetHighload.ManyClientsEchoNoBlock -j1 --repeat "until-fail:${COUNT}" --output-on-failure --timeout "$LINUX_CTEST_TIMEOUT" >>"$STRESS_LOG" 2>&1 &
fi
STRESS_PID=$!
START=$(date +%s)
RC=0
if [[ -n "$STRESS_PID" ]]; then
  while kill -0 "$STRESS_PID" 2>/dev/null; do
    NOW=$(date +%s)
    ELAP=$((NOW-START))
    if [[ "$ELAP" -ge "$LINUX_STRESS_WALL" ]]; then
      echo "[remote][warn] stress exceeded wall ${LINUX_STRESS_WALL}s; sending SIGTERM to pid $STRESS_PID" | tee -a "$STRESS_LOG"
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
    wait "$STRESS_PID" || RC=$?
  fi
else
  echo "[remote][error] cannot determine stress pid" | tee -a "$STRESS_LOG"
  RC=1
fi
echo "[remote] stress exit code: $RC (see $STRESS_LOG)"
exit "$RC"
REMOTE_SCRIPT
