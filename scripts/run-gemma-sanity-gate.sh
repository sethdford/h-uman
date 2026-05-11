#!/usr/bin/env bash
# Phase 1 (RL SOTA) — stock Gemma sanity gate.
#
# Runs every prompt in tests/fixtures/gemma_sanity_gate_prompts.json
# through the rl_sota build's llamacpp provider, scores each against
# its objective pass criterion, and exits non-zero if any prompt fails.
#
# Usage:
#   bash scripts/run-gemma-sanity-gate.sh
#   HU_GGUF_PATH=/abs/path/to/model.gguf bash scripts/run-gemma-sanity-gate.sh
#   HU_TEST_BIN=./build-rl-sota/human_tests bash scripts/run-gemma-sanity-gate.sh
#
# The pass bar is 18/20 (allow tuning of two prompts whose
# expect_substring may need adjustment for greedy decoding output).
# 20/20 = Gemma is good enough to base Phase 2 (DPO/KTO/GRPO) on.

set -euo pipefail

FIXTURE="${HU_FIXTURE:-tests/fixtures/gemma_sanity_gate_prompts.json}"
GGUF="${HU_GGUF_PATH:-${HOME}/.human/models/gemma-3-4b-it-Q4_K_M.gguf}"
BIN="${HU_TEST_BIN:-./build-rl-sota/human_tests}"
PASS_BAR="${HU_SANITY_PASS_BAR:-18}"

if [[ ! -f "$FIXTURE" ]]; then
    echo "[sanity-gate] FAIL: fixture missing at $FIXTURE"
    exit 1
fi
if [[ ! -f "$GGUF" ]]; then
    echo "[sanity-gate] FAIL: GGUF missing at $GGUF"
    echo "             run: bash scripts/fetch-gemma.sh"
    exit 1
fi
if [[ ! -x "$BIN" ]]; then
    echo "[sanity-gate] FAIL: test binary missing at $BIN"
    echo "             run: cmake --preset rl_sota && cmake --build --preset rl_sota"
    exit 1
fi
if ! command -v jq >/dev/null 2>&1; then
    echo "[sanity-gate] FAIL: jq is required (brew install jq)"
    exit 1
fi

PASS=0
FAIL=0
TOTAL="$(jq -r '.prompts | length' "$FIXTURE")"
echo "[sanity-gate] Running $TOTAL prompts against $GGUF"
echo "[sanity-gate] Pass bar: $PASS_BAR / $TOTAL"

while IFS= read -r prompt_json; do
    id="$(jq -r '.id' <<<"$prompt_json")"
    sys="$(jq -r '.system' <<<"$prompt_json")"
    usr="$(jq -r '.user' <<<"$prompt_json")"
    expect_substr="$(jq -r '.expect_substring // empty' <<<"$prompt_json")"
    min_len="$(jq -r '.min_length // 0' <<<"$prompt_json")"
    max_len="$(jq -r '.max_length // 100000' <<<"$prompt_json")"

    response="$("$BIN" --sanity-gate "$GGUF" "$sys" "$usr" 2>/dev/null || echo "")"
    rlen="${#response}"

    ok=1
    fail_reason=""
    if [[ -n "$expect_substr" ]] && ! grep -qiF -- "$expect_substr" <<<"$response"; then
        ok=0; fail_reason="missing expect_substring='$expect_substr'"
    fi
    if (( rlen < min_len )); then
        ok=0; fail_reason="${fail_reason:+$fail_reason; }len $rlen < min $min_len"
    fi
    if (( rlen > max_len )); then
        ok=0; fail_reason="${fail_reason:+$fail_reason; }len $rlen > max $max_len"
    fi

    if (( ok == 1 )); then
        PASS=$((PASS + 1))
        printf "  [PASS] %-22s len=%4d\n" "$id" "$rlen"
    else
        FAIL=$((FAIL + 1))
        printf "  [FAIL] %-22s len=%4d  reason=%s\n" "$id" "$rlen" "$fail_reason"
        snippet="${response:0:120}"
        if (( rlen > 120 )); then snippet="${snippet}..."; fi
        printf "         got: %s\n" "$snippet"
    fi
done < <(jq -c '.prompts[]' "$FIXTURE")

echo
echo "[sanity-gate] Results: $PASS passed, $FAIL failed (out of $TOTAL)"

if (( PASS >= PASS_BAR )); then
    if (( PASS == TOTAL )); then
        echo "[sanity-gate] OK: $PASS/$TOTAL PASS — Gemma is ready for Phase 2 (perfect score)"
    else
        echo "[sanity-gate] OK: $PASS/$TOTAL PASS (>= bar $PASS_BAR) — Gemma is ready for Phase 2"
    fi
    exit 0
else
    echo "[sanity-gate] FAIL: $PASS/$TOTAL PASS (< bar $PASS_BAR) — investigate before tagging Phase 1"
    exit 1
fi
