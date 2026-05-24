#!/usr/bin/env bash
# scripts/eval-prompt-blocks.sh
#
# Runs the Sprint B prompt-block contract registry (see
# docs/eval/prompt-blocks-2026-05-24.md). Fails fast on any
# regression in:
#   EMOTIONAL CONTEXT / UPCOMING / WHAT WORKS / IDENTITY /
#   STYLE HINT / VOICE TONE / autoresponder / lora_export
#
# Usage:
#   ./scripts/eval-prompt-blocks.sh           # runs against ./build/human_tests
#   HU_TESTS=./build-test/human_tests ./scripts/eval-prompt-blocks.sh

set -euo pipefail

HU_TESTS="${HU_TESTS:-./build/human_tests}"

if [ ! -x "$HU_TESTS" ]; then
    echo "ERROR: $HU_TESTS not found or not executable." >&2
    echo "Build first: cmake --preset dev && cmake --build build -j8" >&2
    exit 2
fi

SUITES=(
    emotional_context
    anticipatory
    causal_attribution
    identity_continuity
    style_adapter
    audio_emotion
    autoresponder
    lora_export
)

echo "Running prompt-block eval registry against $HU_TESTS"
echo ""

# `--suite=NAME` flags don't accumulate (only the last one wins), so
# run each suite as a separate invocation and sum the counts. This
# also localizes any failure to the specific suite.
total_passed=0
total_count=0
any_fail=0
out_tmp="$(mktemp)"
trap 'rm -f "$out_tmp"' EXIT

for s in "${SUITES[@]}"; do
    if ! "$HU_TESTS" --suite="$s" > "$out_tmp" 2>&1; then
        echo "❌ Suite '$s' FAILED to run" >&2
        tail -20 "$out_tmp" >&2
        any_fail=1
        continue
    fi
    result_line="$(grep -E '^--- Results' "$out_tmp" | tail -1 || true)"
    if [ -z "$result_line" ]; then
        echo "❌ Suite '$s' produced no '--- Results' line" >&2
        any_fail=1
        continue
    fi
    passed=$(echo "$result_line" | sed -E 's/.*Results: ([0-9]+)\/([0-9]+).*/\1/')
    count=$(echo "$result_line" | sed -E 's/.*Results: ([0-9]+)\/([0-9]+).*/\2/')
    printf "  %-22s %s\n" "$s" "$result_line"
    total_passed=$((total_passed + passed))
    total_count=$((total_count + count))
    if [ "$passed" != "$count" ]; then
        echo "    ↳ FAILURES in suite '$s':" >&2
        grep -E "^  (FAIL|ERROR)" "$out_tmp" >&2 || true
        any_fail=1
    fi
done

echo ""
if [ $any_fail -ne 0 ]; then
    echo "❌ Eval registry: $((total_count - total_passed)) test(s) FAILED across ${#SUITES[@]} suites." >&2
    exit 1
fi
echo "✅ Prompt-block eval registry: $total_passed/$total_count contracts pinned, 0 failures."
echo "   See docs/eval/prompt-blocks-2026-05-24.md for the contract → test map."
