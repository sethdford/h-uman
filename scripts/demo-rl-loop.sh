#!/usr/bin/env bash
# scripts/demo-rl-loop.sh — Phase 6 live demo runner (Apple Silicon, not CI).
set -euo pipefail

PERSONA="${PERSONA:-seth}"
METHOD="${METHOD:-dpo}"
BACKEND="${BACKEND:-mlx}"
REACTION_COUNT="${REACTION_COUNT:-200}"
PROMPT="${PROMPT:-what should I do first?}"
DRY_RUN="${DRY_RUN:-0}"

OUT_DIR="${OUT_DIR:-$HOME/.human/proofs/$(date -u +%Y-%m-%d)-${METHOD}-step-$$}"

# DRY_RUN validates the script's logic/paths without executing the MLX demo, so
# it must run anywhere — including the Linux rl_sota nightly. Exit BEFORE the
# Apple-Silicon + built-binary prerequisites, which only a real (MLX) run needs.
if [ "$DRY_RUN" = "1" ]; then
    echo "==> DRY_RUN=1; skipping Apple Silicon + MLX prerequisites and actual invocation"
    echo "==> Output would be: $OUT_DIR"
    exit 0
fi

echo "==> Verifying prerequisites..."
test "$(uname -s)" = "Darwin" \
    || { echo "ERROR: Apple Silicon required. uname=$(uname -s)"; exit 1; }
test "$(uname -m)" = "arm64" \
    || { echo "ERROR: Apple Silicon required. arch=$(uname -m)"; exit 1; }
test -x ./build-rl-sota/human \
    || { echo "ERROR: build-rl-sota/human missing. Run: cmake --preset rl_sota && cmake --build --preset rl_sota -j"; exit 1; }
echo "==> Output: $OUT_DIR"

set -x
./build-rl-sota/human demo rl-closed-loop \
    --persona "${PERSONA}" \
    --method "${METHOD}" \
    --backend "${BACKEND}" \
    --reaction-count "${REACTION_COUNT}" \
    --prompt "${PROMPT}" \
    --out "${OUT_DIR}" \
    --require-positive-delta
EXIT=$?
set +x

echo "==> Demo exit code: $EXIT"
case "$EXIT" in
    0) echo "==> WIN: persona-fidelity delta met threshold." ;;
    2) echo "==> SOFT FAIL: delta computed but missed threshold. See $OUT_DIR/eval_delta.json" ;;
    3) echo "==> HARD FAIL: harness error before delta could be computed." ;;
    *) echo "==> UNKNOWN exit code: $EXIT" ;;
esac
echo "==> Evidence dir: $OUT_DIR"
echo "==> Runbook: docs/demos/rl-loop-demo.md"
exit "$EXIT"
