#!/usr/bin/env bash
# Tests for the gated mlx-tune candidate-training stage in nightly-retrain.sh.
#
# Sources nightly-retrain.sh with HU_RETRAIN_STAGE_TEST=1 so ONLY log() and
# run_mlxtune_candidate_stage() get defined -- the window check, mlx-server
# bootout, and real training below never execute (see the guard right after
# the function definition in nightly-retrain.sh). HOME is faked to a tmpdir
# for every case, so the venv paths the stage hardcodes
# ($HOME/.human/venvs/{mlxtune312,eval312}/bin/python) never resolve to the
# REAL machine venvs -- this suite never touches a real python interpreter,
# model, or the real ~/.human tree. That is what "hermetic" means here.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="$HERE/nightly-retrain.sh"
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; echo "  --- output ---"; echo "$3" | sed 's/^/  /'; fail=1; fi; }

bash -n "$SCRIPT" || { echo "FAIL bash -n $SCRIPT"; exit 1; }
echo "PASS bash -n $SCRIPT"

# ── Case 1: HU_RETRAIN_MLXTUNE=0 -- disabled, no other side effects ────────
T1=$(mktemp -d)
out1=$(HOME="$T1" HU_RETRAIN_STAGE_TEST=1 HU_RETRAIN_MLXTUNE=0 bash -c '
    source "'"$SCRIPT"'"
    run_mlxtune_candidate_stage
')
check "disabled: logs exactly the disabled line" \
    "[[ \"\$out1\" == *'mlx-tune candidate stage disabled (HU_RETRAIN_MLXTUNE=0)'* ]]" "$out1"
check "disabled: does not mention starting/training/scoring" \
    "[[ \"\$out1\" != *'starting (HU_RETRAIN_MLXTUNE=1)'* && \"\$out1\" != *'training ->'* && \"\$out1\" != *'scoring candidate'* ]]" "$out1"
check "disabled: no adapters directory created (no training side effect)" \
    "[ ! -d \"$T1/.human/training-data/adapters\" ]" "(n/a)"
rm -rf "$T1"

# ── Case 2: default (HU_RETRAIN_MLXTUNE unset) behaves the same as 0 ───────
T1b=$(mktemp -d)
out1b=$(HOME="$T1b" HU_RETRAIN_STAGE_TEST=1 bash -c '
    source "'"$SCRIPT"'"
    run_mlxtune_candidate_stage
')
check "default (unset) is disabled" \
    "[[ \"\$out1b\" == *'mlx-tune candidate stage disabled (HU_RETRAIN_MLXTUNE=0)'* ]]" "$out1b"
rm -rf "$T1b"

# ── Case 3: HU_RETRAIN_MLXTUNE=1 but serving not stopped -- refuses ────────
T2=$(mktemp -d)
out2=$(HOME="$T2" HU_RETRAIN_STAGE_TEST=1 HU_RETRAIN_MLXTUNE=1 bash -c '
    source "'"$SCRIPT"'"
    run_mlxtune_candidate_stage
')
check "enabled but serving not stopped: refuses" \
    "[[ \"\$out2\" == *'serving is not stopped'* ]]" "$out2"
check "refused: never reaches DRY RUN" \
    "[[ \"\$out2\" != *'DRY RUN'* ]]" "$out2"
rm -rf "$T2"

# ── Case 4: HU_RETRAIN_MLXTUNE=1 + HU_RETRAIN_DRY_RUN=1 -- full log sequence,
#    loads nothing. serving_stopped=1 set explicitly (the caller's global the
#    real script sets after a successful bootout).
T3=$(mktemp -d)
out3=$(HOME="$T3" HU_RETRAIN_STAGE_TEST=1 HU_RETRAIN_MLXTUNE=1 HU_RETRAIN_DRY_RUN=1 bash -c '
    source "'"$SCRIPT"'"
    serving_stopped=1
    run_mlxtune_candidate_stage
')
check "dry-run: starting line" \
    "[[ \"\$out3\" == *'mlx-tune candidate stage: starting (HU_RETRAIN_MLXTUNE=1)'* ]]" "$out3"
check "dry-run: config line" \
    "[[ \"\$out3\" == *'mlx-tune candidate stage: config='* ]]" "$out3"
check "dry-run: announces DRY RUN and loads nothing" \
    "[[ \"\$out3\" == *'DRY RUN (HU_RETRAIN_DRY_RUN=1) — loading nothing'* ]]" "$out3"
# HOME is fake, so neither hardcoded venv exists under it -- both checks must
# take the 'missing, skip' branch (never resolve to a real interpreter).
check "dry-run: mlxtune venv resolved under fake HOME is reported missing" \
    "[[ \"\$out3\" == *'mlxtune venv missing'* ]]" "$out3"
check "dry-run: eval venv resolved under fake HOME is reported missing" \
    "[[ \"\$out3\" == *'eval venv missing'* ]]" "$out3"
check "dry-run: terminal DRY RUN done line" \
    "[[ \"\$out3\" == *'mlx-tune candidate stage: DRY RUN done'* ]]" "$out3"
check "dry-run: never reaches the training-launch line" \
    "[[ \"\$out3\" != *'training -> '* ]]" "$out3"
check "dry-run: no adapters directory created" \
    "[ ! -d \"$T3/.human/training-data/adapters\" ]" "(n/a)"
# Sequence order: starting -> config -> DRY RUN announce -> ... -> DRY RUN done
seq_ok=$(python3 - "$out3" <<'PY'
import sys
out = sys.argv[1]
markers = [
    "starting (HU_RETRAIN_MLXTUNE=1)",
    "config=",
    "loading nothing",
    "DRY RUN done",
]
pos = [out.find(m) for m in markers]
print("1" if all(p >= 0 for p in pos) and pos == sorted(pos) else "0")
PY
)
check "dry-run: log lines appear in the documented order" "[ \"$seq_ok\" = 1 ]" "$out3"
rm -rf "$T3"

# ── Case 5: real (non-dry) run with no preference corpus present -- skips,
#    never fails the window ─────────────────────────────────────────────────
T4=$(mktemp -d)
out4=$(HOME="$T4" HU_RETRAIN_STAGE_TEST=1 HU_RETRAIN_MLXTUNE=1 bash -c '
    source "'"$SCRIPT"'"
    serving_stopped=1
    run_mlxtune_candidate_stage
')
check "no corpus: skips without training, exits the function cleanly" \
    "[[ \"\$out4\" == *'no preference corpus at'*'— skipping (not failing the window)'* ]]" "$out4"
rm -rf "$T4"

# ── Case 6: real run with a corpus below the floor -- skips ─────────────────
T5=$(mktemp -d)
mkdir -p "$T5/.human/training-data/glm-v61-pref"
for i in $(seq 1 5); do echo "{\"prompt\":\"p$i\",\"chosen\":\"c$i\",\"rejected\":\"r$i\"}"; done \
    > "$T5/.human/training-data/glm-v61-pref/train.jsonl"
out5=$(HOME="$T5" HU_RETRAIN_STAGE_TEST=1 HU_RETRAIN_MLXTUNE=1 bash -c '
    source "'"$SCRIPT"'"
    serving_stopped=1
    run_mlxtune_candidate_stage
')
check "below-floor corpus: skips with the pair count named" \
    "[[ \"\$out5\" == *'5 pairs < floor 200'* ]]" "$out5"
rm -rf "$T5"

exit $fail
