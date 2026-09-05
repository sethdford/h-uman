#!/usr/bin/env bash
# Tests for trainer_running() in nightly_eval.sh — the guard that keeps the
# fidelity pass from loading a 56 GB model in-process while something else
# holds the base. During the retrain window :8741 is DOWN on purpose, so
# "server down" alone must not mean "memory is free".
#
# Hermetic: extracts the function body from nightly_eval.sh with sed and puts
# a fake pgrep on PATH that matches the pattern it is handed against a fixture
# list of command lines (FAKE_PS) instead of the live process table.
#
# Why this exists: 2026-09-04 fixed nightly-retrain.sh so the server really
# stops at 03:07. The 04:05 eval then sees :8741 DOWN; if the retrain is still
# holding the base (training_loop.py, the steering extractor, or just the
# script between its phases) and the guard does not recognise it, the eval
# loads a second copy — the 2026-09-03 04:31 Metal OOM in a new costume.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="$HERE/nightly_eval.sh"
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; echo "  --- output ---"; echo "$3" | sed 's/^/  /'; fail=1; fi; }

bash -n "$SCRIPT" || { echo "FAIL bash -n $SCRIPT"; exit 1; }
echo "PASS bash -n $SCRIPT"

FAKES=$(mktemp -d)
cat > "$FAKES/pgrep" <<'EOF'
#!/bin/sh
# The last argument is the -f pattern; match it against the fixture list.
eval "pat=\${$#}"
grep -E -q -- "$pat" "$FAKE_PS"
EOF
chmod +x "$FAKES/pgrep"

FUNC=$(sed -n '/^trainer_running() {/,/^}/p' "$SCRIPT")
check "trainer_running() extracted from nightly_eval.sh" "[[ -n \"\$FUNC\" ]]" "$FUNC"

# probe <command line>...: exit 0 when trainer_running reports a trainer.
probe() {
    printf '%s\n' "$@" > "$FAKES/ps"
    FAKE_PS="$FAKES/ps" PATH="$FAKES:$PATH" bash -c "$FUNC"$'\n''trainer_running'
}

SERVER='/opt/homebrew/Cellar/python@3.12/bin/Python /Users/u/Documents/gemma-realtime-1/scripts/mlx-server.py --model mlx-community/GLM-4.5-Air-4bit --port 8741'

# ── Negatives: nothing holding the base → in-process is allowed ─────────────
probe "$SERVER"; rc=$?
check "server only: no trainer" "[[ $rc -ne 0 ]]" "rc=$rc"
probe "/bin/bash /Users/u/Projects/h-uman/scripts/nightly_eval.sh" "$SERVER"; rc=$?
check "eval itself + server: no trainer" "[[ $rc -ne 0 ]]" "rc=$rc"
probe "-bash"; rc=$?
check "idle shell: no trainer" "[[ $rc -ne 0 ]]" "rc=$rc"

# ── Positives: every holder of the base during the retrain window ───────────
# training_loop.py spawns `<venv python> -m mlx_lm lora ...` (space, not dot).
probe "$SERVER" "/Users/u/.venv312/bin/python3.12 -m mlx_lm lora --model x --train"; rc=$?
check "mlx_lm lora subprocess (the real argv shape): trainer" "[[ $rc -eq 0 ]]" "rc=$rc"
probe "/bin/bash /Users/u/Projects/h-uman/scripts/nightly-retrain.sh"; rc=$?
check "nightly-retrain.sh itself (covers the gaps between its phases): trainer" "[[ $rc -eq 0 ]]" "rc=$rc"
probe "/Users/u/Documents/gemma-realtime-1/.venv312/bin/python3.12 /Users/u/Projects/h-uman/scripts/training_loop.py --source-jsonl m3.jsonl --adapter-out a"; rc=$?
check "training_loop.py (preflight + its own loads): trainer" "[[ $rc -eq 0 ]]" "rc=$rc"
probe "python3.12 /Users/u/Documents/gemma-realtime-1/scripts/extract_persona_vectors.py --base GLM-4.5-Air-4bit"; rc=$?
check "steering-vector extractor (loads the full base): trainer" "[[ $rc -eq 0 ]]" "rc=$rc"
probe "/bin/bash /Users/u/Projects/h-uman/scripts/train-glm-adapter.sh"; rc=$?
check "train-glm-adapter.sh: trainer" "[[ $rc -eq 0 ]]" "rc=$rc"

rm -rf "$FAKES"
if [[ "$fail" == "0" ]]; then echo "ALL PASS"; else echo "SOME FAILED"; fi
exit "$fail"
