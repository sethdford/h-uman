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

# Composite floor — the multi-axis L1 score from
# `hu_persona_fidelity_score_l1` reported alongside the single-axis mean
# (added 2026-05-16 in src/ml/cli.c).
#
# 2026-05-16 baseline measured composite=0.614. Floor of 0.55 leaves
# ~10% headroom for legitimate scoring nudges (weight retunes, axis
# refinements) without flagging them as regressions, while still
# catching a broken composite (zero / NaN / always-pass).
#
# See docs/eval/baseline-2026-05-16.md for the rationale and retake protocol.
COMPOSITE_FLOOR="${LORA_BASELINE_COMPOSITE_FLOOR:-0.55}"
COMPOSITE_LINE="$(printf '%s\n' "$OUTPUT" | grep -E '^\[lora-baseline\]\s+composite:' || true)"

# Composite line is optional for backwards compat — older binaries
# (without the 2026-05-16 src/ml/cli.c change) emit only the
# single-axis mean. Warn and skip rather than fail so a CI matrix
# pinned to an older release of `${LORA_BASELINE_BIN}` keeps working.
if [ -z "$COMPOSITE_LINE" ]; then
  echo "[lora-baseline-gate] WARN: no composite line in output — older binary?"
  COMPOSITE=""
else
  # Line shape:
  #   [lora-baseline]   composite:       0.614 (style=... traits=... line=... stderr=... n=...)
  # Field 3 (1-indexed after awk's space split) is the bare composite value.
  COMPOSITE="$(printf '%s\n' "$COMPOSITE_LINE" | awk '{print $3}')"
  if ! [[ "$COMPOSITE" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
    echo "[lora-baseline-gate] FAIL: composite '$COMPOSITE' is not a number" >&2
    printf '%s\n' "$OUTPUT" >&2
    exit 1
  fi
fi

# Use awk for float comparison — bash arithmetic only handles ints.
MEAN_PASS=0
if awk -v m="$MEAN" -v f="$FLOOR" 'BEGIN { exit !(m+0 >= f+0) }'; then
  MEAN_PASS=1
fi

COMPOSITE_PASS=1
if [ -n "$COMPOSITE" ]; then
  COMPOSITE_PASS=0
  if awk -v m="$COMPOSITE" -v f="$COMPOSITE_FLOOR" 'BEGIN { exit !(m+0 >= f+0) }'; then
    COMPOSITE_PASS=1
  fi
fi

if [ "$MEAN_PASS" = 1 ] && [ "$COMPOSITE_PASS" = 1 ]; then
  if [ -n "$COMPOSITE" ]; then
    echo "[lora-baseline-gate] PASS: mean=$MEAN >= $FLOOR, composite=$COMPOSITE >= $COMPOSITE_FLOOR"
  else
    echo "[lora-baseline-gate] PASS: fixture mean=$MEAN >= floor=$FLOOR (composite skipped)"
  fi
  exit 0
fi

if [ "$MEAN_PASS" != 1 ]; then
  echo "[lora-baseline-gate] FAIL: fixture mean=$MEAN < floor=$FLOOR" >&2
fi
if [ "$COMPOSITE_PASS" != 1 ]; then
  echo "[lora-baseline-gate] FAIL: composite=$COMPOSITE < floor=$COMPOSITE_FLOOR" >&2
fi
printf '%s\n' "$OUTPUT" >&2
exit 1
