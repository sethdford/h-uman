#!/usr/bin/env bash
# e2e_all_capabilities.sh — prove every humanness/behavior capability FIRES on a
# real agent turn against the live local model, with its gate flipped ON.
#
# This is the "wired + running e2e" gate: for each capability it runs one
# contact-attributed turn (so memory_session_id is bound — required by GraphRAG,
# ToM, etc.) with HU_DEBUG logging, then greps the capability's own runtime log
# signal as EVIDENCE it actually executed (not just that the code exists).
#
# It does NOT judge quality — that's the blind-A/B's job. It answers exactly:
# "does each capability fire end-to-end on a live turn?"  PASS = its ACTIVE/fired
# log line appears; FAIL = it does not.
#
# Usage: HUMAN_BIN=./build-e2e/human CONTACT="+447914633409" bash scripts/e2e_all_capabilities.sh
set -uo pipefail

HUMAN_BIN="${HUMAN_BIN:-./build-e2e/human}"
CONTACT="${CONTACT:-+447914633409}"   # a contact WITH community summaries
MSG="${MSG:-hey, you around this weekend? been meaning to catch up}"
TURN_TIMEOUT="${TURN_TIMEOUT:-240}"
OUT_DIR="${OUT_DIR:-/tmp/e2e-caps}"
mkdir -p "$OUT_DIR"

if [ ! -x "$HUMAN_BIN" ]; then echo "ERROR: $HUMAN_BIN not executable"; exit 2; fi

# Run one turn with a given env, capture stderr (where the [gate]/[subsystem]
# logs go). macOS has no `timeout`; use a bg PID + watchdog.
run_turn() { # $1=label  $2..=ENV=VAL pairs
    local label="$1"; shift
    local errf="$OUT_DIR/${label}.err" outf="$OUT_DIR/${label}.out"
    ( env HU_DEBUG=1 HU_LOG_LEVEL=info "$@" \
        "$HUMAN_BIN" agent --contact "$CONTACT" -m "$MSG" >"$outf" 2>"$errf" &
      local p=$!; ( sleep "$TURN_TIMEOUT"; kill $p 2>/dev/null ) & local w=$!
      wait $p 2>/dev/null; kill $w 2>/dev/null )
    echo "$errf"
}

pass=0; fail=0
check() { # $1=capability  $2=errfile  $3=grep-ERE signal
    if grep -qiE "$3" "$2" 2>/dev/null; then
        echo "  PASS  $1  →  $(grep -iE "$3" "$2" | head -1 | sed 's/^[[:space:]]*//' | cut -c1-90)"
        pass=$((pass+1))
    else
        echo "  FAIL  $1  →  signal '$3' not found (gate may not have fired)"
        fail=$((fail+1))
    fi
}

echo "=== e2e all-capabilities activation ($HUMAN_BIN, contact=$CONTACT) ==="
echo "Each capability: gate ON → real turn → grep its runtime ACTIVE/fired signal."
echo

# 1. GraphRAG grounding (default ON) — shadow mode logs the loaded bytes.
e=$(run_turn graphrag HU_GRAPH_GROUNDING=shadow)
check "GraphRAG grounding" "$e" 'graph_grounding.*shadow:.*graph_context bytes|graph_grounding=ACTIVE'

# 2. Salience arbitration (LIVE).
e=$(run_turn salience HU_SALIENCE=live)
check "Salience arbitration" "$e" 'salience\(live\)|salience=ACTIVE|salience.*kept [0-9]'

# 3. Intent directive (default ON).
e=$(run_turn intent HU_INTENT_DIRECTIVE=on)
check "Intent directive" "$e" 'intent=ACTIVE|intent.*directive'

# 4. Theory of Mind (default OFF → flip ON). Gate-logger uses "theory_of_mind".
e=$(run_turn tom HU_TOM_DIRECTIVE=on)
check "Theory of Mind" "$e" 'theory_of_mind=ACTIVE|\[tom\]|tom.*directive'

# 5. Self-model readback (needs HU_ENABLE_SELF_MODEL build + >=3 turns).
#    Real log: "[self_model] shadow self-observation: ..."
e=$(run_turn selfmodel HU_SELF_MODEL=shadow)
check "Self-model readback" "$e" '\[self_model\]|self_model.*shadow self-observation'

# 6. Self-uncertainty (shadow). Real log: "[self_uncertainty] shadow confidence=..."
e=$(run_turn uncertainty HU_SELF_UNCERTAINTY=shadow)
check "Self-uncertainty" "$e" 'self_uncertainty=ACTIVE|\[self_uncertainty\] shadow confidence'

# 7. Bandit humanization (default ON) — note: fires on daemon social-tick, may
#    NOT log on a plain CLI turn. Reported honestly either way.
e=$(run_turn bandit HU_BANDIT_HUMANIZATION=on)
check "Bandit humanization" "$e" 'bandit_humanization=ACTIVE|bandit.*param|humanization.*bandit'

# 8. Intrinsic goals (default OFF → shadow). World-model path; may need goal engine.
e=$(run_turn intrinsic HU_INTRINSIC_GOALS=shadow)
check "Intrinsic goals" "$e" 'intrinsic.*goal|autonomy.*goal|self.initiated'

echo
echo "=== e2e capability activation: $pass fired, $fail did not ==="
echo "Evidence logs: $OUT_DIR/*.err"
[ "$fail" -eq 0 ] && echo "ALL capabilities fire e2e." || echo "Some capabilities did not log a fire signal — see per-capability notes above."
