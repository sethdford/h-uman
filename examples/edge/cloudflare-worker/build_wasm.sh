#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../../.." && pwd)"
WORKER_DIR="$ROOT_DIR/examples/edge/cloudflare-worker"

mkdir -p "$WORKER_DIR/dist"

clang --target=wasm32 -nostdlib -O2 \
  -Wl,--no-entry -Wl,--export=choose_policy \
  -o "$WORKER_DIR/dist/agent_core.wasm" \
  "$WORKER_DIR/agent_core.c"

echo "Built $WORKER_DIR/dist/agent_core.wasm"
