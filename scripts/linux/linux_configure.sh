#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"/../..

if [[ -f scripts/linux/.env ]]; then
  source scripts/linux/.env
else
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

ssh -p "${LINUX_SSH_PORT}" ${LINUX_SSH_OPTS} "${LINUX_SSH}" bash -s -- \
  "${LINUX_DIR}" \
  "${LINUX_BUILD}" \
  "${LINUX_CMAKE}" \
  "${LINUX_BUILD_TYPE}" \
  "${LINUX_IOURING}" \
  "${LINUX_VERBOSE_IOURING:-OFF}" \
  "${LINUX_ASAN:-OFF}" <<'REMOTE_SCRIPT'
set -euo pipefail
LINUX_DIR="$1"; LINUX_BUILD="$2"; LINUX_CMAKE="$3"; LINUX_BUILD_TYPE="$4"; LINUX_IOURING="$5"; LINUX_VERBOSE_IOURING="$6"; LINUX_ASAN="$7"
mkdir -p "$LINUX_DIR" && cd "$LINUX_DIR"
mkdir -p "$LINUX_BUILD"
"$LINUX_CMAKE" -S . -B "$LINUX_BUILD" \
  -DCMAKE_BUILD_TYPE="$LINUX_BUILD_TYPE" \
  -DIO_BUILD_TESTS=ON -DIO_BUILD_EXAMPLES=ON \
  -DIO_WITH_IOURING="$LINUX_IOURING" \
  -DIO_ENABLE_IOURING_VERBOSE="$LINUX_VERBOSE_IOURING" \
  -DIO_ENABLE_ASAN="$LINUX_ASAN"
REMOTE_SCRIPT

echo "Configured on ${LINUX_SSH} in ${LINUX_DIR}/${LINUX_BUILD} (IO_URING=${LINUX_IOURING}, VERBOSE_IOURING=${LINUX_VERBOSE_IOURING:-OFF}, ASAN=${LINUX_ASAN:-OFF})"
