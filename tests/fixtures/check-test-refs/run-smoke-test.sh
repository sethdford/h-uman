#!/usr/bin/env bash
# run-smoke-test.sh — Smoke-test for scripts/check-test-references.sh
#
# Validates that the script correctly accepts a good fixture (exits 0) and
# rejects a bad fixture (exits 1). Called explicitly with the fixture files
# so the script's internal tests/test_*.c path filter is bypassed — these
# fixtures are named bad.c / good.c intentionally so the path filter does NOT
# match them when the script runs in default (pre-commit / staged-files) mode.
#
# Exit codes:
#   0  — both checks pass
#   1  — one or more checks failed

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/check-test-references.sh"
GOOD="$SCRIPT_DIR/good.c"
BAD="$SCRIPT_DIR/bad.c"
DISAMBIG="$SCRIPT_DIR/test_imessage_disambig.c"

FAIL=0

# ── Check 1: good.c must exit 0 ───────────────────────────────────────────────
if bash "$SCRIPT" "$GOOD"; then
    echo "PASS  good.c → exit 0 (expected)"
else
    echo "FAIL  good.c → non-zero exit (expected 0)" >&2
    FAIL=1
fi

# ── Check 2: bad.c must exit 1 ────────────────────────────────────────────────
if bash "$SCRIPT" "$BAD" 2>/dev/null; then
    echo "FAIL  bad.c → exit 0 (expected 1)" >&2
    FAIL=1
else
    echo "PASS  bad.c → exit 1 (expected)"
fi

# ── Check 3: multi-candidate disambiguation ───────────────────────────────────
# The basename "imessage" matches both src/channels/imessage.c and
# src/feeds/imessage.c. Before the 2026-05-17 fix, the script used
# `find ... | head -1` and picked whichever filesystem order returned first.
# That meant tests legitimately covering src/channels/imessage.c could fail
# because the script checked for src/feeds/imessage.c symbols instead.
#
# The fixture references ONLY a channels/imessage.c symbol. With the fix,
# the script scores both candidates and picks channels → pass. If the
# disambiguation regresses, this fails with the wrong-module error.
if bash "$SCRIPT" "$DISAMBIG"; then
    echo "PASS  test_imessage_disambig.c → exit 0 (multi-candidate disambiguation works)"
else
    echo "FAIL  test_imessage_disambig.c → non-zero exit (multi-candidate disambiguation regressed)" >&2
    FAIL=1
fi

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi

echo "OK  check-test-references smoke test passed"
exit 0
