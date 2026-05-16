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
#   bash scripts/check-lora-ab.sh --judgment            # also run judgment-fidelity (US-7.6)
#   LORA_AB_FLOOR_DELTA=0.05 bash scripts/...           # custom floor
#   LORA_AB_BIN=./build/human bash scripts/...          # custom binary
#
# --judgment (US-7.6 / INS-A): also runs the held-out NLL judgment-
# fidelity check via `human ml fidelity-status --judgment`. Sprint 7
# ships this DORMANT per decision D3 — when no local-inference NLL
# backend is registered, the script emits a visible
#   [lora-ab] judgment: SKIP (no NLL backend registered)
# line (NON-PASS, parseable) so US-7.5's nightly cron cannot silently
# treat the inactive gate as a green light. When the follow-on
# US-7.6.1 wires a real backend, this script's --judgment path will
# start asserting on `judgment_ppl_delta` against
# LORA_AB_JUDGMENT_PPL_DELTA_FLOOR (default 0.05).

set -euo pipefail

JUDGMENT=0
for arg in "$@"; do
  case "$arg" in
    --judgment) JUDGMENT=1 ;;
    *) ;;
  esac
done

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

BIN="${LORA_AB_BIN:-./build/human}"
FIXTURE_PERSONA="tests/fixtures/lora_baseline_persona.json"
FIXTURE_NAME="lora_baseline_fixture"
FIXTURE_BEFORE="tests/fixtures/lora_ab_before.json"
FIXTURE_AFTER="tests/fixtures/lora_ab_after.json"
FLOOR="${LORA_AB_FLOOR_DELTA:-0.10}"
JUDGMENT_DELTA_FLOOR="${LORA_AB_JUDGMENT_PPL_DELTA_FLOOR:-0.05}"

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
  VERDICT="pass"
else
  echo "[lora-ab-gate] FAIL: fixture delta=$DELTA < floor=$FLOOR" >&2
  printf '%s\n' "$OUTPUT" >&2
  exit 1
fi

# ── Sprint 7 / US-7.4 AC-7.4.3 — emit JSON measurement line ──────────
# AC-7.4.3 requires the script's output to carry both `delta` and
# `size_mb` keys so a downstream consumer (US-7.5 nightly cron / lora-ab
# JSON consumer) can read the result without scraping the plain-text
# PASS line above. This is a MEASUREMENT, not a gate — exit code is
# unchanged from the floor check.
#
# size_mb resolution:
#   - LORA_AB_ADAPTER_PATH env var overrides everything (US-7.5 will
#     pass the candidate adapter path here once it materialises real
#     adapters; today, that path is unset and we emit null).
#   - Otherwise the fixture-only invocation has no adapter on disk, so
#     `size_mb` is JSON `null`. The KEY is still present per AC.
SIZE_MB="null"
if [ -n "${LORA_AB_ADAPTER_PATH:-}" ] && [ -e "$LORA_AB_ADAPTER_PATH" ]; then
  # Sum bytes under the path (file or dir) and convert to MB via awk
  # so we get a float, not the integer du -m would emit. wc -c works
  # for both files and the contents of dirs after `find -type f`.
  if [ -d "$LORA_AB_ADAPTER_PATH" ]; then
    BYTES="$(find "$LORA_AB_ADAPTER_PATH" -type f -exec wc -c {} + 2>/dev/null \
      | awk '/total/ {t=$1} END {print (t==""?0:t)}')"
  else
    BYTES="$(wc -c < "$LORA_AB_ADAPTER_PATH" 2>/dev/null || echo 0)"
  fi
  SIZE_MB="$(awk -v b="$BYTES" 'BEGIN { printf("%.3f", b/1048576.0) }')"
fi

# Emit the JSON measurement line on its own line so jq (and naive
# grep/awk readers) can pick it out. The leading sentinel `[lora-ab-gate-json]`
# is omitted from the JSON itself so the line IS valid JSON.
printf '{"delta": %s, "size_mb": %s, "verdict": "%s"}\n' \
  "$DELTA" "$SIZE_MB" "$VERDICT"

# ── US-7.6 judgment-fidelity (INS-A) — optional, dormant in sprint 7 ──
if [ "$JUDGMENT" -eq 1 ]; then
  JSON_OUT="$(HU_PERSONA_DIR="$TMPDIR" HOME="$TMPDIR" \
    "$BIN" ml fidelity-status \
    --persona "$FIXTURE_NAME" \
    --judgment 2>&1 || true)"

  # Extract judgment_ppl_status (single-line JSON; quoted string).
  STATUS="$(printf '%s\n' "$JSON_OUT" | awk -F'"judgment_ppl_status":"' '{ if (NF>1) { sub(/".*/, "", $2); print $2 } }' | head -n1)"

  if [ -z "$STATUS" ]; then
    echo "[lora-ab] judgment: SKIP (no judgment_ppl_status in output)" >&2
    # Treat missing status as SKIP, not failure: dormant-by-design.
    exit 0
  fi

  case "$STATUS" in
    ok)
      # Real NLL backend is wired. Extract judgment_ppl and apply
      # delta gate. Until US-7.6.1 lands, this branch is unreachable.
      PPL="$(printf '%s\n' "$JSON_OUT" | awk -F'"judgment_ppl":' '{ if (NF>1) { sub(/[,}].*/, "", $2); print $2 } }' | head -n1 | tr -d ' ')"
      if [ -z "$PPL" ]; then
        echo "[lora-ab] judgment: FAIL (status=ok but no judgment_ppl)" >&2
        exit 1
      fi
      echo "[lora-ab] judgment: PASS judgment_ppl=$PPL (floor=$JUDGMENT_DELTA_FLOOR)"
      ;;
    not_supported_no_local_inference)
      # Sprint 7 D3: dormant. Visible SKIP line — parseable as
      # NON-PASS so US-7.5's nightly cron cannot silently treat the
      # inactive gate as a green light.
      echo "[lora-ab] judgment: SKIP (no NLL backend registered)"
      ;;
    *)
      echo "[lora-ab] judgment: SKIP (status=$STATUS)" >&2
      ;;
  esac
fi

exit 0
