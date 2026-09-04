#!/usr/bin/env bash
# run-smoke-test.sh — Smoke-test for scripts/check_markdown_relative_links.py
#
# Each subdirectory is a fixture root. The checker is pointed at it with
# MARKDOWN_LINK_ROOTS (a repo-relative directory; the checker also scans the
# top-level *.md files, which doc-fleet already keeps green). The fixture
# directory is excluded from MARKDOWN_LINK_SCAN_ALL=1 so the deliberately
# broken case cannot fail the real docs gate.
#
# Cases (see the 2026-09-03 incident: commit 30568c04d put a regex with a
# `](` in a table cell inside backticks and CI run 33732740550 reported it
# as a missing file):
#   code-only-links      — every [x](y) shape is inside an inline code span
#                          or a fenced block (```, ~~~, indented, closing
#                          run longer than opening). Must exit 0.
#   broken-prose-link    — a reference definition with a missing target,
#                          placed right after a fenced block so ^-anchored
#                          matching must survive stripping. Must exit 1 and
#                          name the target.
#   valid-prose-and-code — a real link in prose plus the same text inside
#                          code. Must exit 0.
# Plus: strip_code() must preserve the line count of every fixture, so any
# future line-number reporting stays correct.
#
# Exit codes:
#   0  — every case produced the expected result
#   1  — one or more cases failed

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/check_markdown_relative_links.py"
FIXTURE_REL="tests/fixtures/check-markdown-links"

FAIL=0

# expect <case-dir> <expected-exit> [<substring that must appear in output>]
expect() {
    local case_dir="$1" want="$2" needle="${3:-}" got=0 out
    out="$(MARKDOWN_LINK_ROOTS="$FIXTURE_REL/$case_dir" python3 "$SCRIPT" 2>&1)" || got=$?
    if [[ "$got" -ne "$want" ]]; then
        echo "FAIL  $case_dir → exit $got (expected $want)" >&2
        echo "$out" | sed 's/^/      /' >&2
        FAIL=1
        return
    fi
    if [[ -n "$needle" && "$out" != *"$needle"* ]]; then
        echo "FAIL  $case_dir → exit $got but output lacks '$needle'" >&2
        echo "$out" | sed 's/^/      /' >&2
        FAIL=1
        return
    fi
    echo "PASS  $case_dir → exit $got (expected $want)"
}

expect code-only-links      0
expect broken-prose-link    1 "./does-not-exist.md (missing)"
expect valid-prose-and-code 0

# strip_code() must not change the number of lines.
if python3 - "$SCRIPT" "$SCRIPT_DIR" <<'PY'
import importlib.util, pathlib, sys
spec = importlib.util.spec_from_file_location("chk", sys.argv[1])
chk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(chk)
bad = 0
for md in sorted(pathlib.Path(sys.argv[2]).rglob("*.md")):
    text = md.read_text(encoding="utf-8")
    stripped = chk.strip_code(text)
    if text.count("\n") != stripped.count("\n"):
        print(f"      {md.name}: {text.count(chr(10))} lines -> {stripped.count(chr(10))} after strip_code")
        bad += 1
sys.exit(1 if bad else 0)
PY
then
    echo "PASS  strip_code preserves line count"
else
    echo "FAIL  strip_code changed the line count" >&2
    FAIL=1
fi

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi

echo "OK  check_markdown_relative_links smoke test passed"
exit 0
