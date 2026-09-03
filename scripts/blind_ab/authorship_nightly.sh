#!/usr/bin/env bash
# authorship_nightly.sh — nightly wrapper for the LUAR authorship-gap tier (contract C4).
#
# (a) regenerates the classifier-trial contexts through the LIVE production head
#     + :8741 into ~/blind_ab_run/classifier_trials_<date>.json (today's system,
#     not a frozen snapshot).
# (b) runs authorship_gap.py (PersonalBench 5v5 LUAR-MUD) on that file, writing
#     ~/.human/logs/authorship-gap-<date>.json.
#
# Invoked by scripts/nightly-watchdog.sh's "authorship" job (marker:
# ~/.human/logs/authorship-gap-DATE.json). Never runs two model loaders: stage
# (a) only calls the ALREADY-RUNNING :8741 server; LUAR (torch/transformers,
# loaded by authorship_gap.py) is a separate CPU-side model and only starts
# after stage (a)'s HTTP calls are all done, never concurrently with them.
#
# Exit non-zero, writes nothing further, if either stage refuses.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATE="$(date +%Y-%m-%d)"
PY="${HU_EVAL_PYTHON:-$HOME/.human/venvs/eval312/bin/python}"
[ -x "$PY" ] || PY="python3"
LOGDIR="$HOME/.human/logs"
mkdir -p "$LOGDIR" "$HOME/blind_ab_run"
OUT_TRIALS="$HOME/blind_ab_run/classifier_trials_${DATE}.json"

echo "[$DATE] authorship_nightly: regenerating trials via production head + :8741 -> $OUT_TRIALS"
"$PY" "$HERE/gen_classifier_trials.py" --out "$OUT_TRIALS"
rc=$?
if [ $rc -ne 0 ]; then
    echo "[$DATE] authorship_nightly: gen_classifier_trials.py refused (rc=$rc) — not running LUAR"
    exit $rc
fi

echo "[$DATE] authorship_nightly: running authorship_gap.py (PersonalBench 5v5 LUAR-MUD)"
"$PY" "$HERE/authorship_gap.py" --trials "$OUT_TRIALS" --out "$LOGDIR/authorship-gap-${DATE}.json"
exit $?
