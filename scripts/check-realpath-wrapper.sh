#!/usr/bin/env bash
# check-realpath-wrapper.sh
#
# Absolute gate: no raw realpath() call may appear in the C sources of
# src/, apps/, sdk/ or tools/ outside src/app/platform.c. Every path
# resolution goes through hu_platform_realpath (include/human/platform.h)
# so the Windows (_fullpath) and allocator-ownership contract is honored
# everywhere. See docs/standards/engineering/cross-platform.md and
# .claude/rules/ (Task 20: route raw realpath() through
# hu_platform_realpath). Ceiling is 0.
#
# USAGE: bash scripts/check-realpath-wrapper.sh [--root DIR]
#   --root DIR  scan DIR/{src,apps,sdk,tools} instead of the repo root's.
#               Used by tests/fixtures/check-realpath-wrapper/run-smoke-test.sh.
#
# THE PATTERN, and why each piece is load-bearing:
#
#   (^|[^_a-zA-Z0-9])realpath[[:space:]]*\(
#    \______________/          \_________/
#     prefix guard              space tolerance
#
# The prefix guard is NOT optional. Dropping it makes the pattern match
# `hu_platform_realpath(` — the wrapper this gate exists to promote — in
# six files today, turning the gate into a false-positive machine that
# fails on correct code. It excludes digits as well as letters and `_`, so
# a hypothetical `utf8realpath(` is also not a hit, and it allows a match
# at start-of-line via the `^` alternative.
#
# The space tolerance is the 2026-09-21 fix: a space between the identifier
# and the paren, which every C compiler accepts, slipped through the
# previous no-space pattern entirely. Both spellings are pinned by the
# smoke-test fixtures.
#
# KNOWN LIMITATION (pre-existing, deliberately not fixed here): the scan is
# line-oriented and has no notion of comments or string literals, so a
# COMMENT that spells out a bare call is reported as a hit. There are zero
# such comments in the tree today, and a comment demonstrating the call the
# gate forbids is arguably worth flagging anyway. Making the gate
# comment-aware means a C tokenizer, which is not worth it for a ceiling-0
# grep — but it is why the good-wrapper fixture describes the wrapper's
# name in prose instead of quoting it.
set -euo pipefail

ROOT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --root) ROOT="$2"; shift 2 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
if [ -z "$ROOT" ]; then
    ROOT="$(git rev-parse --show-toplevel 2>/dev/null || echo .)"
fi
cd "$ROOT"

# Scan every tree that holds first-party C. apps/ carries no .c today
# (Swift/Kotlin), but is listed so a future C file there is covered the day
# it lands rather than the day someone remembers this gate. Missing
# directories are skipped: grep -r on a nonexistent path is a hard error,
# and a checkout without apps/ must not fail the gate.
scan_dirs=()
for d in src apps sdk tools; do
    [ -d "$d" ] && scan_dirs+=("$d")
done

if [ ${#scan_dirs[@]} -eq 0 ]; then
    echo "raw realpath() calls outside src/app/platform.c: 0 (ceiling 0) — no source dirs to scan"
    exit 0
fi

hits=$(grep -rnE '(^|[^_a-zA-Z0-9])realpath[[:space:]]*\(' "${scan_dirs[@]}" \
  --include='*.c' --include='*.m' --include='*.h' \
  | grep -v '^src/app/platform\.c:' || true)
n=$(printf '%s' "$hits" | grep -c . || true)
echo "raw realpath() calls outside src/app/platform.c: $n (ceiling 0) [scanned: ${scan_dirs[*]}]"
if [ "$n" -gt 0 ]; then
  echo "FAIL: call hu_platform_realpath(alloc, path) instead of raw realpath()." >&2
  echo "      See include/human/platform.h and docs/standards/engineering/cross-platform.md." >&2
  printf '%s\n' "$hits" | sed 's/^/  /' >&2
  exit 1
fi
