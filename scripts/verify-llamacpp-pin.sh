#!/usr/bin/env bash
# Phase 1 (RL SOTA) — guard against accidental llama.cpp drift.
# Fails non-zero if the vendored submodule HEAD does not match the
# pinned SHA in third_party/llama.cpp.sha256.
#
# Usage:
#   bash scripts/verify-llamacpp-pin.sh
#
# Exit codes:
#   0  pin matches
#   1  pin file missing, submodule uninitialized, or SHA drift
set -euo pipefail

PIN_FILE="third_party/llama.cpp.sha256"
SUBMODULE_DIR="third_party/llama.cpp"

if [[ ! -f "$PIN_FILE" ]]; then
    echo "[verify-llamacpp-pin] FAIL: $PIN_FILE missing"
    exit 1
fi
if [[ ! -d "$SUBMODULE_DIR/.git" && ! -f "$SUBMODULE_DIR/.git" ]]; then
    echo "[verify-llamacpp-pin] FAIL: submodule not initialized at $SUBMODULE_DIR"
    echo "[verify-llamacpp-pin] hint: git submodule update --init --recursive"
    exit 1
fi

EXPECTED="$(head -n 1 "$PIN_FILE")"
ACTUAL="$(git -C "$SUBMODULE_DIR" rev-parse HEAD)"

if [[ "$EXPECTED" != "$ACTUAL" ]]; then
    echo "[verify-llamacpp-pin] FAIL: vendored llama.cpp drifted"
    echo "  expected: $EXPECTED  ($(sed -n '2p' "$PIN_FILE"))"
    echo "  actual:   $ACTUAL"
    exit 1
fi
echo "[verify-llamacpp-pin] OK: $ACTUAL ($(sed -n '2p' "$PIN_FILE"))"
