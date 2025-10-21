#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"/../..

if [[ -f scripts/solaris/.env ]]; then
  source scripts/solaris/.env
else
  source scripts/solaris/.env.example
fi

TARGET=${1:-io_tests}

ssh -tt -p "${SOLARIS_SSH_PORT}" ${SOLARIS_SSH_OPTS} "${SOLARIS_SSH}" bash -s -- \
  "${SOLARIS_DIR}" \
  "${SOLARIS_BUILD}" \
  "$TARGET" <<'REMOTE_SCRIPT'
set -euo pipefail
SOLARIS_DIR="$1"; SOLARIS_BUILD="$2"; TARGET="$3"
cd "$SOLARIS_DIR/$SOLARIS_BUILD"
if [[ ! -x "$TARGET" ]]; then
  echo "Binary $TARGET not found in $SOLARIS_DIR/$SOLARIS_BUILD" >&2
  exit 1
fi
gdb -q --tui --args "$TARGET"
REMOTE_SCRIPT
