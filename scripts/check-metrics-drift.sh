#!/usr/bin/env bash
# Verify that key docs have consistent, up-to-date numeric claims.
# Compares counts from repo-metrics.sh against claims in CLAUDE.md,
# AGENTS.md, ARCHITECTURE.md, and README.md.
# Fails if any count is off by more than a tolerance margin.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

EXIT_CODE=0
DRIFT_COUNT=0

# Optional mode:
#   (no args)            build-free doc-metric drift check (runs in doc-fleet + pre-push)
#   --binary <path>      ALSO check the docs' "~NNNNN KB" binary-size claims against the
#                        actual size of <path>. Used by the release-size CI job, which is
#                        the only place a MinSizeRel binary exists. Build-free callers can't
#                        measure binary size, so it stays ungated there — which is exactly
#                        how the docs drifted to a stale ~23209 KB (a debug-build number)
#                        while the real release binary is < 2600 KB.
MODE="${1:-}"
BINARY_PATH="${2:-}"

eval "$(bash scripts/repo-metrics.sh)"

# Lines of C in src/, rounded to the nearest K, to match the docs' "~NNNK lines" phrasing.
SRC_LOC_K=$(( (SRC_LOC + 500) / 1000 ))

check_metric() {
  local file="$1"
  local label="$2"
  local actual="$3"
  local pattern="$4"
  local tolerance="${5:-10}"

  if [ ! -f "$file" ]; then return; fi

  while IFS= read -r line; do
    claimed=$(echo "$line" | grep -oE "$pattern" | grep -oE '[0-9,]+' | head -1 | tr -d ',')
    if [ -z "$claimed" ] || [ "$claimed" -eq 0 ] 2>/dev/null; then continue; fi

    diff=$((actual - claimed))
    if [ "$diff" -lt 0 ]; then diff=$((-diff)); fi

    pct=0
    if [ "$actual" -gt 0 ]; then
      pct=$((diff * 100 / actual))
    fi

    if [ "$pct" -gt "$tolerance" ]; then
      echo "  DRIFT: $file $label: claimed $claimed, actual $actual (${pct}% off)"
      DRIFT_COUNT=$((DRIFT_COUNT + 1))
      EXIT_CODE=1
      return
    fi
  done < <(grep -E "$pattern" "$file" 2>/dev/null || true)
}

KEY_DOCS=("CLAUDE.md" "AGENTS.md" "ARCHITECTURE.md" "README.md")

# --binary mode: ONLY the binary-size claims, against a real built binary.
if [ "$MODE" = "--binary" ]; then
  if [ -z "$BINARY_PATH" ] || [ ! -f "$BINARY_PATH" ]; then
    echo "::error::--binary requires a path to a built binary (got '${BINARY_PATH:-<empty>}')"
    exit 2
  fi
  BIN_BYTES=$(stat -f%z "$BINARY_PATH" 2>/dev/null || stat -c%s "$BINARY_PATH" 2>/dev/null || echo 0)
  BIN_KB=$((BIN_BYTES / 1024))
  echo "Checking binary-size claims against ${BINARY_PATH} (${BIN_KB} KB)..."
  echo ""
  for doc in "${KEY_DOCS[@]}"; do
    if [ ! -f "$doc" ]; then continue; fi
    echo "  Checking $doc..."
    # Pattern '~[0-9]+ KB' is specific enough to skip competitor "~8 MB" cells on
    # the same line; check_metric extracts the number from the matched substring.
    check_metric "$doc" "binary_size_kb" "$BIN_KB" '~[0-9]+ KB' 15
  done
  echo ""
  if [ "$DRIFT_COUNT" -eq 0 ]; then
    echo "  No binary-size drift detected (within 15% of ${BIN_KB} KB)."
  else
    echo "  Found $DRIFT_COUNT binary-size drift(s). Run scripts/update-stats.sh --apply with a"
    echo "  MinSizeRel binary in build/ to regenerate, or fix the '~NNNNN KB' claims by hand."
  fi
  exit $EXIT_CODE
fi

echo "Checking metric consistency across docs..."
echo ""

for doc in "${KEY_DOCS[@]}"; do
  if [ ! -f "$doc" ]; then continue; fi
  echo "  Checking $doc..."
  check_metric "$doc" "test_cases" "$TEST_CASES" '[0-9,]+\+? tests[^/]' 15
  check_metric "$doc" "test_files" "$TEST_FILES" '[0-9,]+ test files' 15
  check_metric "$doc" "channels"   "$CHANNEL_ENUM" '[0-9]+ (messaging )?channels' 15
  check_metric "$doc" "tools"      87              '[0-9]+ tool impl' 15
  check_metric "$doc" "fuzz"       "$FUZZ_HARNESSES" '[0-9]+ libFuzzer' 15
  # Lines of C (src/), in K. Pattern is the specific "NNNK lines of C" phrasing so
  # it can't collide with the "NNNK lines of tests" figure on the same line. The
  # canonical claim in CLAUDE.md + AGENTS.md uses this exact wording. Tolerance 10%
  # rides normal growth between doc refreshes but catches a stale snapshot.
  check_metric "$doc" "lines_of_c" "$SRC_LOC_K" '[0-9]+K lines of C' 10
done

echo ""
if [ "$DRIFT_COUNT" -eq 0 ]; then
  echo "  No metric drift detected."
else
  echo "  Found $DRIFT_COUNT metric drift(s)."
  echo "  Run 'bash scripts/repo-metrics.sh --human' to see current values."
fi

exit $EXIT_CODE
