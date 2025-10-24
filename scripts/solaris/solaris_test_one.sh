#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"/../..

if [[ -f scripts/solaris/.env ]]; then
  source scripts/solaris/.env
else
  source scripts/solaris/.env.example
fi

PATTERN=${1:-}
if [[ -z "$PATTERN" ]]; then
  echo "Usage: $0 <gtest_regex>" >&2
  exit 2
fi

ssh -p "${SOLARIS_SSH_PORT}" ${SOLARIS_SSH_OPTS} "${SOLARIS_SSH}" bash -s -- \
  "${SOLARIS_DIR}" \
  "${SOLARIS_BUILD}" \
  "${SOLARIS_CTEST}" \
  "$PATTERN" \
  "${SOLARIS_CTEST_TIMEOUT:-180}" \
  "${IO_STRESS_N:-}" \
  "${IO_STRESS_M:-}" \
  "${IO_BENCH_CLIENTS:-}" \
  "${IO_BENCH_MSGS:-}" \
  "${IO_BENCH_PIPELINE:-}" \
  "${IO_BENCH_PROGRESS_SEC:-}" \
  "${IO_BENCH_DATASET_DIR:-}" \
  "${IO_BENCH_SEED:-}" \
  "${IO_BENCH_MIN_MB:-}" \
  "${IO_BENCH_MAX_MB:-}" \
  "${IO_BENCH_MIN_BYTES:-}" \
  "${IO_BENCH_MAX_BYTES:-}" \
  "${IO_BENCH_BUF:-}" \
  "${IO_BENCH_TIMEOUT:-}" <<'REMOTE_SCRIPT'
set -euo pipefail
SOLARIS_DIR="$1"; SOLARIS_BUILD="$2"; SOLARIS_CTEST="$3"; PATTERN="$4"; SOLARIS_CTEST_TIMEOUT="$5"; IO_STRESS_N_ARG="${6:-}"; IO_STRESS_M_ARG="${7:-}"
# Optional benchmark env overrides
IO_BENCH_CLIENTS_ARG="${8:-}"; IO_BENCH_MSGS_ARG="${9:-}"; IO_BENCH_PIPELINE_ARG="${10:-}"; IO_BENCH_PROGRESS_SEC_ARG="${11:-}"
IO_BENCH_DATASET_DIR_ARG="${12:-}"; IO_BENCH_SEED_ARG="${13:-}"; IO_BENCH_MIN_MB_ARG="${14:-}"; IO_BENCH_MAX_MB_ARG="${15:-}"
IO_BENCH_MIN_BYTES_ARG="${16:-}"; IO_BENCH_MAX_BYTES_ARG="${17:-}"; IO_BENCH_BUF_ARG="${18:-}"; IO_BENCH_TIMEOUT_ARG="${19:-}"
cd "$SOLARIS_DIR"
# Forward optional stress tunables if provided
if [ -n "${IO_STRESS_N_ARG}" ]; then export IO_STRESS_N="${IO_STRESS_N_ARG}"; fi
if [ -n "${IO_STRESS_M_ARG}" ]; then export IO_STRESS_M="${IO_STRESS_M_ARG}"; fi
# Forward optional benchmark tunables if provided
if [ -n "${IO_BENCH_CLIENTS_ARG}" ]; then export IO_BENCH_CLIENTS="${IO_BENCH_CLIENTS_ARG}"; fi
if [ -n "${IO_BENCH_MSGS_ARG}" ]; then export IO_BENCH_MSGS="${IO_BENCH_MSGS_ARG}"; fi
if [ -n "${IO_BENCH_PIPELINE_ARG}" ]; then export IO_BENCH_PIPELINE="${IO_BENCH_PIPELINE_ARG}"; fi
if [ -n "${IO_BENCH_PROGRESS_SEC_ARG}" ]; then export IO_BENCH_PROGRESS_SEC="${IO_BENCH_PROGRESS_SEC_ARG}"; fi
if [ -n "${IO_BENCH_DATASET_DIR_ARG}" ]; then export IO_BENCH_DATASET_DIR="${IO_BENCH_DATASET_DIR_ARG}"; fi
if [ -n "${IO_BENCH_SEED_ARG}" ]; then export IO_BENCH_SEED="${IO_BENCH_SEED_ARG}"; fi
if [ -n "${IO_BENCH_MIN_MB_ARG}" ]; then export IO_BENCH_MIN_MB="${IO_BENCH_MIN_MB_ARG}"; fi
if [ -n "${IO_BENCH_MAX_MB_ARG}" ]; then export IO_BENCH_MAX_MB="${IO_BENCH_MAX_MB_ARG}"; fi
if [ -n "${IO_BENCH_MIN_BYTES_ARG}" ]; then export IO_BENCH_MIN_BYTES="${IO_BENCH_MIN_BYTES_ARG}"; fi
if [ -n "${IO_BENCH_MAX_BYTES_ARG}" ]; then export IO_BENCH_MAX_BYTES="${IO_BENCH_MAX_BYTES_ARG}"; fi
if [ -n "${IO_BENCH_BUF_ARG}" ]; then export IO_BENCH_BUF="${IO_BENCH_BUF_ARG}"; fi
if [ -n "${IO_BENCH_TIMEOUT_ARG}" ]; then export IO_BENCH_TIMEOUT="${IO_BENCH_TIMEOUT_ARG}"; fi
LOG="$SOLARIS_BUILD/ctest_${PATTERN//[^A-Za-z0-9_.-]/_}.log"
echo "[remote] Running ctest -R '$PATTERN' in $SOLARIS_BUILD. Log: $LOG"
set +e
"$SOLARIS_CTEST" --test-dir "$SOLARIS_BUILD" -R "$PATTERN" --output-on-failure -j1 --timeout "$SOLARIS_CTEST_TIMEOUT" >"$LOG" 2>&1
RC=$?
set -e
if [[ "$RC" -ne 0 ]]; then
  echo "[remote] tail of log ($LOG):"
  tail -2000 "$LOG" || true
fi
echo "[remote] ctest exit code: $RC (see $LOG)"
exit "$RC"
REMOTE_SCRIPT
