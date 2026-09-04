#!/usr/bin/env bash
# run-smoke-test.sh — Smoke-test for scripts/check-test-source-gate-symmetry.sh
#
# Each subdirectory is a miniature repo root (its own CMakeLists.txt plus
# tests/ and, where needed, src/) reproducing one gate shape. The checker is
# pointed at it with `--root` and run with `--strict` so BASELINE_ALLOWLIST
# cannot mask a fixture. Fixture test files live under
# tests/fixtures/check-gate-symmetry/<case>/tests/, so the pre-commit
# `tests/test_*.c` pathspec never matches them and they are invisible to the
# real CMakeLists.txt.
#
# Cases (see .claude/rules/test-source-gate-symmetry.md, "Platform gates"):
#   bad-unwrapped    — the PRE-fix shape of commit 012ced741: the source is
#                      under if(HU_HAS_IMESSAGE), the test is registered
#                      unconditionally and calls hu_imessage_* unguarded.
#                      Before this checker learned platform macros it reported
#                      this as symmetric. Must exit 1.
#   bad-body-gated   — the source is registered unconditionally but its whole
#                      body is behind `#if HU_HAS_IMESSAGE` with no stub, so it
#                      exports nothing on Linux. Must exit 1.
#   good-wrap        — the POST-fix shape of 012ced741: accepted shape 2
#                      (internal `#if HU_HAS_IMESSAGE` wrap + stub runner).
#                      Must exit 0.
#   good-cmake-gate  — accepted shape 1: test registered inside the same
#                      if(HU_HAS_IMESSAGE) block as the source. Must exit 0.
#
# Exit codes:
#   0  — every case produced the expected exit code
#   1  — one or more cases failed

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/check-test-source-gate-symmetry.sh"

FAIL=0

# expect <case-dir> <expected-exit>
expect() {
    local case_dir="$1" want="$2" got=0
    bash "$SCRIPT" --root "$SCRIPT_DIR/$case_dir" --strict >/dev/null 2>&1 || got=$?
    if [[ "$got" -eq "$want" ]]; then
        echo "PASS  $case_dir → exit $got (expected $want)"
    else
        echo "FAIL  $case_dir → exit $got (expected $want)" >&2
        bash "$SCRIPT" --root "$SCRIPT_DIR/$case_dir" --strict 2>&1 | sed 's/^/      /' >&2 || true
        FAIL=1
    fi
}

expect bad-unwrapped   1
expect bad-body-gated  1
expect good-wrap       0
expect good-cmake-gate 0

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi

echo "OK  check-test-source-gate-symmetry smoke test passed"
exit 0
