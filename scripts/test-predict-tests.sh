#!/usr/bin/env bash
# Smoke test for predict-tests.sh — pins its contract so the selector doesn't
# silently rot. Run manually or from a pre-push hook:
#   bash scripts/test-predict-tests.sh
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

PT="scripts/predict-tests.sh"
fails=0
check() { # check <desc> <expected-substring-or-EMPTY> <actual>
    local desc="$1" want="$2" got="$3"
    if [ "$want" = "EMPTY" ]; then
        if [ -z "$(printf '%s' "$got" | tr -d '[:space:]')" ]; then
            echo "  PASS  $desc"
        else
            echo "  FAIL  $desc — expected no --suite output, got: $got"
            fails=$((fails + 1))
        fi
    elif printf '%s' "$got" | grep -qF -- "$want"; then
        echo "  PASS  $desc"
    else
        echo "  FAIL  $desc — expected to contain '$want', got: $got"
        fails=$((fails + 1))
    fi
}

# 1. A changed test file selects exactly its own suite.
check "test-file → own suite" "--suite=follow_up" \
    "$($PT tests/test_follow_up.c 2>/dev/null)"

# 2. A central/hot header recommends the full suite (no stdout filter).
check "central header (json.h) → full suite (empty stdout)" "EMPTY" \
    "$($PT include/human/core/json.h 2>/dev/null)"

# 3. A loose root source / build system change → full suite (empty stdout).
check "CMakeLists.txt → full suite (empty stdout)" "EMPTY" \
    "$($PT CMakeLists.txt 2>/dev/null)"

# 4. A leaf header yields a tight, NON-empty filter (precise reachability) —
#    and crucially a SMALLER set than the absolute cap.
leaf_out="$($PT include/human/channels/discord.h 2>/dev/null || true)"
leaf_n="$(printf '%s\n' "$leaf_out" | sed '/^$/d' | wc -l | tr -d ' ')"
if [ -n "$(printf '%s' "$leaf_out" | tr -d '[:space:]')" ] && [ "$leaf_n" -le 25 ]; then
    echo "  PASS  leaf header (discord.h) → tight filter ($leaf_n suites)"
else
    echo "  FAIL  leaf header → expected 1..25 suites, got $leaf_n: $leaf_out"
    fails=$((fails + 1))
fi

echo ""
if [ "$fails" -eq 0 ]; then
    echo "predict-tests smoke: ALL PASS"
else
    echo "predict-tests smoke: $fails FAILED"
    exit 1
fi
