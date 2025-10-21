#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"/..

if [[ -f scripts/solaris/.env ]]; then
  source scripts/solaris/.env
else
  source scripts/solaris/.env.example
fi

ssh -tt "${SOLARIS_SSH}" bash -lc "'
set -euo pipefail
cd "${SOLARIS_DIR}/${SOLARIS_BUILD}"
echo "Server:"; ./example_server &
sleep 1
echo "Client:"; ./example_client || true
pkill example_server || true
'"
