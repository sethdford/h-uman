#!/usr/bin/env bash
# PCTT regression guard: prevent hardcoded thinking fillers from being
# reintroduced under src/. The strings below are the original three from
# the deprecated classify_think_type DECISION bucket. data/eval_*.json
# preserves them as fixtures and is intentionally excluded.
#
# Usage:
#   ./tools/check-no-hardcoded-fillers.sh
#
# Exit codes:
#   0 — clean (no hardcoded fillers found in src/)
#   1 — at least one hardcoded filler found
#
# Environment:
#   HU_SKIP_FILLER_GUARD=1 — bypass the check (for Wave 2 in-flight builds
#                             before PCTT Task 4 deletes the strings).
#                             Remove this bypass once Task 4 merges.

set -euo pipefail

if [ "${HU_SKIP_FILLER_GUARD:-0}" = "1" ]; then
    echo "check-no-hardcoded-fillers: SKIPPED (HU_SKIP_FILLER_GUARD=1)"
    exit 0
fi

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

PATTERNS=(
  "ooh that's a tough one"
  "let me think about that for a sec"
  "hm good question"
)

found=0
for pat in "${PATTERNS[@]}"; do
  if hits=$(grep -rn --include='*.c' --include='*.h' "$pat" src/ 2>/dev/null); then
    if [ -n "$hits" ]; then
      echo "ERROR: hardcoded filler regression: '$pat'" >&2
      echo "$hits" >&2
      found=1
    fi
  fi
done

if [ "$found" -ne 0 ]; then
  echo "" >&2
  echo "These hardcoded fillers were deleted in PCTT Task 4. If you need them" >&2
  echo "for an eval fixture, put them in data/eval_*.json which is excluded." >&2
  exit 1
fi

echo "check-no-hardcoded-fillers: OK"
