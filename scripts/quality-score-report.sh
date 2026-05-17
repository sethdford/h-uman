#!/usr/bin/env bash
# quality-score-report.sh — Automated quality score reporter
# Collects measurable quality signals and produces a summary.
# Designed to run in CI as a step summary or locally for audits.
set -euo pipefail

SCORE=0
MAX=0
REPORT=""

pass() { SCORE=$((SCORE + $1)); MAX=$((MAX + $1)); REPORT="${REPORT}\n✓ $2 (+$1)"; }
fail() { MAX=$((MAX + $1)); REPORT="${REPORT}\n✗ $2 (0/$1)"; }
warn() { SCORE=$((SCORE + ($1 / 2))); MAX=$((MAX + $1)); REPORT="${REPORT}\n~ $2 (+$(($1 / 2))/$1)"; }

echo "=== Human Quality Score Report ==="
echo ""

# --- Component Quality ---
echo "## Component Quality"
COMP_FAIL=0
if [ -d "ui/src/components" ]; then
  for f in ui/src/components/hu-*.ts; do
    [ -f "$f" ] || continue
    # check-component.sh cd's into ui/ before opening $1, so the path
    # must be relative to ui/, not to the repo root. Without stripping
    # the `ui/` prefix every check silently failed with "File not found"
    # — which was the root cause of 110/110 components being scored as
    # failing (gate stuck at 64%).
    if ! bash ui/scripts/check-component.sh "${f#ui/}" >/dev/null 2>&1; then
      COMP_FAIL=$((COMP_FAIL + 1))
    fi
  done
  COMP_TOTAL=$(ls ui/src/components/hu-*.ts 2>/dev/null | wc -l | tr -d ' ')
  if [ "$COMP_FAIL" -eq 0 ]; then
    pass 10 "All $COMP_TOTAL components pass quality checks"
  elif [ "$COMP_FAIL" -lt 5 ]; then
    warn 10 "$COMP_FAIL/$COMP_TOTAL components have quality issues"
  else
    fail 10 "$COMP_FAIL/$COMP_TOTAL components have quality issues"
  fi
else
  fail 10 "No components directory found"
fi

# --- Design Token Compliance ---
echo "## Token Compliance"
if [ -f "ui/scripts/lint-raw-values.sh" ]; then
  RAW_COUNT=$(bash ui/scripts/lint-raw-values.sh 2>/dev/null | grep -c "WARN\|FAIL" || true)
  if [ "$RAW_COUNT" -eq 0 ]; then
    pass 10 "Zero raw hex/px/duration values in UI code"
  elif [ "$RAW_COUNT" -lt 10 ]; then
    warn 10 "$RAW_COUNT raw values detected"
  else
    fail 10 "$RAW_COUNT raw values detected"
  fi
else
  warn 10 "Token lint script not found"
fi

# --- Test Coverage ---
echo "## Test Coverage"
if [ -f "build/human_tests" ]; then
  # `tail -1` was unreliable — the test binary emits stderr log lines
  # AFTER the Results: line (e.g., "[agent_turn] consistency drift...")
  # which then caused the regex to miss and report "Could not parse
  # test results" (-10 from the quality score). Pin to the Results
  # line directly via grep.
  #
  # `|| true` is REQUIRED: under `set -euo pipefail` (line 4), a grep
  # with zero matches exits 1 which propagates and aborts the entire
  # script — bypassing the graceful "Could not parse" fallback below
  # (lines 82-83). Both Bugbot and CodeRabbit caught this regression
  # against the previous `tail -1` which always exited 0.
  TEST_LINE=$(./build/human_tests 2>&1 | grep -E '^--- Results:' | tail -1 || true)
  PASSED=$(echo "$TEST_LINE" | grep -oE '[0-9]+/[0-9]+ passed' | head -1 || echo "")
  if [ -n "$PASSED" ]; then
    COUNT=$(echo "$PASSED" | cut -d/ -f1)
    TOTAL=$(echo "$PASSED" | cut -d/ -f2 | cut -d' ' -f1)
    if [ "$COUNT" -eq "$TOTAL" ] && [ "$TOTAL" -gt 4500 ]; then
      pass 10 "$PASSED — zero failures"
    elif [ "$COUNT" -eq "$TOTAL" ]; then
      warn 10 "$PASSED — consider adding more tests"
    else
      fail 10 "$PASSED — failures detected"
    fi
  else
    fail 10 "Could not parse test results"
  fi
else
  warn 10 "Test binary not found (build/human_tests)"
fi

# --- Untested Sources ---
echo "## Source Coverage"
if [ -f "scripts/check-untested.sh" ]; then
  UNTESTED=$(bash scripts/check-untested.sh 2>&1 | grep -c "MISSING" || true)
  if [ "$UNTESTED" -eq 0 ]; then
    pass 5 "All source files have test references"
  else
    fail 5 "$UNTESTED source files lack test coverage"
  fi
else
  warn 5 "check-untested.sh not found"
fi

# --- Unused Tokens ---
echo "## Token Health"
if [ -f "scripts/check-unused-tokens.sh" ]; then
  UNUSED=$(bash scripts/check-unused-tokens.sh 2>&1 | grep -c "unused" || true)
  if [ "$UNUSED" -eq 0 ]; then
    pass 5 "Zero unused tokens"
  else
    warn 5 "$UNUSED unused tokens detected"
  fi
else
  warn 5 "Unused token check not found"
fi

# --- Cross-Platform Token Parity ---
echo "## Cross-Platform Parity"
PLATFORMS=0
[ -f "apps/ios/Sources/HumaniOS/ContentView.swift" ] && grep -q "HUTokens" "apps/ios/Sources/HumaniOS/ContentView.swift" 2>/dev/null && PLATFORMS=$((PLATFORMS + 1))
[ -f "apps/macos/Sources/HumanApp/SettingsView.swift" ] && grep -q "HUTokens" "apps/macos/Sources/HumanApp/SettingsView.swift" 2>/dev/null && PLATFORMS=$((PLATFORMS + 1))
[ -f "apps/android/app/src/main/java/ai/human/app/ui/theme/Theme.kt" ] && PLATFORMS=$((PLATFORMS + 1))
if [ "$PLATFORMS" -ge 3 ]; then
  pass 10 "All 3 native platforms use design tokens"
elif [ "$PLATFORMS" -ge 2 ]; then
  warn 10 "$PLATFORMS/3 native platforms use design tokens"
else
  fail 10 "Only $PLATFORMS/3 native platforms use design tokens"
fi

# --- Accessibility ---
echo "## Accessibility"
if [ -f "ui/e2e/accessibility.spec.ts" ]; then
  pass 5 "axe-core E2E accessibility tests exist"
else
  fail 5 "No accessibility E2E tests"
fi

# --- Motion Quality ---
echo "## Motion Quality"
SPRING_USAGE=$(grep -rl "spring\|hu-spring\|springExpressive\|animation-timeline" ui/src/ 2>/dev/null | wc -l | tr -d ' ')
if [ "$SPRING_USAGE" -gt 20 ]; then
  pass 5 "Spring animations used in $SPRING_USAGE files"
elif [ "$SPRING_USAGE" -gt 10 ]; then
  warn 5 "Spring animations in $SPRING_USAGE files (target: 20+)"
else
  fail 5 "Spring animations only in $SPRING_USAGE files"
fi

# --- Reduced Motion ---
echo "## Reduced Motion"
REDUCED=$(grep -rl "prefers-reduced-motion" ui/src/ 2>/dev/null | wc -l | tr -d ' ')
if [ "$REDUCED" -gt 5 ]; then
  pass 5 "prefers-reduced-motion in $REDUCED files"
else
  warn 5 "prefers-reduced-motion only in $REDUCED files (target: 5+)"
fi

echo ""
echo "=== Quality Score: $SCORE / $MAX ==="
PCT=$((SCORE * 100 / MAX))
echo "Percentage: ${PCT}%"
echo ""

if [ "$PCT" -ge 90 ]; then
  echo "Rating: AWARD-WINNING TIER"
elif [ "$PCT" -ge 75 ]; then
  echo "Rating: BEST-IN-CLASS"
elif [ "$PCT" -ge 60 ]; then
  echo "Rating: ABOVE AVERAGE"
else
  echo "Rating: NEEDS IMPROVEMENT"
fi

echo ""
echo "--- Detail ---"
printf "$REPORT\n"

if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  {
    echo "## Quality Score: $SCORE / $MAX (${PCT}%)"
    echo ""
    printf "$REPORT\n" | sed 's/^/- /'
  } >> "$GITHUB_STEP_SUMMARY"
fi

# Fail CI if quality score drops below threshold
QUALITY_GATE=${QUALITY_GATE_PCT:-70}
if [ "$PCT" -lt "$QUALITY_GATE" ]; then
  echo "::error::Quality score ${PCT}% is below the ${QUALITY_GATE}% gate. Fix quality issues before merging."
  exit 1
fi
