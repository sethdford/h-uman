#!/usr/bin/env bash
# run-smoke-test.sh — Smoke-test for scripts/check-realpath-wrapper.sh
#
# Each subdirectory is a miniature repo root holding the source trees the
# gate scans. The checker is pointed at one with `--root`. Fixture sources
# live under tests/fixtures/check-realpath-wrapper/<case>/, so the real
# gate's `src apps sdk tools` scan never sees them.
#
# Cases:
#   bad-paren     — `realpath(path, NULL)`. The classic spelling; the
#                   pre-2026-09-21 pattern caught this one. Must exit 1.
#   bad-space     — `realpath (path, NULL)`. Legal C that the old
#                   `realpath\(` pattern missed entirely. Must exit 1.
#   bad-widened   — a raw call in tools/, outside the old src/-only scan.
#                   Must exit 1.
#   good-wrapper  — `hu_platform_realpath(...)` only. Pins the prefix
#                   guard: that identifier CONTAINS `realpath(`, so a
#                   pattern without the guard fails here on correct code.
#                   Must exit 0.
#   good-exempt   — a raw call in src/app/platform.c, the wrapper's own
#                   implementation and the single exemption. Must exit 0.
#
# Exit codes:
#   0  — every case produced the expected exit code
#   1  — one or more cases failed
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/check-realpath-wrapper.sh"

FAIL=0

# expect <case-dir> <expected-exit>
expect() {
    local case_dir="$1" want="$2" got=0
    bash "$SCRIPT" --root "$SCRIPT_DIR/$case_dir" >/dev/null 2>&1 || got=$?
    if [ "$got" -eq "$want" ]; then
        echo "PASS  $case_dir → exit $got (expected $want)"
    else
        echo "FAIL  $case_dir → exit $got (expected $want)" >&2
        bash "$SCRIPT" --root "$SCRIPT_DIR/$case_dir" 2>&1 | sed 's/^/      /' >&2 || true
        FAIL=1
    fi
}

expect bad-paren    1
expect bad-space    1
expect bad-widened  1
expect good-wrapper 0
expect good-exempt  0

if [ $FAIL -gt 0 ]; then
    exit 1
fi

echo "OK  check-realpath-wrapper smoke test passed"
exit 0
