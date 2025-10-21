#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"/..

if [[ -f scripts/solaris/.env ]]; then
  source scripts/solaris/.env
else
  source scripts/solaris/.env.example
fi

rsync -avz --delete \
  --exclude ".git/" \
  --exclude "build/" \
  --exclude "_install/" \
  --exclude "_deps/" \
  ./ "${SOLARIS_SSH}:${SOLARIS_DIR}/"

echo "Synced project to ${SOLARIS_SSH}:${SOLARIS_DIR}"
