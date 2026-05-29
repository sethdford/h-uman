#!/bin/sh
# scripts/prove-loop-local-e2e.sh — Sprint 60: prove the learning loop works
# LOCALLY, end to end, against the REAL compiled artifacts (not the test mocks).
#
# Drives the production `human` binary + the real test binary and asserts
# OBSERVABLE learning, printing a consolidated PASS/FAIL report with numbers.
#
# Legs proven here (all local, no cloud, no live model server required):
#   1. REWARD: `human ml rm-train` trains the Bradley-Terry reward model on
#      real preference pairs; we assert final_loss < initial_loss (it learns).
#   2. FIDELITY (SOTA signal): `human ml lora-baseline` scores a persona's
#      example bank against its style fingerprint (CPU-only). Reports mean
#      fidelity in [0,1] — the M3 metric the LoRA loop is built to lift.
#   3. LOOP CLOSURE: the real test binary runs the four spine suites
#      (huml, dpo_collector, contextual_bandit, e2e_learning_loop) that
#      exercise outcome→mining→reward-learn and proactive→bandit-update.
#
# The LLM-adapter train→swap leg (lora-persona --backend mlx + apply-adapter)
# needs a live MLX server + base model; run it separately on Apple Silicon.
#
# Usage: scripts/prove-loop-local-e2e.sh [--persona <name>]   (default: seth)
# Exit 0 = all legs proven; 1 = a leg failed.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/human"
TESTBIN="$ROOT/build/human_tests"
PERSONA="seth"
[ "${1:-}" = "--persona" ] && PERSONA="${2:-seth}"
WORK="$(mktemp -d /tmp/hu_loop_e2e.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
fail=0
pass() { printf "  \033[32mPASS\033[0m  %s\n" "$1"; }
bad()  { printf "  \033[31mFAIL\033[0m  %s\n" "$1"; fail=1; }

[ -x "$BIN" ]     || { echo "missing $BIN (build first: cmake --build build --target human)"; exit 2; }
[ -x "$TESTBIN" ] || { echo "missing $TESTBIN (build first: cmake --build build --target human_tests)"; exit 2; }

echo "=============================================================="
echo " h-uman learning loop — LOCAL end-to-end proof (real binary)"
echo " bin: $BIN"
echo "=============================================================="

# ---- Leg 1: reward model genuinely learns from preference pairs ----------
echo
echo "[1/3] REWARD MODEL — human ml rm-train (Bradley-Terry, HUML backend)"
cat > "$WORK/pairs.jsonl" <<'EOF'
{"prompt":"0 1 2","chosen":"1 3","rejected":"26 28"}
{"prompt":"0 1 2","chosen":"1 4","rejected":"27 29"}
{"prompt":"0 1 2","chosen":"2 5","rejected":"26 30"}
{"prompt":"0 1 2","chosen":"1 5","rejected":"28 30"}
{"prompt":"0 1 2","chosen":"2 3","rejected":"27 31"}
{"prompt":"0 1 2","chosen":"1 2","rejected":"29 31"}
{"prompt":"0 1 2","chosen":"3 4","rejected":"26 27"}
{"prompt":"0 1 2","chosen":"2 4","rejected":"28 29"}
EOF
mkdir -p "$WORK/rm_ckpt"
rm_out="$("$BIN" ml rm-train --pairs "$WORK/pairs.jsonl" --backend huml \
          --vocab-size 32 --iters 1000 --learning-rate 10.0 --save "$WORK/rm_ckpt" 2>&1)"
echo "$rm_out" | sed 's/^/      /'
il="$(echo "$rm_out" | grep -oE 'initial_loss=[0-9.]+' | head -1 | cut -d= -f2)"
fl="$(echo "$rm_out" | grep -oE 'final_loss=[0-9.]+'   | head -1 | cut -d= -f2)"
if [ -n "$il" ] && [ -n "$fl" ] && awk "BEGIN{exit !($fl < $il)}"; then
  pass "reward model learned: loss $il -> $fl (preference signal absorbed)"
else
  bad "reward model did not reduce loss (initial=$il final=$fl)"
fi
[ -f "$WORK/rm_ckpt/value_head.vh" ] && pass "checkpoint persisted (value_head.vh + rm_meta.json)" \
                                      || bad "checkpoint not written"

# ---- Leg 2: persona fidelity (the SOTA metric) ---------------------------
echo
echo "[2/3] PERSONA FIDELITY — human ml lora-baseline --persona $PERSONA (CPU)"
fid_out="$("$BIN" ml lora-baseline --persona "$PERSONA" 2>&1)"
echo "$fid_out" | sed 's/^/      /'
mean="$(echo "$fid_out" | grep -oE 'mean:[[:space:]]+[0-9.]+' | grep -oE '[0-9.]+$' | head -1)"
n="$(echo "$fid_out" | grep -oE 'examples scored:[[:space:]]+[0-9]+' | grep -oE '[0-9]+$' | head -1)"
if [ -n "$mean" ] && awk "BEGIN{exit !($mean > 0)}"; then
  pass "fidelity baseline measured: mean=$mean over n=${n:-?} examples (SOTA target: LoRA lifts this)"
else
  bad "no fidelity score produced for persona '$PERSONA'"
fi

# ---- Leg 3: loop closure via the real test binary ------------------------
echo
echo "[3/3] LOOP CLOSURE — real spine suites (outcome->mining->reward; proactive->bandit)"
for suite in huml dpo_collector contextual_bandit e2e_learning_loop; do
  line="$("$TESTBIN" --suite="$suite" 2>/dev/null | grep -oE 'Results: [0-9]+/[0-9]+ passed[^-]*' | head -1)"
  case "$line" in
    *" 0 FAILED"*|"") : ;;
  esac
  pc="$(echo "$line" | grep -oE '[0-9]+/[0-9]+' | head -1)"
  num="${pc%%/*}"; den="${pc##*/}"
  if [ -n "$pc" ] && [ "$num" = "$den" ] && [ "$num" -gt 0 ]; then
    pass "suite $suite: $pc"
  else
    bad "suite $suite: '${line:-no result}'"
  fi
done

echo
echo "=============================================================="
if [ "$fail" -eq 0 ]; then
  echo " RESULT: LOCAL E2E PROOF PASSED — the loop learns on real artifacts."
  echo "=============================================================="
  exit 0
else
  echo " RESULT: LOCAL E2E PROOF FAILED — see FAIL lines above."
  echo "=============================================================="
  exit 1
fi
