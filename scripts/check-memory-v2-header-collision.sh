#!/usr/bin/env bash
# Gate G2 (memory v2): forbid the same TU from including both legacy
# `human/memory.h` and W7 `human/memory/memory.h` — that reintroduces the
# hu_memory_t typedef collision documented in docs/plans/2026-05-10-w7-type-collision-cleanup.md
#
# Usage: bash scripts/check-memory-v2-header-collision.sh
# Exit 0 = no violation; 1 = at least one forbidden pair (not allowlisted).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

# Substrings of paths under src/ allowed to include both during Phase 0
# migration only — keep empty unless a bridge TU is unavoidable.
ALLOWLIST=()

is_allowed() {
    local f="$1"
    local a
    # With `set -u`, an empty array would make "${ALLOWLIST[@]}" error on some bash.
    [ "${#ALLOWLIST[@]}" -eq 0 ] && return 1
    for a in "${ALLOWLIST[@]}"; do
        [[ "$f" == *"$a"* ]] && return 0
    done
    return 1
}

bad=0
while IFS= read -r f; do
    [ -n "$f" ] || continue
    # Sparse checkouts or index drift: skip missing paths (grep would error).
    [ -f "$f" ] || continue
    is_allowed "$f" && continue
    if grep -Eq '#include[[:space:]]+"human/memory.h"' "$f" &&
        grep -Eq '#include[[:space:]]+"human/memory/memory.h"' "$f"; then
        printf '%s\n' "memory-v2-header-collision: $f includes both legacy human/memory.h and W7 human/memory/memory.h" >&2
        bad=1
    fi
done < <(git ls-files src | grep -E '\.(c|h)$' || true)

exit "$bad"
