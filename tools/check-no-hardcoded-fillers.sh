#!/bin/sh
# PCTT regression guard: prevent hardcoded thinking fillers from being
# reintroduced under src/. The strings below are the original three from
# the deprecated classify_think_type DECISION bucket. data/eval_*.json
# preserves them as fixtures and is intentionally excluded.
#
# POSIX sh (not bash) so the script runs in slim Docker base images that
# don't carry bash on PATH.
#
# Usage:
#   ./tools/check-no-hardcoded-fillers.sh
#
# Exit codes:
#   0 — clean (no hardcoded fillers found in src/)
#   1 — at least one hardcoded filler found
#
# Environment:
#   HU_SKIP_FILLER_GUARD=1 — bypass the check (intended for transitional
#                             builds; remove this once Task 4 has merged
#                             everywhere).

set -eu

if [ "${HU_SKIP_FILLER_GUARD:-0}" = "1" ]; then
    echo "check-no-hardcoded-fillers: SKIPPED (HU_SKIP_FILLER_GUARD=1)"
    exit 0
fi

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

found=0
check_one() {
    pat=$1
    hits=$(grep -rn --include='*.c' --include='*.h' "$pat" src/ 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "ERROR: hardcoded filler regression: '$pat'" >&2
        echo "$hits" >&2
        found=1
    fi
}

check_one "ooh that's a tough one"
check_one "let me think about that for a sec"
check_one "hm good question"

if [ "$found" -ne 0 ]; then
    echo "" >&2
    echo "These hardcoded fillers were deleted in PCTT Task 4. If you need them" >&2
    echo "for an eval fixture, put them in data/eval_*.json which is excluded." >&2
    exit 1
fi

echo "check-no-hardcoded-fillers: OK"
