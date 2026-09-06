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


# ── Case 7: full real-run path with a FAKE train-glm-adapter.sh -- proves the
#    caller-managed-serving ordering fix: (a) the fake wrapper is told to skip
#    its own stop/restore, (b) the stage does NOT skip offline scoring just
#    because :8741 "looks" up (it is confirmed down for real, on a throwaway
#    port so this is independent of whatever the REAL machine's :8741 is doing
#    right now), and (c) the scoring step is actually reached and invoked.
#
#    HU_RETRAIN_PORT is pinned to an unused high port so this check never
#    depends on -- or interferes with -- the real production mlx-server.
T6=$(mktemp -d)
FAKE_REPO="$T6/fake-repo"
mkdir -p "$FAKE_REPO/scripts/blind_ab"
cp "$HERE/adapter_is_real.py" "$FAKE_REPO/scripts/adapter_is_real.py"

cat > "$FAKE_REPO/scripts/train-glm-adapter.sh" <<'FAKE_TRAIN'
#!/usr/bin/env bash
# FAKE train-glm-adapter.sh for the hermetic nightly-retrain stage test.
# Records whether it was told to manage its own serving stop/restore, then
# writes a real-shaped (non-zero lora_b, scale 2.0) adapter -- deliberately
# NEVER touches launchctl or :8741, matching the caller-managed contract.
set -u
TAG=""
while [ $# -gt 0 ]; do
  case "$1" in
    --tag) TAG=$2; shift 2 ;;
    --trainer) TRAINER_ARG=$2; shift 2 ;;
    --train-mode) MODE_ARG=$2; shift 2 ;;
    --beta) BETA_ARG=$2; shift 2 ;;
    --config|--gamma|--est-minutes) shift 2 ;;
    *) shift ;;
  esac
done
{
  echo "MANAGED_BY_CALLER=${HU_TRAIN_SERVING_MANAGED_BY_CALLER:-unset}"
  echo "MATCH_EMOJI=${HU_TRAIN_MATCH_EMOJI:-unset}"
  echo "TAG=$TAG"
  echo "TRAINER=${TRAINER_ARG:-unset}"
  echo "MODE=${MODE_ARG:-unset}"
  echo "BETA=${BETA_ARG:-unset}"
} > "$HOME/.fake-train-glm-adapter.record"

STAMP="$(date +%Y%m%d%H%M%S)fake"
OUT="$HOME/.human/training-data/adapters/seth-glm-air-${TAG}-${STAMP}"
mkdir -p "$OUT"
python3 - "$OUT" <<'PY'
import json, struct, sys
d = sys.argv[1]
def f32(x):
    return struct.pack("<f", x)
a_bytes = f32(0.0) * 300000
b_bytes = f32(1.0) * 16
hdr = {
    "l.lora_a": {"dtype": "F32", "shape": [300000], "data_offsets": [0, len(a_bytes)]},
    "l.lora_b": {"dtype": "F32", "shape": [16], "data_offsets": [len(a_bytes), len(a_bytes) + len(b_bytes)]},
}
h = json.dumps(hdr).encode()
with open(d + "/adapters.safetensors", "wb") as f:
    f.write(struct.pack("<Q", len(h)))
    f.write(h)
    f.write(a_bytes)
    f.write(b_bytes)
json.dump({"lora_parameters": {"rank": 8, "scale": 2.0, "dropout": 0.0}},
          open(d + "/adapter_config.json", "w"))
PY
echo "[fake-train-glm-adapter] wrote $OUT (managed_by_caller=${HU_TRAIN_SERVING_MANAGED_BY_CALLER:-unset})"
exit 0
FAKE_TRAIN
chmod +x "$FAKE_REPO/scripts/train-glm-adapter.sh"
T6_FAKE_TRAIN_BACKUP=$(mktemp); cp "$FAKE_REPO/scripts/train-glm-adapter.sh" "$T6_FAKE_TRAIN_BACKUP"

cat > "$FAKE_REPO/scripts/blind_ab/score_candidate_offline.py" <<'FAKE_SCORE'
#!/usr/bin/env python3
# FAKE score_candidate_offline.py -- records invocation, loads nothing.
import os
import sys

record = os.path.join(os.environ["HOME"], ".fake-score.record")
with open(record, "a") as f:
    f.write("ARGS:" + " ".join(sys.argv[1:]) + "\n")
print("[fake score_candidate_offline] invoked")
sys.exit(0)
FAKE_SCORE
T6_FAKE_SCORE_BACKUP=$(mktemp); cp "$FAKE_REPO/scripts/blind_ab/score_candidate_offline.py" "$T6_FAKE_SCORE_BACKUP"

mkdir -p "$T6/.human/training-data/glm-v61-pref" "$T6/.human/venvs/eval312/bin"
for i in $(seq 1 200); do
    printf '{"prompt":"p%d","chosen":"c%d","rejected":"r%d"}\n' "$i" "$i" "$i"
done > "$T6/.human/training-data/glm-v61-pref/train.jsonl"
# eval-python only needs to be AN executable -- score_candidate_offline.py is
# the fake stub above, so this never runs real torch/mlx.
ln -s "$(command -v python3)" "$T6/.human/venvs/eval312/bin/python"

out6=$(HOME="$T6" HU_REPO_DIR="$FAKE_REPO" HU_RETRAIN_PORT=19741 \
       HU_RETRAIN_STAGE_TEST=1 HU_RETRAIN_MLXTUNE=1 bash -c '
    source "'"$SCRIPT"'"
    serving_stopped=1
    run_mlxtune_candidate_stage
')

check "real-run: fake wrapper was told to manage-by-caller" \
    "[ -f \"$T6/.fake-train-glm-adapter.record\" ] && grep -q '^MANAGED_BY_CALLER=1$' \"$T6/.fake-train-glm-adapter.record\"" \
    "$(cat "$T6/.fake-train-glm-adapter.record" 2>/dev/null)"
check "real-run: fake wrapper exited 0 and adapter guard passed" \
    "[[ \"\$out6\" == *'adapter real:'* ]]" "$out6"
check "real-run: NEVER logs the port-already-up skip (the bug being fixed)" \
    "[[ \"\$out6\" != *'is unexpectedly back up already'* && \"\$out6\" != *'skipping offline authorship scoring to avoid a second loader'* ]]" "$out6"
check "real-run: reaches and announces the scoring step" \
    "[[ \"\$out6\" == *'scoring candidate vs serving (offline LUAR)'* ]]" "$out6"
check "real-run: score_candidate_offline.py (fake) was actually invoked" \
    "[ -f \"$T6/.fake-score.record\" ] && grep -q -- '--candidate' \"$T6/.fake-score.record\"" \
    "$(cat "$T6/.fake-score.record" 2>/dev/null)"
check "real-run: never promotes -- logs the register command instead" \
    "[[ \"\$out6\" == *'candidate staged at'*'(NOT promoted)'* && \"\$out6\" == *'register_v6_adapter.py --adapter'* ]]" "$out6"
rm -rf "$T6"

# ── Case 8: same fake wrapper, but simulate it MISBEHAVING (bringing a real
#    listener up on the test port) -- the stage must still refuse to score,
#    proving the defensive fallback still fires when serving genuinely is up.
T7=$(mktemp -d)
FAKE_REPO2="$T7/fake-repo"
mkdir -p "$FAKE_REPO2/scripts"
cp "$FAKE_REPO/scripts/adapter_is_real.py" "$FAKE_REPO2/scripts/adapter_is_real.py" 2>/dev/null || cp "$HERE/adapter_is_real.py" "$FAKE_REPO2/scripts/adapter_is_real.py"
PORT2=19742
cat > "$FAKE_REPO2/scripts/train-glm-adapter.sh" <<FAKE_TRAIN2
#!/usr/bin/env bash
set -u
TAG=""
while [ \$# -gt 0 ]; do
  case "\$1" in
    --tag) TAG=\$2; shift 2 ;;
    --config|--trainer|--train-mode|--beta|--gamma|--est-minutes) shift 2 ;;
    *) shift ;;
  esac
done
STAMP="\$(date +%Y%m%d%H%M%S)fake"
OUT="\$HOME/.human/training-data/adapters/seth-glm-air-\${TAG}-\${STAMP}"
mkdir -p "\$OUT"
python3 - "\$OUT" <<'PY'
import json, struct, sys
d = sys.argv[1]
def f32(x):
    return struct.pack("<f", x)
a_bytes = f32(0.0) * 300000
b_bytes = f32(1.0) * 16
hdr = {
    "l.lora_a": {"dtype": "F32", "shape": [300000], "data_offsets": [0, len(a_bytes)]},
    "l.lora_b": {"dtype": "F32", "shape": [16], "data_offsets": [len(a_bytes), len(a_bytes) + len(b_bytes)]},
}
h = json.dumps(hdr).encode()
with open(d + "/adapters.safetensors", "wb") as f:
    f.write(struct.pack("<Q", len(h)))
    f.write(h)
    f.write(a_bytes)
    f.write(b_bytes)
json.dump({"lora_parameters": {"rank": 8, "scale": 2.0, "dropout": 0.0}},
          open(d + "/adapter_config.json", "w"))
PY
# Misbehave: bring a real listener up on the test port, simulating serving
# having come back despite the caller-managed contract.
python3 -c "
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', $PORT2))
s.listen(1)
time.sleep(6)
" &
echo \$! > "\$HOME/.fake-listener.pid"
sleep 1
exit 0
FAKE_TRAIN2
chmod +x "$FAKE_REPO2/scripts/train-glm-adapter.sh"

mkdir -p "$T7/.human/training-data/glm-v61-pref" "$T7/.human/venvs/eval312/bin"
for i in $(seq 1 200); do
    printf '{"prompt":"p%d","chosen":"c%d","rejected":"r%d"}\n' "$i" "$i" "$i"
done > "$T7/.human/training-data/glm-v61-pref/train.jsonl"
ln -s "$(command -v python3)" "$T7/.human/venvs/eval312/bin/python"

out7=$(HOME="$T7" HU_REPO_DIR="$FAKE_REPO2" HU_RETRAIN_PORT="$PORT2" \
       HU_RETRAIN_STAGE_TEST=1 HU_RETRAIN_MLXTUNE=1 bash -c '
    source "'"$SCRIPT"'"
    serving_stopped=1
    run_mlxtune_candidate_stage
')
check "misbehaving-wrapper: refuses to score when the port really is back up" \
    "[[ \"\$out7\" == *'is unexpectedly back up already'* ]]" "$out7"
check "misbehaving-wrapper: never reaches the scoring-invocation line" \
    "[[ \"\$out7\" != *'scoring candidate vs serving (offline LUAR)'* ]]" "$out7"
[ -f "$T7/.fake-listener.pid" ] && kill "$(cat "$T7/.fake-listener.pid")" 2>/dev/null
wait 2>/dev/null
rm -rf "$T7"

# ── Case 9: trainer/mode/beta knobs reach the wrapper (2026-09-05) ──────────
#    mlx_tune/simpo has never produced an adapter; the ORPO path that made the
#    served v6 adapter is selectable via HU_RETRAIN_MLXTUNE_TRAINER/MODE/BETA.
T9=$(mktemp -d)
FAKE_REPO9="$T9/repo"; mkdir -p "$FAKE_REPO9/scripts/blind_ab"
cp "$T6_FAKE_TRAIN_BACKUP" "$FAKE_REPO9/scripts/train-glm-adapter.sh"
cp "$T6_FAKE_SCORE_BACKUP" "$FAKE_REPO9/scripts/blind_ab/score_candidate_offline.py"
cp "$HERE/adapter_is_real.py" "$FAKE_REPO9/scripts/adapter_is_real.py"
chmod +x "$FAKE_REPO9/scripts/train-glm-adapter.sh"
mkdir -p "$T9/.human/training-data/glm-v61-pref" "$T9/.human/venvs/eval312/bin"
for i in $(seq 1 200); do
    printf '{"prompt":"p%d","chosen":"c%d","rejected":"r%d"}\n' "$i" "$i" "$i"
done > "$T9/.human/training-data/glm-v61-pref/train.jsonl"
ln -s "$(command -v python3)" "$T9/.human/venvs/eval312/bin/python"
out9=$(HOME="$T9" HU_REPO_DIR="$FAKE_REPO9" HU_RETRAIN_PORT=19741 \
       HU_RETRAIN_STAGE_TEST=1 HU_RETRAIN_MLXTUNE=1 \
       HU_RETRAIN_MLXTUNE_TRAINER=mlx_lm_lora HU_RETRAIN_MLXTUNE_MODE=orpo HU_RETRAIN_MLXTUNE_BETA=0.05 bash -c '
    source "'"$SCRIPT"'"
    serving_stopped=1
    run_mlxtune_candidate_stage
')
rec9="$(cat "$T9/.fake-train-glm-adapter.record" 2>/dev/null)"
check "knobs: wrapper received --trainer mlx_lm_lora" "grep -q '^TRAINER=mlx_lm_lora$' <<<\"\$rec9\"" "$rec9"
check "knobs: wrapper received --train-mode orpo" "grep -q '^MODE=orpo$' <<<\"\$rec9\"" "$rec9"
check "knobs: wrapper received --beta 0.05" "grep -q '^BETA=0.05$' <<<\"\$rec9\"" "$rec9"
check "knobs: emoji neutralisation is on by default for the candidate" "grep -q '^MATCH_EMOJI=1$' <<<\"\$rec9\"" "$rec9"
check "knobs: tag names the mode (mlxtune-orpo-...)" "grep -q '^TAG=mlxtune-orpo-' <<<\"\$rec9\"" "$rec9"
check "knobs: stage still reaches the scoring step" "[[ \"\$out9\" == *'scoring candidate vs serving (offline LUAR)'* ]]" "$out9"
rm -rf "$T9" "$T6_FAKE_TRAIN_BACKUP" "$T6_FAKE_SCORE_BACKUP"

exit $fail
