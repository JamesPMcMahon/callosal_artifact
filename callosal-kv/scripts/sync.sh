#!/usr/bin/env bash
# Sync the local working tree to the configured CloudLab node.
#
# Reads CLOUDLAB_HOST, CLOUDLAB_USER, CLOUDLAB_REMOTE_DIR from config.env.
# Forwards the SSH agent so remote operations can reach the local keys.
# Excludes local build artifacts, git metadata, and the config file itself.

set -euo pipefail

cd "$(dirname "$0")/.."

if [ -f config.env ]; then
    # shellcheck disable=SC1091
    . config.env
fi

: "${CLOUDLAB_HOST:?CLOUDLAB_HOST is required (copy config.env.example to config.env and edit)}"
: "${CLOUDLAB_USER:=$USER}"
: "${CLOUDLAB_REMOTE_DIR:=callosal-kv}"

remote="${CLOUDLAB_USER}@${CLOUDLAB_HOST}"

ssh -A "$remote" "mkdir -p ${CLOUDLAB_REMOTE_DIR}"

rsync -az --delete \
    --exclude '/build/' \
    --exclude '/.git/' \
    --exclude '/config.env' \
    --exclude '/runs/' \
    --exclude '/callosal-kv' \
    --exclude '/harness/.venv/' \
    --exclude '/harness/.ruff_cache/' \
    --exclude '/harness/.mypy_cache/' \
    --exclude '/harness/__pycache__/' \
    -e 'ssh -A' \
    ./ "${remote}:${CLOUDLAB_REMOTE_DIR}/"

echo "synced to ${remote}:${CLOUDLAB_REMOTE_DIR}"
