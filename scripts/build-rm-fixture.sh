#!/usr/bin/env bash
# scripts/build-rm-fixture.sh — Phase 4 Task 10 (RL SOTA)
#
# Regenerates tests/fixtures/rm_synthetic_checkpoint/ by training a tiny
# HUML reward model on the existing Phase 2 synthetic preference-pair
# fixture (tests/fixtures/synthetic_preference_pairs_huml.jsonl) and
# saving it via Phase 3's hu_reward_model_save.
#
# The output is committed to the repo as a ~5 KB fixture; this script
# is the "regenerate from source" recipe used when the checkpoint
# format or training schedule changes. The Phase 4 Task 10 acceptance
# criteria are:
#   1. Run once → fixture appears at tests/fixtures/rm_synthetic_checkpoint/
#   2. tests/test_cli_grpo.c::test_cli_grpo_rm_backed_reward_loads_phase3_checkpoint
#      consumes the fixture and exits HU_OK.
#
# Schema (see src/ml/reward_model.c::hu_reward_model_save + Phase 3
# Task 1 hu_value_head_save):
#   <dir>/value_head.vh  — "VHED" magic + u32 hidden_dim + float[hd] W + float b
#   <dir>/rm_meta.json   — {"vocab_size":32,"hidden_dim":32,"backend":"huml"}
#
# Total on-disk size: ~145 bytes (32 f32 weights + 4-byte bias + header
# in value_head.vh; ~50-byte rm_meta.json). Far below the 5 KB cap
# named in the plan.
#
# Determinism: the value_head is xavier-initialized in
# hu_reward_model_create_huml and the Bradley-Terry SGD step is
# deterministic (no random sampling), so the fixture bytes are
# reproducible given the same fixture pairs + iteration count.

set -euo pipefail

cd "$(dirname "$0")/.."

if [ ! -x build-rl-sota/human ]; then
    echo "ERROR: build-rl-sota/human not found." >&2
    echo "  Build first: cmake --build --preset rl_sota -j8" >&2
    exit 1
fi

PAIRS=tests/fixtures/synthetic_preference_pairs_huml.jsonl
OUT=tests/fixtures/rm_synthetic_checkpoint

if [ ! -f "$PAIRS" ]; then
    echo "ERROR: $PAIRS missing — Phase 2 fixture is the input." >&2
    exit 1
fi

mkdir -p "$OUT"

# A small iteration budget keeps the value head close to the xavier
# init (the loaded checkpoint just needs to parse + score, not produce
# meaningful preference signal — the GRPO E2E test uses the rewards
# only to compute advantages, and any non-degenerate value head works).
./build-rl-sota/human ml rm-train \
    --pairs "$PAIRS" \
    --backend huml \
    --save "$OUT" \
    --iters 20 \
    --learning-rate 0.01 \
    --vocab-size 32

echo
echo "Fixture written to $OUT:"
ls -la "$OUT"
