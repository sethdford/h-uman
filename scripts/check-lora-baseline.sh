#!/usr/bin/env bash
# Track D D2.2 — lora-baseline fidelity gate
#
# Runs `human ml lora-baseline` against the fixture persona at
# `tests/fixtures/lora_baseline_persona.json` and asserts the
# reported mean fidelity is at least the floor below.
#
# Why this gate exists:
#   The offline persona-fidelity scorer (`hu_communication_style_fidelity_score`)
#   is informational on its own — anyone can ignore the number it
#   prints. This gate makes it load-bearing: a regression in the
#   scorer (always-zero, NaN, broken axis math) or in the synthetic
#   fingerprint defaults will fail CI before it can ship. The fixture
#   persona's example responses are intentionally tuned to score
#   ~0.85+ against the synthetic target, so a 0.50 floor leaves
#   plenty of headroom for legitimate scoring nudges (e.g. a future
#   abbreviation-list update) without flagging them as regressions.
#
# Threshold rationale:
#   - 0.50 is well below the current measured mean (~0.92) but well
#     above what a broken scorer would emit (0.00 or NaN both fail).
#   - When the synthetic fingerprint or the abbreviation list changes
#     intentionally, update the fixture and the threshold together.
#
# Usage:
#   bash scripts/check-lora-baseline.sh                 # verify
#   LORA_BASELINE_FLOOR=0.40 bash scripts/...           # custom floor
#   LORA_BASELINE_BIN=./build/human bash scripts/...    # custom binary

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

BIN="${LORA_BASELINE_BIN:-./build/human}"
FIXTURE="tests/fixtures/lora_baseline_persona.json"
FIXTURE_NAME="lora_baseline_fixture"
FLOOR="${LORA_BASELINE_FLOOR:-0.50}"

if [ ! -x "$BIN" ]; then
  # Fall back to a fresh build so the gate is runnable from a clean
  # checkout. Builds quietly into ./build to match CI conventions.
  echo "[lora-baseline-gate] building $BIN ..."
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug >/dev/null
  cmake --build build -j --target human >/dev/null
fi

if [ ! -f "$FIXTURE" ]; then
  echo "[lora-baseline-gate] FAIL: fixture missing at $FIXTURE" >&2
  exit 1
fi

# Stage the fixture in a private persona dir so the test doesn't
# touch the user's real ~/.human/personas, and so the synthetic-
# fingerprint path triggers reliably (no personal_model.bin in the
# tmp HOME).
TMPDIR="$(mktemp -d -t human-lora-baseline-XXXXXX)"
trap 'rm -rf "$TMPDIR"' EXIT

cp "$FIXTURE" "$TMPDIR/${FIXTURE_NAME}.json"

OUTPUT="$(HU_PERSONA_DIR="$TMPDIR" HOME="$TMPDIR" \
  "$BIN" ml lora-baseline --persona "$FIXTURE_NAME" 2>&1)"

# Extract the mean from the [lora-baseline]   mean:            0.923
# line. We pin to the exact prefix so a future log-line addition
# can't accidentally match.
MEAN_LINE="$(printf '%s\n' "$OUTPUT" | grep -E '^\[lora-baseline\]\s+mean:' || true)"
if [ -z "$MEAN_LINE" ]; then
  echo "[lora-baseline-gate] FAIL: could not find mean line in output:" >&2
  printf '%s\n' "$OUTPUT" >&2
  exit 1
fi

MEAN="$(printf '%s\n' "$MEAN_LINE" | awk '{print $NF}')"
if ! [[ "$MEAN" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
  echo "[lora-baseline-gate] FAIL: mean '$MEAN' is not a number" >&2
  printf '%s\n' "$OUTPUT" >&2
  exit 1
fi

# Use awk for float comparison — bash arithmetic only handles ints.
if awk -v m="$MEAN" -v f="$FLOOR" 'BEGIN { exit !(m+0 >= f+0) }'; then
  echo "[lora-baseline-gate] PASS: fixture mean=$MEAN >= floor=$FLOOR"
  exit 0
fi

echo "[lora-baseline-gate] FAIL: fixture mean=$MEAN < floor=$FLOOR" >&2
printf '%s\n' "$OUTPUT" >&2
exit 1
