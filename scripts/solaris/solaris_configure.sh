#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"/../..

if [[ -f scripts/solaris/.env ]]; then
  source scripts/solaris/.env
else
  source scripts/solaris/.env.example
fi

ssh -tt -p "${SOLARIS_SSH_PORT}" ${SOLARIS_SSH_OPTS} "${SOLARIS_SSH}" bash -s -- \
  "${SOLARIS_DIR}" \
  "${SOLARIS_BUILD}" \
  "${SOLARIS_CMAKE}" \
  "${SOLARIS_GENERATOR}" \
  "${SOLARIS_BUILD_TYPE}" \
  "${SOLARIS_EVENTPORTS}" <<'REMOTE_SCRIPT'
set -euo pipefail
SOLARIS_DIR="$1"; SOLARIS_BUILD="$2"; SOLARIS_CMAKE="$3"; SOLARIS_GENERATOR="$4"; SOLARIS_BUILD_TYPE="$5"; SOLARIS_EVENTPORTS="$6"
mkdir -p "$SOLARIS_DIR" && cd "$SOLARIS_DIR"
mkdir -p "$SOLARIS_BUILD"
"$SOLARIS_CMAKE" -S . -B "$SOLARIS_BUILD" \
  -G "$SOLARIS_GENERATOR" \
  -DCMAKE_BUILD_TYPE="$SOLARIS_BUILD_TYPE" \
  -DIO_BUILD_TESTS=ON -DIO_BUILD_EXAMPLES=ON \
  -DIO_WITH_EVENTPORTS="$SOLARIS_EVENTPORTS"
REMOTE_SCRIPT

echo "Configured on ${SOLARIS_SSH} in ${SOLARIS_DIR}/${SOLARIS_BUILD}"
