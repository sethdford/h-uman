#!/bin/bash
#
# run-dpo-training.sh — Wrapper around dpo_mlx_train.py for manual/standalone training runs.
#
# PRODUCTION PATH: For automated training via the daemon, use scripts/training_loop.py
# directly (spawned by src/agent/lora_training_runner.c:385). This wrapper is for
# manual testing and iteration.
#
# Usage:
#   bash scripts/run-dpo-training.sh \
#     --model <hf_model_id> \
#     --data <jsonl_or_dir> \
#     --adapter-path <dir> \
#     --iters <N> \
#     [--beta <beta>] [--scale <scale>]
#
# On completion:
#   1. Records results to ~/.human/logs/dpo-training-results.jsonl
#   2. Runs regression verdict
#   3. Exits non-zero if regression FAIL (training made things worse)
#

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY="${PY:-python3}"
RESULTS_FILE="${RESULTS_FILE:-$HOME/.human/logs/dpo-training-results.jsonl}"

# Parse arguments
MODEL=""
DATA=""
ADAPTER_PATH=""
ITERS=""
BETA="0.1"
SCALE="2.0"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --model)        MODEL="$2"; shift 2 ;;
    --data)         DATA="$2"; shift 2 ;;
    --adapter-path) ADAPTER_PATH="$2"; shift 2 ;;
    --iters)        ITERS="$2"; shift 2 ;;
    --beta)         BETA="$2"; shift 2 ;;
    --scale)        SCALE="$2"; shift 2 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

# Validate required args
for var in MODEL DATA ADAPTER_PATH ITERS; do
  if [ -z "${!var}" ]; then
    echo "ERROR: --${var,,} is required" >&2
    exit 1
  fi
done

echo "[run-dpo-training] Starting DPO training..."
echo "[run-dpo-training] model=$MODEL data=$DATA adapter-path=$ADAPTER_PATH iters=$ITERS scale=$SCALE"

# Run the actual training, capturing stdout/stderr to a temp file
TRAIN_LOG="/tmp/dpo_train_$$.log"
trap "rm -f '$TRAIN_LOG'" EXIT INT TERM

# Run training wrapper and capture output
"$PY" "$REPO_ROOT/scripts/dpo_mlx_train.py" \
  --model "$MODEL" \
  --data "$DATA" \
  --adapter-path "$ADAPTER_PATH" \
  --iters "$ITERS" \
  --beta "$BETA" \
  --scale "$SCALE" \
  2>&1 | tee "$TRAIN_LOG"

TRAIN_EXIT=${PIPESTATUS[0]}
if [ $TRAIN_EXIT -ne 0 ]; then
  echo "[run-dpo-training] Training failed with exit code $TRAIN_EXIT" >&2
  exit $TRAIN_EXIT
fi

echo "[run-dpo-training] Training completed. Recording results..."

# Extract metrics from training log (mlx-lm-lora outputs "Iter N: loss X.XXX...")
# Parse last loss and val loss lines
TRAIN_LOSS="null"
VAL_LOSS="null"

# Find the last non-validation loss line (it will be the final training loss)
TRAIN_LOSS=$(grep -E "^Iter [0-9]+:.*\bloss\b" "$TRAIN_LOG" | tail -1 | sed -E 's/.*\bloss\s+([0-9.e-]+).*/\1/' || echo "null")

# Find validation loss if present
VAL_LOSS=$(grep -E "Val loss" "$TRAIN_LOG" | tail -1 | sed -E 's/.*Val loss\s+([0-9.e-]+).*/\1/' || echo "null")

# Count DPO pairs by source from the data file (simple heuristic)
N_PAIRS_JSON='{}'
if [ -f "$DATA" ]; then
  # Count total lines in the JSONL file as proxy for pairs
  N_PAIRS=$(wc -l < "$DATA" || echo "0")
  N_PAIRS_JSON='{"training_data": '"$N_PAIRS"'}'
fi

# Get git commit
GIT_COMMIT=$(cd "$REPO_ROOT" && git rev-parse HEAD 2>/dev/null || echo "unknown")

# Get adapter ID from the path
ADAPTER_ID=$(basename "$ADAPTER_PATH")

echo "[run-dpo-training] Appending to $RESULTS_FILE..."
"$PY" "$REPO_ROOT/scripts/dpo_results.py" \
  --results-file "$RESULTS_FILE" \
  --append "$ADAPTER_ID" "$N_PAIRS_JSON" "$TRAIN_LOSS" "$VAL_LOSS" "null" "$SCALE" "$ITERS" "$GIT_COMMIT"

# Run regression check
echo "[run-dpo-training] Running regression verdict..."
VERDICT=$("$PY" "$REPO_ROOT/scripts/dpo_results.py" --results-file "$RESULTS_FILE" --check "$VAL_LOSS" 2>/dev/null || echo "unknown")

echo "[run-dpo-training] Regression verdict: $VERDICT"

if [ "$VERDICT" = "FAIL" ]; then
  echo "[run-dpo-training] FAIL: Training regression detected. Val loss worse than prior 4 weeks." >&2
  exit 1
fi

echo "[run-dpo-training] SUCCESS: Training completed and passed regression check."
exit 0
