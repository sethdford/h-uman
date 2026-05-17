#!/usr/bin/env bash
# US-7.6 AC-7.6.5 — assert `scripts/check-lora-ab.sh --judgment` emits
# a visible SKIP line when no NLL backend is registered (sprint 7 D3:
# the seam ships dormant).
#
# The SKIP line MUST be parseable as a non-pass result so US-7.5's
# nightly cron cannot silently treat the inactive gate as a green
# light. We assert on the exact string "[lora-ab] judgment: SKIP".
#
# Run manually: bash tests/test_check_lora_ab_judgment.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

BIN="${LORA_AB_BIN:-./build/human}"
if [ ! -x "$BIN" ]; then
  echo "FAIL: $BIN not built; run cmake --build --preset dev" >&2
  exit 1
fi

# Test 1: with --judgment, the SKIP line appears AND the lexical gate
# still passes AND exit code is 0.
OUTPUT="$(LORA_AB_BIN="$BIN" bash "$REPO_ROOT/scripts/check-lora-ab.sh" --judgment 2>&1)"
EXIT=$?
if [ "$EXIT" -ne 0 ]; then
  echo "FAIL: --judgment gate exited $EXIT (expected 0)" >&2
  printf '%s\n' "$OUTPUT" >&2
  exit 1
fi

if ! printf '%s\n' "$OUTPUT" | grep -qE '^\[lora-ab\] judgment: SKIP'; then
  echo "FAIL: --judgment gate did not emit SKIP line" >&2
  printf '%s\n' "$OUTPUT" >&2
  exit 1
fi

# The existing lexical-surface PASS line must still be there — we did
# not break the old gate.
if ! printf '%s\n' "$OUTPUT" | grep -qE '^\[lora-ab-gate\] PASS:'; then
  echo "FAIL: lexical-surface PASS line missing" >&2
  printf '%s\n' "$OUTPUT" >&2
  exit 1
fi

# The SKIP line MUST be parseable as non-pass. Verify that the literal
# string "PASS" does NOT appear on the judgment line — this is what
# downstream automation depends on.
JUDGE_LINE="$(printf '%s\n' "$OUTPUT" | grep -E '^\[lora-ab\] judgment:')"
if printf '%s\n' "$JUDGE_LINE" | grep -q 'PASS'; then
  echo "FAIL: judgment SKIP line should not contain 'PASS'" >&2
  echo "got: $JUDGE_LINE" >&2
  exit 1
fi

echo "Test 1 passed: --judgment emits SKIP, lexical gate still PASS, exit 0"

# Test 2: without --judgment, the SKIP line must NOT appear (the new
# code path is opt-in).
OUTPUT2="$(LORA_AB_BIN="$BIN" bash "$REPO_ROOT/scripts/check-lora-ab.sh" 2>&1)"
EXIT2=$?
if [ "$EXIT2" -ne 0 ]; then
  echo "FAIL: bare gate exited $EXIT2 (expected 0)" >&2
  printf '%s\n' "$OUTPUT2" >&2
  exit 1
fi
if printf '%s\n' "$OUTPUT2" | grep -qE '^\[lora-ab\] judgment:'; then
  echo "FAIL: judgment line appeared without --judgment flag" >&2
  printf '%s\n' "$OUTPUT2" >&2
  exit 1
fi
echo "Test 2 passed: bare gate unchanged, no judgment line emitted"

echo "test_check_lora_ab_judgment: OK"
