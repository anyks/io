#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"/../..

if [[ -f scripts/solaris/.env ]]; then
  source scripts/solaris/.env
else
  source scripts/solaris/.env.example
fi

ssh -p "${SOLARIS_SSH_PORT}" ${SOLARIS_SSH_OPTS} "${SOLARIS_SSH}" bash -s -- \
  "${SOLARIS_DIR}" \
  "${SOLARIS_BUILD}" \
  "${SOLARIS_CMAKE}" \
  "${SOLARIS_BUILD_TYPE}" \
  "${SOLARIS_EVENTPORTS}" <<'REMOTE_SCRIPT'
set -euo pipefail
SOLARIS_DIR="$1"; SOLARIS_BUILD="$2"; SOLARIS_CMAKE="$3"; SOLARIS_BUILD_TYPE="$4"; SOLARIS_EVENTPORTS="$5"
mkdir -p "$SOLARIS_DIR" && cd "$SOLARIS_DIR"
mkdir -p "$SOLARIS_BUILD"
"$SOLARIS_CMAKE" -S . -B "$SOLARIS_BUILD" \
  -DCMAKE_BUILD_TYPE="$SOLARIS_BUILD_TYPE" \
  -DIO_BUILD_TESTS=ON -DIO_BUILD_EXAMPLES=ON -DIO_BUILD_BENCHMARKS=ON \
  -DIO_WITH_EVENTPORTS="$SOLARIS_EVENTPORTS"
REMOTE_SCRIPT

echo "Configured on ${SOLARIS_SSH} in ${SOLARIS_DIR}/${SOLARIS_BUILD}"
