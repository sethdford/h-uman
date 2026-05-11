#!/usr/bin/env bash
# Combined verification script: build, test, doc fleet, skill registry, token lint.
# Run before claiming any work done, and as part of the weekly drift audit.
#
# Environment:
#   VERIFY_SECURITY_SCAN=1  — run scripts/security-sensitive-api-scan.sh after C tests
#   VERIFY_SECURITY_SCAN=strict — same scan, but the script exits 1 if hits exist (local triage)
# Tip: capture full output with `bash scripts/verify-all.sh 2>&1 | tee verify-all.log`
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

PASS=0
FAIL=0
SKIP=0

run_check() {
  local name="$1"
  shift
  echo ""
  echo "=== $name ==="
  if "$@"; then
    echo "--- PASS: $name"
    PASS=$((PASS + 1))
  else
    echo "--- FAIL: $name"
    FAIL=$((FAIL + 1))
  fi
}

skip_check() {
  local name="$1"
  local reason="$2"
  echo ""
  echo "=== $name ==="
  echo "--- SKIP: $reason"
  SKIP=$((SKIP + 1))
}

echo "=============================="
echo " human verify-all"
echo "=============================="

# 1. C build (Track F1.2: record success so we never run tests on a failed compile)
C_BUILD_OK=0
if [ -d "build" ]; then
  echo ""
  echo "=== C Build ==="
  if cmake --build build -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"; then
    echo "--- PASS: C Build"
    PASS=$((PASS + 1))
    C_BUILD_OK=1
  else
    echo "--- FAIL: C Build"
    FAIL=$((FAIL + 1))
    C_BUILD_OK=0
  fi
else
  skip_check "C Build" "build/ directory not found (run cmake -B build first)"
  C_BUILD_OK=0
fi

# 2. C tests — skip when the build failed or was skipped (avoids misleading stale results)
if [ -f "build/human_tests" ]; then
  if [ "$C_BUILD_OK" -ne 1 ]; then
    skip_check "C Tests" "skipped because C build failed or was not run (stale binary not executed)"
  else
    run_check "C Tests" ./build/human_tests
  fi
else
  skip_check "C Tests" "build/human_tests not found"
fi

# 2b. Memory query explicit-variant guard (Track B2; python3 required)
if [ -f "scripts/check-memory-query-variant.sh" ]; then
  run_check "Memory query variant scan" bash scripts/check-memory-query-variant.sh
fi

# 2d. Track D D2.2 — lora-baseline fidelity gate (cheap, deterministic).
# Runs the offline persona-fidelity scorer on a fixture persona and
# fails when the mean drifts below the floor. Catches regressions in
# the scorer (always-zero, NaN), the synthetic fingerprint defaults,
# and the abbreviation list. Skipped when the human binary isn't built.
if [ -f "scripts/check-lora-baseline.sh" ] && [ -x "build/human" ]; then
  run_check "lora-baseline gate" bash scripts/check-lora-baseline.sh
fi

# 2e. Track D D2.2 — lora-ab fidelity-delta gate (the actual A/B
# evaluation harness). Runs the comparator on the paired fixtures
# (formal pre-LoRA / casual post-LoRA) and fails when the mean
# delta drops below LORA_AB_FLOOR_DELTA (0.10 default). Pinned
# alongside the baseline gate so a regression in either the scorer
# or the comparator is caught before merge.
if [ -f "scripts/check-lora-ab.sh" ] && [ -x "build/human" ]; then
  run_check "lora-ab gate" bash scripts/check-lora-ab.sh
fi

# 2c. Optional security surface scan (Track E; informational unless VERIFY_SECURITY_SCAN=strict)
if [ "${VERIFY_SECURITY_SCAN:-0}" = "1" ] || [ "${VERIFY_SECURITY_SCAN:-}" = "strict" ]; then
  if [ -f "scripts/security-sensitive-api-scan.sh" ]; then
    if [ "${VERIFY_SECURITY_SCAN}" = "strict" ]; then
      run_check "Security sensitive API scan" env VERIFY_SECURITY_SCAN=strict bash scripts/security-sensitive-api-scan.sh
    else
      run_check "Security sensitive API scan" bash scripts/security-sensitive-api-scan.sh
    fi
  fi
fi

# 3. UI typecheck + lint + test
if [ -f "ui/package.json" ]; then
  if command -v npm &>/dev/null && [ -d "ui/node_modules" ]; then
    run_check "UI Check" npm run check --prefix ui
  else
    skip_check "UI Check" "npm not available or ui/node_modules missing"
  fi
else
  skip_check "UI Check" "ui/package.json not found"
fi

# 4. Doc fleet (standards index, drift, terminology, docs frontmatter, docs relative links)
if [ -f "scripts/doc-fleet.sh" ]; then
  run_check "Doc Fleet" bash scripts/doc-fleet.sh
fi

# 5. Skill registry (in-tree index + skill.json parity)
if [ -f "scripts/validate-skill-registry.sh" ]; then
  run_check "Skill Registry" bash scripts/validate-skill-registry.sh
fi

# 6. Token lint (raw colors)
if [ -f "scripts/lint-raw-colors.sh" ]; then
  run_check "Token Lint (colors)" bash scripts/lint-raw-colors.sh --all
fi

# 7. UI token lint
if [ -f "ui/package.json" ] && command -v npm &>/dev/null && [ -d "ui/node_modules" ]; then
  run_check "Token Lint (UI)" npm run lint:tokens --prefix ui 2>/dev/null || true
fi

# 8. Doc stats (display for manual review)
if [ -f "scripts/doc-stats.sh" ]; then
  run_check "Doc Stats" bash scripts/doc-stats.sh
fi

# 9. Native apps (optional — macOS + Xcode + JDK; set VERIFY_NATIVE=1)
if [ "${VERIFY_NATIVE:-0}" = 1 ] && [ -f "scripts/run-native-fleet-local.sh" ]; then
  if [ "$(uname -s 2>/dev/null)" = Darwin ]; then
    run_check "Native fleet local (full)" bash scripts/run-native-fleet-local.sh full
  else
    run_check "Native fleet local (quick, no XCUITest)" bash scripts/run-native-fleet-local.sh quick
  fi
fi

# 10. Red-team / eval fleet (offline by default; set VERIFY_REDTEAM=1 — does not enable live API unless REDTEAM_FLEET_LIVE=1)
if [ "${VERIFY_REDTEAM:-0}" = 1 ] && [ -f "scripts/redteam-eval-fleet.sh" ]; then
  run_check "Red-team eval fleet" bash scripts/redteam-eval-fleet.sh
fi

# 11. Install red team (docs consistency, CLI flag parity, platform defaults)
if [ -f "scripts/redteam-install.sh" ]; then
  run_check "Install red team" bash scripts/redteam-install.sh
fi

# Summary
echo ""
echo "=============================="
echo " Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo "=============================="

if [ "$FAIL" -gt 0 ]; then
  echo ""
  echo "VERIFICATION FAILED. Fix failures before claiming work is done."
  exit 1
else
  echo ""
  echo "All checks passed."
  exit 0
fi
