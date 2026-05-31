#!/usr/bin/env bash
# Gold-standard single-run E2E proof of the on-device reaction→learn loop.
#
# In ONE invocation, with real production code at every step:
#
#   real reactions (hu_reaction_handler_handle_event)
#     → single-sided rows in a real SQLite collector DB
#     → hu_dpo_export_paired  (INSIDE `human ml dpo-train`)
#     → mlx_lm_lora DPO fuse   (LoRA scale pinned to 2.0)
#     → a real LoRA adapter on disk, verified to exist AND be scale-safe.
#
# Runs entirely locally on Apple Silicon. All run artifacts go to a throwaway
# mktemp dir (GOLD_DIR); NOTE: $HOME is intentionally NOT redirected, because
# mlx_lm_lora lives in the user's Python site-packages and is invisible under a
# fake HOME — so a stray `human` invocation here could read/write the real
# ~/.human. Defaults to the smallest cached MLX model + 1 iteration so the whole
# chain finishes in a few minutes.
#
# Usage:  bash scripts/e2e-reaction-to-adapter-gold.sh
# Env:    GOLD_MODEL (default mlx-community/gemma-4-e2b-it-4bit)
#         GOLD_ITERS (default 1)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

MODEL="${GOLD_MODEL:-mlx-community/gemma-4-e2b-it-4bit}"
ITERS="${GOLD_ITERS:-1}"
HUMAN_BIN="${HUMAN_BIN:-$ROOT/build/human}"
TESTS_BIN="${TESTS_BIN:-$ROOT/build/human_tests}"

test -x "$HUMAN_BIN"  || { echo "FAIL: $HUMAN_BIN missing (build first: cmake --build build --target human)"; exit 1; }
test -x "$TESTS_BIN"  || { echo "FAIL: $TESTS_BIN missing (build first: cmake --build build --target human_tests)"; exit 1; }

GOLD_DIR="$(mktemp -d -t hu-gold-XXXXXX)"
GOLD_PAIRS="$GOLD_DIR/pairs.jsonl"
GOLD_ADAPTER="$GOLD_DIR/adapter-out"
mkdir -p "$GOLD_ADAPTER"
cleanup() { rm -rf "$GOLD_DIR"; }
trap cleanup EXIT

echo "==> [1/4] Real reactions → hu_dpo_export_paired → reaction-derived pairs (JSONL)"
# The e2e test drives 6 single-sided reactions through the real handler and
# runs the REAL hu_dpo_export_paired; HU_E2E_PAIRED_JSONL_OUT makes it dump the
# resulting two-sided pairs so we can fuse them. (We use --pairs rather than the
# SQLite path so the real $HOME is preserved — mlx_lm_lora lives in the user's
# Python site-packages and is invisible under a redirected HOME.)
HU_E2E_PAIRED_JSONL_OUT="$GOLD_PAIRS" "$TESTS_BIN" --filter=reaction_stream_becomes_trainable \
    >/dev/null 2>&1 \
    || { echo "FAIL: reaction corpus generation (the e2e test did not pass)"; exit 1; }
test -s "$GOLD_PAIRS" || { echo "FAIL: paired JSONL not produced at $GOLD_PAIRS"; exit 1; }
NPAIRS=$(grep -c . "$GOLD_PAIRS" 2>/dev/null || echo 0)
echo "    reaction-derived two-sided pairs (via hu_dpo_export_paired): $NPAIRS"
test "${NPAIRS:-0}" -ge 3 || { echo "FAIL: expected >=3 paired rows, got ${NPAIRS:-0}"; exit 1; }
echo "    sample: $(head -1 "$GOLD_PAIRS")"

echo "==> [2/4] human ml dpo-train --backend mlx --pairs <reaction pairs>  (→ mlx_lm_lora DPO, scale 2.0)"
echo "    model=$MODEL iters=$ITERS adapter-out=$GOLD_ADAPTER"
set +e
"$HUMAN_BIN" ml dpo-train \
    --backend mlx \
    --pairs "$GOLD_PAIRS" \
    --model "$MODEL" \
    --iters "$ITERS" \
    --adapter-out "$GOLD_ADAPTER" >"$GOLD_DIR/train.log" 2>&1
TRAIN_RC=$?
set -e
echo "    --- dpo-train tail ---"
tail -25 "$GOLD_DIR/train.log" | sed 's/^/    /'
echo "    dpo-train exit code: $TRAIN_RC"
test "$TRAIN_RC" -eq 0 || { echo "FAIL: dpo-train exited with code $TRAIN_RC"; exit 1; }

echo "==> [3/4] Verifying a real adapter was produced"
SAFET="$GOLD_ADAPTER/adapters.safetensors"
test -s "$SAFET" || { echo "FAIL: $SAFET missing or empty after fuse"; exit 1; }
echo "    adapter: $SAFET ($(wc -c <"$SAFET" | tr -d ' ') bytes)"

echo "==> [4/4] Verifying the adapter is scale-safe (<=8.0 ceiling; pinned to 2.0)"
CFG="$GOLD_ADAPTER/adapter_config.json"
test -f "$CFG" || { echo "FAIL: $CFG missing"; exit 1; }
SCALE=$(python3 -c "import json;c=json.load(open('$CFG'));print(c.get('lora_parameters',{}).get('scale', c.get('scale','?')))")
echo "    adapter_config.json scale = $SCALE"
python3 -c "import sys; s=float('$SCALE'); sys.exit(0 if s<=8.0 else 1)" \
    || { echo "FAIL: scale $SCALE exceeds the 8.0 ceiling (lora-scale-default-or-die.md)"; exit 1; }

echo
echo "✅ GOLD-STANDARD E2E PASS"
echo "   reaction stream → hu_dpo_export_paired → mlx_lm_lora DPO fuse → safe adapter (scale=$SCALE)"
