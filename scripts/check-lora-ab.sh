#!/usr/bin/env bash
# Track D D2.2 — lora-ab fidelity-delta gate
#
# Runs `human ml lora-ab` against the paired fixtures
# `tests/fixtures/lora_ab_before.json` (formal, low-fidelity) and
# `tests/fixtures/lora_ab_after.json` (casual, high-fidelity) and
# asserts the reported delta meets `LORA_AB_FLOOR_DELTA` (default 0.10).
#
# Why this gate exists:
#   The A/B comparator (`hu_communication_style_compare_response_sets`)
#   is the actual evaluation harness for the LoRA-vs-baseline question
#   ("did the adapter actually personalize the frontier model?"). Like
#   the lora-baseline gate, this is informational on its own — anyone
#   could ignore the delta. The gate makes it load-bearing: a regression
#   in the comparator (always-zero, NaN, mean-instead-of-delta) or in
#   the synthetic-fingerprint defaults will fail CI before merge.
#   The fixture pair is intentionally extreme (formal vs casual) so the
#   delta is robust to small scoring nudges (e.g. abbreviation list
#   tweaks) without becoming a false-positive gate.
#
# Threshold rationale:
#   - 0.10 is well below the current measured delta (~+0.24) but well
#     above what a broken comparator would emit (0.00 or NaN both fail).
#   - When the synthetic fingerprint or the abbreviation list changes
#     intentionally, update the fixtures and the threshold together.
#
# Usage:
#   bash scripts/check-lora-ab.sh                       # verify
#   LORA_AB_FLOOR_DELTA=0.05 bash scripts/...           # custom floor
#   LORA_AB_BIN=./build/human bash scripts/...          # custom binary

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

BIN="${LORA_AB_BIN:-./build/human}"
FIXTURE_PERSONA="tests/fixtures/lora_baseline_persona.json"
FIXTURE_NAME="lora_baseline_fixture"
FIXTURE_BEFORE="tests/fixtures/lora_ab_before.json"
FIXTURE_AFTER="tests/fixtures/lora_ab_after.json"
FLOOR="${LORA_AB_FLOOR_DELTA:-0.10}"

if [ ! -x "$BIN" ]; then
  echo "[lora-ab-gate] building $BIN ..."
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug >/dev/null
  cmake --build build -j --target human >/dev/null
fi

for f in "$FIXTURE_PERSONA" "$FIXTURE_BEFORE" "$FIXTURE_AFTER"; do
  if [ ! -f "$f" ]; then
    echo "[lora-ab-gate] FAIL: fixture missing at $f" >&2
    exit 1
  fi
done

# Stage the persona into a private dir so the test never touches the
# user's real ~/.human/personas, and so the synthetic-fingerprint
# path triggers reliably. The before/after fixtures are passed as
# absolute paths (the CLI accepts arbitrary file paths for those).
TMPDIR="$(mktemp -d -t human-lora-ab-XXXXXX)"
trap 'rm -rf "$TMPDIR"' EXIT

cp "$FIXTURE_PERSONA" "$TMPDIR/${FIXTURE_NAME}.json"

OUTPUT="$(HU_PERSONA_DIR="$TMPDIR" HOME="$TMPDIR" \
  "$BIN" ml lora-ab \
  --persona "$FIXTURE_NAME" \
  --before "$REPO_ROOT/$FIXTURE_BEFORE" \
  --after "$REPO_ROOT/$FIXTURE_AFTER" 2>&1)"

# Extract the delta from the
# `[lora-ab]   delta:  +0.239 (after - before)` line. We tolerate
# the optional leading sign and use awk for float parsing because
# bash arithmetic is integer-only.
DELTA_LINE="$(printf '%s\n' "$OUTPUT" | grep -E '^\[lora-ab\]\s+delta:' || true)"
if [ -z "$DELTA_LINE" ]; then
  echo "[lora-ab-gate] FAIL: could not find delta line in output:" >&2
  printf '%s\n' "$OUTPUT" >&2
  exit 1
fi

# Extract the raw numeric ("+0.239" or "-0.239") — the second field
# after "delta:" is the value. Strip a leading + so awk parses it.
DELTA="$(printf '%s\n' "$DELTA_LINE" | awk '{print $3}' | sed 's/^+//')"
if ! [[ "$DELTA" =~ ^-?[0-9]+(\.[0-9]+)?$ ]]; then
  echo "[lora-ab-gate] FAIL: delta '$DELTA' is not a number" >&2
  printf '%s\n' "$OUTPUT" >&2
  exit 1
fi

if awk -v d="$DELTA" -v f="$FLOOR" 'BEGIN { exit !(d+0 >= f+0) }'; then
  echo "[lora-ab-gate] PASS: fixture delta=$DELTA >= floor=$FLOOR"
  exit 0
fi

echo "[lora-ab-gate] FAIL: fixture delta=$DELTA < floor=$FLOOR" >&2
printf '%s\n' "$OUTPUT" >&2
exit 1
