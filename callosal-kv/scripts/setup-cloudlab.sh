#!/usr/bin/env bash
# Prepare a fresh CloudLab Ubuntu 24.04 node to build and run callosal-kv.
#
# Idempotent — re-running on an already-configured node is a no-op.
# Intended invocation from a laptop:
#     make remote-setup
# ...which rsyncs this tree to the remote, then runs `make setup` there, which
# runs this script.
#
# Assumes:
#   - passwordless sudo (CloudLab default)
#   - network access to Ubuntu archives

set -euo pipefail

echo "[setup-cloudlab] apt-get update"
sudo apt-get update -qq

echo "[setup-cloudlab] installing toolchain"
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential \
    clang-18 \
    clang-tidy-18 \
    clang-format-18 \
    curl \
    python3 \
    python3-venv \
    rsync \
    jq

echo "[setup-cloudlab] registering clang-18 as default /usr/bin/clang"
sudo update-alternatives --install /usr/bin/clang        clang        /usr/bin/clang-18        100
sudo update-alternatives --install /usr/bin/clang-tidy   clang-tidy   /usr/bin/clang-tidy-18   100
sudo update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-18 100

echo "[setup-cloudlab] done"
clang --version | head -1
clang-tidy --version | head -2 | tail -1
