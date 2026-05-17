#!/usr/bin/env bash
# Sprint 11 / US-11.7 AC-11.7.6 — integration test for `--cascade` flag.
#
# Asserts:
#   - The script emits a per-stage JSON breakdown
#   - Exit code 2 on the Sprint 8 regression fixture (REJECT)
#   - Exit code 0 on the iter60_padfix fixture (PROMOTE)
#   - Exit code 1 on the iter60_dirty fixture (DEFER)
#   - The `[cascade] stage<N>:` parseable lines are present
#
# The test runs the cascade end-to-end through the shell wrapper, which is
# the call signature CI / US-11.8 cron will use.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/check-lora-ab.sh"
FIX_DIR="$REPO_ROOT/tests/fixtures/cascade"

fail() { echo "[test_check_lora_ab_staged] FAIL: $1" >&2; exit 1; }
pass() { echo "[test_check_lora_ab_staged] OK: $1"; }

[ -x "$SCRIPT" ] || chmod +x "$SCRIPT" || true
[ -f "$FIX_DIR/sprint8_iter200.json" ] || fail "missing sprint8 fixture"
[ -f "$FIX_DIR/iter60_padfix.json" ] || fail "missing padfix fixture"
[ -f "$FIX_DIR/iter60_dirty.json" ] || fail "missing dirty fixture"

# ── REJECT case (Sprint 8 regression guard) ──────────────────────────────
set +e
OUT_REJECT="$(bash "$SCRIPT" --cascade \
  --cascade-fixture "tests/fixtures/cascade/sprint8_iter200.json" 2>&1)"
RC=$?
set -e
[ "$RC" -eq 2 ] || fail "sprint8 fixture: expected exit 2, got $RC"
echo "$OUT_REJECT" | grep -q '"final_verdict": "REJECT"' \
  || fail "sprint8 fixture: missing final_verdict=REJECT in JSON"
echo "$OUT_REJECT" | grep -q '\[cascade\] stage1: REJECT' \
  || fail "sprint8 fixture: missing parseable stage1 REJECT line"
echo "$OUT_REJECT" | grep -q '\[cascade\] stage2: skipped_due_to_short_circuit' \
  || fail "sprint8 fixture: missing short-circuit marker on stage2"
pass "sprint8 fixture rejected at Stage 1 (AC-11.7.3)"

# ── PROMOTE case ─────────────────────────────────────────────────────────
set +e
OUT_PROMOTE="$(bash "$SCRIPT" --cascade \
  --cascade-fixture "tests/fixtures/cascade/iter60_padfix.json" 2>&1)"
RC=$?
set -e
[ "$RC" -eq 0 ] || fail "padfix fixture: expected exit 0, got $RC"
echo "$OUT_PROMOTE" | grep -q '"final_verdict": "PROMOTE"' \
  || fail "padfix fixture: missing final_verdict=PROMOTE"
echo "$OUT_PROMOTE" | grep -q '\[cascade\] stage1: PASS' \
  || fail "padfix fixture: missing stage1 PASS"
echo "$OUT_PROMOTE" | grep -q '\[cascade\] stage3: SKIP' \
  || fail "padfix fixture: Stage 3 should be SKIP (D3 dormancy)"
pass "padfix fixture promotes cleanly"

# ── DEFER case ───────────────────────────────────────────────────────────
set +e
OUT_DEFER="$(bash "$SCRIPT" --cascade \
  --cascade-fixture "tests/fixtures/cascade/iter60_dirty.json" 2>&1)"
RC=$?
set -e
[ "$RC" -eq 1 ] || fail "dirty fixture: expected exit 1, got $RC"
echo "$OUT_DEFER" | grep -q '"final_verdict": "DEFER"' \
  || fail "dirty fixture: missing final_verdict=DEFER"
pass "dirty fixture defers"

# ── Shape check: per-stage JSON breakdown is present (AC-11.7.6) ─────────
# Each PROMOTE/DEFER run must include all four stage entries.
for s in 1 2 3 4; do
  echo "$OUT_PROMOTE" | grep -q "\"stage\": $s" \
    || fail "padfix output missing 'stage: $s' in JSON"
done
pass "per-stage breakdown JSON contains all 4 stages (AC-11.7.6)"

echo "[test_check_lora_ab_staged] ALL OK"
exit 0
