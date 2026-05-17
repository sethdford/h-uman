#!/usr/bin/env bash
# Sprint 2c F5 — negative test for check-test-time-symbol-availability.sh.
#
# Proves the lint catches the four shapes of the test-time-invisibility
# pattern that bit Sprint 1 Story C (and any future regression):
#
#   1. extern in header + def inside `#ifdef HU_IS_TEST ... #else <DEF>` (the original Sprint 1 bug)
#   2. extern in header + def inside `#ifndef HU_IS_TEST <DEF>`
#   3. extern in header + def inside `#if !defined(HU_IS_TEST) <DEF>`
#   4. NEGATIVE CONTROL: extern in header + def above the guard (correct shape) — must NOT flag
#
# Plus three false-positive guards:
#
#   5. Symbol declared extern in a private header under `tests/` (not include/) — not the lint's scope
#   6. Symbol guarded by `#ifdef _WIN32` (platform, not test) — must NOT flag
#   7. extern function declaration (parens) in header — must NOT flag (lint targets data, not code)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
LINT="$REPO_ROOT/scripts/check-test-time-symbol-availability.sh"

[[ -x "$LINT" ]] || { echo "missing $LINT" >&2; exit 99; }

TMP="$(mktemp -d "${TMPDIR:-/tmp}/sp2c-F5.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0
ac_pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
ac_fail() { echo "  FAIL: $1" >&2;  FAIL=$((FAIL + 1)); }

mk_fixture() {
  local name="$1"
  local fixture_dir="$TMP/$name"
  mkdir -p "$fixture_dir/include" "$fixture_dir/src"
  echo "$fixture_dir"
}

run_lint() {
  local fixture_dir="$1"
  local logfile="$2"
  set +e
  HU_TESTSYM_SCAN_ROOT="$fixture_dir" bash "$LINT" >"$logfile" 2>&1
  local rc=$?
  set -e
  echo "$rc"
}

# ============================================================
# Scenario 1: ORIGINAL BUG (#ifdef HU_IS_TEST ... #else <DEF>)
# ============================================================
echo "== Scenario 1: #ifdef HU_IS_TEST ... #else <DEF> (original Sprint 1 bug) =="
F1="$(mk_fixture s1)"
cat >"$F1/include/foo.h" <<'C'
#ifndef FOO_H
#define FOO_H
extern const char foo_payload[];
#endif
C
cat >"$F1/src/foo.c" <<'C'
#include "foo.h"

#ifdef HU_IS_TEST
/* test stub does nothing */
#else
const char foo_payload[] = "production-only";
#endif
C
rc=$(run_lint "$F1" "$TMP/s1.log")
if [[ "$rc" -ne 0 ]]; then
  ac_pass "lint flagged #ifdef-#else hidden definition (rc=$rc)"
else
  ac_fail "lint missed the original Sprint 1 failure mode"
fi
if grep -q "#ifdef HU_IS_TEST 'else'" "$TMP/s1.log"; then
  ac_pass "diagnostic mentions the actual guard shape"
else
  ac_fail "diagnostic shape unexpected"
  cat "$TMP/s1.log" >&2 || true
fi

# ============================================================
# Scenario 2: #ifndef HU_IS_TEST <DEF>
# ============================================================
echo "== Scenario 2: #ifndef HU_IS_TEST <DEF> =="
F2="$(mk_fixture s2)"
cat >"$F2/include/bar.h" <<'C'
#ifndef BAR_H
#define BAR_H
extern const char bar_payload[];
#endif
C
cat >"$F2/src/bar.c" <<'C'
#include "bar.h"

#ifndef HU_IS_TEST
const char bar_payload[] = "production-only";
#endif
C
rc=$(run_lint "$F2" "$TMP/s2.log")
if [[ "$rc" -ne 0 ]]; then
  ac_pass "lint flagged #ifndef HU_IS_TEST hidden definition"
else
  ac_fail "lint missed the #ifndef shape"
fi

# ============================================================
# Scenario 3: #if !defined(HU_IS_TEST) <DEF>
# ============================================================
echo "== Scenario 3: #if !defined(HU_IS_TEST) <DEF> =="
F3="$(mk_fixture s3)"
cat >"$F3/include/baz.h" <<'C'
#ifndef BAZ_H
#define BAZ_H
extern const char baz_payload[];
#endif
C
cat >"$F3/src/baz.c" <<'C'
#include "baz.h"

#if !defined(HU_IS_TEST)
const char baz_payload[] = "production-only";
#endif
C
rc=$(run_lint "$F3" "$TMP/s3.log")
if [[ "$rc" -ne 0 ]]; then
  ac_pass "lint flagged #if !defined(HU_IS_TEST) hidden definition"
else
  ac_fail "lint missed the #if !defined shape"
fi

# ============================================================
# Scenario 4: CORRECT SHAPE — definition above guard (must NOT flag)
# ============================================================
echo "== Scenario 4: correct shape (def above guard) — lint MUST NOT flag =="
F4="$(mk_fixture s4)"
cat >"$F4/include/qux.h" <<'C'
#ifndef QUX_H
#define QUX_H
extern const char qux_payload[];
#endif
C
cat >"$F4/src/qux.c" <<'C'
#include "qux.h"

const char qux_payload[] = "always-compiled";

#ifdef HU_IS_TEST
/* test stub does nothing */
#else
/* real implementation */
#endif
C
rc=$(run_lint "$F4" "$TMP/s4.log")
if [[ "$rc" -eq 0 ]]; then
  ac_pass "correct shape (def above guard) accepted"
else
  ac_fail "lint false-positived a correct fixture (rc=$rc)"
  cat "$TMP/s4.log" >&2 || true
fi

# ============================================================
# Scenario 5: extern in tests/ header (NOT include/) — out of lint scope
# ============================================================
echo "== Scenario 5: extern in tests/ (not include/) — lint MUST NOT flag =="
F5="$(mk_fixture s5)"
mkdir -p "$F5/tests"
cat >"$F5/tests/test_helpers.h" <<'C'
extern const char test_only_payload[];
C
cat >"$F5/src/test_only.c" <<'C'
#include "../tests/test_helpers.h"

#ifdef HU_IS_TEST
const char test_only_payload[] = "test-only";
#else
/* not compiled in production */
#endif
C
rc=$(run_lint "$F5" "$TMP/s5.log")
if [[ "$rc" -eq 0 ]]; then
  ac_pass "private extern in tests/ header ignored (out of lint scope)"
else
  ac_fail "lint false-positived an in-scope-test extern"
  cat "$TMP/s5.log" >&2 || true
fi

# ============================================================
# Scenario 6: platform guard (#ifdef _WIN32) — NOT a test guard
# ============================================================
echo "== Scenario 6: platform guard (#ifdef _WIN32) — lint MUST NOT flag =="
F6="$(mk_fixture s6)"
cat >"$F6/include/plat.h" <<'C'
#ifndef PLAT_H
#define PLAT_H
extern const char plat_payload[];
#endif
C
cat >"$F6/src/plat.c" <<'C'
#include "plat.h"

#ifdef _WIN32
const char plat_payload[] = "windows-only";
#else
const char plat_payload[] = "posix";
#endif
C
rc=$(run_lint "$F6" "$TMP/s6.log")
if [[ "$rc" -eq 0 ]]; then
  ac_pass "platform-only guard ignored (not test-related)"
else
  ac_fail "lint false-positived a platform guard"
  cat "$TMP/s6.log" >&2 || true
fi

# ============================================================
# Scenario 7: extern function declaration — out of lint scope (data only)
# ============================================================
echo "== Scenario 7: extern function decl — lint MUST NOT flag =="
F7="$(mk_fixture s7)"
cat >"$F7/include/fn.h" <<'C'
#ifndef FN_H
#define FN_H
extern int compute_thing(int x);
#endif
C
cat >"$F7/src/fn.c" <<'C'
#include "fn.h"

#ifdef HU_IS_TEST
int compute_thing(int x) { (void)x; return 0; }
#else
int compute_thing(int x) { return x * 2; }
#endif
C
rc=$(run_lint "$F7" "$TMP/s7.log")
if [[ "$rc" -eq 0 ]]; then
  ac_pass "extern function decl ignored (lint targets data)"
else
  ac_fail "lint false-positived a function decl"
  cat "$TMP/s7.log" >&2 || true
fi

# ============================================================
# Scenario 8: live tree — must remain clean
# ============================================================
echo "== Scenario 8: live tree must remain clean =="
rc=$(cd "$REPO_ROOT" && run_lint "$REPO_ROOT" "$TMP/live.log")
if [[ "$rc" -eq 0 ]]; then
  ac_pass "live tree has no test-time-invisible extern symbols"
else
  ac_fail "live tree has new violations — sprint-2c regressed against itself"
  cat "$TMP/live.log" >&2 || true
fi

echo
echo "=========================================================="
echo "Sprint 2c F5 negative test: PASS=$PASS FAIL=$FAIL"
echo "=========================================================="
[[ "$FAIL" -eq 0 ]]
