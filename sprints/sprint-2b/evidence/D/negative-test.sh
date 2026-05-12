#!/usr/bin/env bash
# Sprint 2b Story D — negative test for scripts/check-memory-query-variant.sh
# (master follow-through Track B2.2).
#
# The variant scanner exits 0 today because the inventory is clean. That
# proves the *current* tree is good but does NOT prove the scanner is
# capable of catching a regression. A scanner that returns "all good"
# regardless of input is worthless as a CI gate.
#
# This driver synthesizes two fixture trees and runs the scanner against
# each:
#
#   bad/  — has memset(&q, 0, sizeof(hu_memory_query_t)) with no
#           subsequent `.variant =` in the next 48 lines. Scanner MUST
#           exit non-zero.
#
#   good/ — same memset followed by `q.variant = HU_MEMORY_QUERY_FACT;`.
#           Scanner MUST exit zero.
#
# We run the scanner from a tmp working directory so its `src/**/*.c`
# glob picks up our fixtures instead of the real tree.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
SCANNER="$REPO_ROOT/scripts/check-memory-query-variant.sh"

[[ -x "$SCANNER" ]] || { echo "missing $SCANNER" >&2; exit 99; }

TMP="$(mktemp -d "${TMPDIR:-/tmp}/sp2b-storyD.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0
ac_pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
ac_fail() { echo "  FAIL: $1" >&2;  FAIL=$((FAIL + 1)); }

# --- bad fixture: missing variant ---
BAD_DIR="$TMP/bad"
mkdir -p "$BAD_DIR/src/memory"
cat >"$BAD_DIR/src/memory/bad_query.c" <<'C'
#include "human/memory.h"

void run_query(void) {
    hu_memory_query_t q;
    memset(&q, 0, sizeof(hu_memory_query_t));
    q.text = "missing-variant";
    q.k = 5;
    facade_read(&q);
}
C

# --- good fixture: variant set explicitly ---
GOOD_DIR="$TMP/good"
mkdir -p "$GOOD_DIR/src/memory"
cat >"$GOOD_DIR/src/memory/good_query.c" <<'C'
#include "human/memory.h"

void run_query(void) {
    hu_memory_query_t q;
    memset(&q, 0, sizeof(hu_memory_query_t));
    q.variant = HU_MEMORY_QUERY_FACT;
    q.text = "explicit-variant";
    q.k = 5;
    facade_read(&q);
}
C

# --- run scanner against bad ---
echo "== negative: scanner MUST flag missing .variant =="
set +e
HU_VARIANT_SCAN_ROOT="$BAD_DIR" bash "$SCANNER" >"$TMP/bad.log" 2>&1
rc=$?
set -e
if [[ "$rc" -ne 0 ]]; then
  ac_pass "scanner exit non-zero on bad fixture (rc=$rc)"
else
  ac_fail "scanner missed the bad fixture (returned 0)"
fi
if grep -q "without \`.variant =\`" "$TMP/bad.log"; then
  ac_pass "scanner emitted the expected diagnostic"
else
  ac_fail "scanner did not emit the expected diagnostic"
  cat "$TMP/bad.log" >&2 || true
fi

# --- run scanner against good ---
echo "== positive: scanner MUST accept explicit .variant =="
set +e
HU_VARIANT_SCAN_ROOT="$GOOD_DIR" bash "$SCANNER" >"$TMP/good.log" 2>&1
rc=$?
set -e
if [[ "$rc" -eq 0 ]]; then
  ac_pass "scanner exit 0 on good fixture"
else
  ac_fail "scanner false-positived the good fixture (rc=$rc)"
  cat "$TMP/good.log" >&2 || true
fi

# --- run scanner against the live tree (sanity check inventory still clean) ---
echo "== inventory: scanner MUST be clean against the live tree =="
set +e
( cd "$REPO_ROOT" && bash "$SCANNER" >"$TMP/live.log" 2>&1 )
rc=$?
set -e
if [[ "$rc" -eq 0 ]]; then
  ac_pass "live tree inventory clean"
else
  ac_fail "live tree has new variant violations (someone reverted the inventory fix)"
  cat "$TMP/live.log" >&2 || true
fi

echo
echo "=========================================================="
echo "Story D (Sprint 2b) Track B negative test: PASS=$PASS FAIL=$FAIL"
echo "=========================================================="
[[ "$FAIL" -eq 0 ]]
