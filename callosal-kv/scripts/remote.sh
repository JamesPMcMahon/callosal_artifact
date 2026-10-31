#!/usr/bin/env bash
# Run a command on the configured CloudLab node inside the synced directory.
#
# Usage: scripts/remote.sh <cmd> [args...]
# Example: scripts/remote.sh make test
# Example: scripts/remote.sh ./callosal-kv --help
#
# Reads CLOUDLAB_HOST, CLOUDLAB_USER, CLOUDLAB_REMOTE_DIR from config.env.

set -euo pipefail

cd "$(dirname "$0")/.."

if [ -f config.env ]; then
    # shellcheck disable=SC1091
    . config.env
fi

: "${CLOUDLAB_HOST:?CLOUDLAB_HOST is required (copy config.env.example to config.env and edit)}"
: "${CLOUDLAB_USER:=$USER}"
: "${CLOUDLAB_REMOTE_DIR:=callosal-kv}"

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <cmd> [args...]" >&2
    exit 2
fi

remote="${CLOUDLAB_USER}@${CLOUDLAB_HOST}"

# Quote each argument so it survives the ssh boundary with spaces / quotes intact.
cmd=""
for arg in "$@"; do
    cmd+="$(printf '%q ' "$arg")"
done

exec ssh -A "$remote" "cd ${CLOUDLAB_REMOTE_DIR} && ${cmd}"
