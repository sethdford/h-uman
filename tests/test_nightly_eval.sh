#!/usr/bin/env bash
# Smoke test: nightly_eval.sh stage 3 (blind-A/B gate refresh) is wired and preconditions work.
#
# This is a developer-runnable shell test (not a C test).
# Run manually: bash tests/test_nightly_eval.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/nightly_eval.sh"

# Precondition: nightly_eval.sh exists and is executable
if [ ! -x "$SCRIPT" ]; then
    echo "FAIL: nightly_eval.sh not executable: $SCRIPT" >&2
    exit 1
fi

# Test 1: script syntax is valid
if ! bash -n "$SCRIPT"; then
    echo "FAIL: nightly_eval.sh has syntax errors" >&2
    exit 1
fi
echo "Test 1 passed: nightly_eval.sh syntax valid"

# Test 2: --dry-run includes stage-3 plan lines
dry_run_output=$("$SCRIPT" --dry-run 2>&1 || true)
if ! echo "$dry_run_output" | grep -q "blind_ab:"; then
    echo "FAIL: --dry-run does not show blind_ab script path" >&2
    echo "$dry_run_output" | head -20
    exit 1
fi
echo "Test 2 passed: --dry-run shows blind_ab path"

if ! echo "$dry_run_output" | grep -q "\[3/3\].*blind-ab"; then
    echo "FAIL: --dry-run does not include stage 3 (blind-ab) in plan" >&2
    echo "$dry_run_output" | tail -5
    exit 1
fi
echo "Test 3 passed: --dry-run includes [3/3] blind-ab in plan"

# Test 4: --dry-run shows precondition states (all should be present on this machine)
if ! echo "$dry_run_output" | grep -q "ground truth:"; then
    echo "FAIL: --dry-run does not show ground truth check" >&2
    exit 1
fi
echo "Test 4 passed: --dry-run checks ground truth precondition"

if ! echo "$dry_run_output" | grep -q "judge creds:"; then
    echo "FAIL: --dry-run does not show judge creds check" >&2
    exit 1
fi
echo "Test 5 passed: --dry-run checks judge credentials precondition"

if ! echo "$dry_run_output" | grep -q "autopush:"; then
    echo "FAIL: --dry-run does not show autopush setting" >&2
    exit 1
fi
echo "Test 6 passed: --dry-run shows autopush env control"

echo ""
echo "All tests passed: nightly_eval.sh stage 3 (blind-A/B gate refresh) wired correctly"
