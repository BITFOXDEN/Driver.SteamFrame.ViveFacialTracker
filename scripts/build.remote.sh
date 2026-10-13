#!/usr/bin/env bash
# Build vftstream ON the Steam Frame, over SSH.
#
# Usage (Developer Mode on, password set under Settings > Developer):
#   scripts/build.remote.sh [user@host]
#
# Stages the source and builds it on-device. See README.md ("Build & deploy")
# for installing the user-level systemd service.
set -euo pipefail

HOST=${1:-steamos@frame}
REMOTE_DIR=~/vftstream-src

echo "==> staging source"
ssh "$HOST" "mkdir -p $REMOTE_DIR"
scp -r src Makefile "$HOST:$REMOTE_DIR/"

echo "==> building"
ssh "$HOST" "cd $REMOTE_DIR && make clean && make -j\$(nproc)"

echo
echo "Done. The binary is at $REMOTE_DIR/vftstream."
echo "See README.md for the systemd user-service install steps."
echo "Check the running service with: curl http://frame:8097/status"
