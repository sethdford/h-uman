#!/bin/bash
# check-molora-binary-budget.sh — Sprint 7 US-7.8 AC-7.8.5 enforcement.
#
# Builds the `human` binary with HU_ENABLE_MOLORA=OFF and =ON in two
# scratch dirs, computes the byte delta, and fails when it exceeds the
# Init #02 phase-1 budget (8192 bytes).
#
# Designed to run from a clean repo checkout. The build dirs live under
# build-molora-{off,on} so they don't collide with the default `build/`.
#
# Exit codes:
#   0 — delta within budget (or equal/negative)
#   1 — delta exceeds 8 KB budget
#   2 — build failure
#   3 — binary missing post-build

set -euo pipefail

BUDGET=${MOLORA_BUDGET_BYTES:-8192}
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

OFF_DIR="build-molora-off"
ON_DIR="build-molora-on"

stat_size() {
    # macOS uses -f, linux uses -c
    if stat -f %z "$1" >/dev/null 2>&1; then
        stat -f %z "$1"
    else
        stat -c %s "$1"
    fi
}

echo "[molora-budget] Building OFF in $OFF_DIR ..." >&2
cmake -S . -B "$OFF_DIR" -DCMAKE_BUILD_TYPE=MinSizeRel -DHU_ENABLE_MOLORA=OFF \
    -DHU_ENABLE_ML=OFF -DHU_ENABLE_SQLITE=ON >/dev/null || exit 2
cmake --build "$OFF_DIR" --target human >/dev/null || exit 2

echo "[molora-budget] Building ON in $ON_DIR ..." >&2
cmake -S . -B "$ON_DIR" -DCMAKE_BUILD_TYPE=MinSizeRel -DHU_ENABLE_MOLORA=ON \
    -DHU_ENABLE_ML=OFF -DHU_ENABLE_SQLITE=ON >/dev/null || exit 2
cmake --build "$ON_DIR" --target human >/dev/null || exit 2

OFF_BIN="$OFF_DIR/human"
ON_BIN="$ON_DIR/human"
[[ -f "$OFF_BIN" && -f "$ON_BIN" ]] || exit 3

OFF_SIZE=$(stat_size "$OFF_BIN")
ON_SIZE=$(stat_size "$ON_BIN")
DELTA=$((ON_SIZE - OFF_SIZE))

printf "[molora-budget] OFF=%d  ON=%d  DELTA=%d (budget=%d)\n" \
    "$OFF_SIZE" "$ON_SIZE" "$DELTA" "$BUDGET" >&2

if [[ "$DELTA" -gt "$BUDGET" ]]; then
    echo "[molora-budget] FAIL: delta ${DELTA}B exceeds budget ${BUDGET}B (AC-7.8.5)" >&2
    exit 1
fi

echo "[molora-budget] PASS: delta ${DELTA}B within ${BUDGET}B budget" >&2
exit 0
