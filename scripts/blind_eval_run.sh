#!/usr/bin/env bash
# Wave 5 — blind human-eval cadence + protocol.
#
# Orchestrates the two-phase blind persona-fidelity evaluation around the one
# step a machine cannot do: humans who know the target rating real-vs-model
# output blind. Automated metrics correlate only ~40-60% with human judgment,
# so this is the ground truth that tells you whether Waves 3/4 actually helped.
#
# PHASE 1 (export):  pairs.json -> anonymized rating sheet  + rater instructions
# PHASE 2 (ingest):  filled sheet -> per-dimension aggregate + real-vs-model win rate
#
# The pairs.json input is a list of {prompt, real_response, model_response}:
#   - real_response  : YOUR actual message (from scripts/build_heldout_corpus.py)
#   - model_response : the model's reply on the LIVE path (capture via
#                      scripts/eval_fidelity_live.py against the running daemon/
#                      MLX server, or the daemon's outbound log)
#
# Recommended cadence: run after every adapter swap (Wave 4) and at least
# monthly, so fidelity is tracked over time, not asserted once.
#
# Usage:
#   scripts/blind_eval_run.sh export --pairs pairs.json --sheet sheet.csv
#   # ... recruit raters, have each fill a COPY of sheet.csv ...
#   scripts/blind_eval_run.sh ingest --ratings filled.csv --exported sheet.jsonl --out results.json
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="${PYTHON:-python3}"

print_protocol() {
  cat <<'PROTO'
────────────────────────────────────────────────────────────────────────
BLIND RATER PROTOCOL
────────────────────────────────────────────────────────────────────────
Recruit 5–10 people who actually know the target person (Seth). Each rater
fills their OWN copy of the sheet — do NOT let them compare or discuss.

The sheet hides which response is real vs. model (blinded + shuffled). For
each item, rate 1–10 on FOUR dimensions:

  tone           — does it FEEL like how they talk? (warmth, energy, register)
  vocabulary     — word choice, slang, abbreviations, punctuation habits
  humor          — timing + style of jokes/teasing/irony (or its absence)
  decision_style — how they reason/commit/hedge when asked something

1 = "not at all this person", 10 = "indistinguishable from them".
Skip nothing; if unsure, use your gut — that's the signal.

Ground truth check: a faithful model should be statistically hard to tell
from the real responses. After ingest, a real-vs-model win rate near 50%
means the model is convincingly in-voice; a high real win rate means the
model is still distinguishable (more Wave 3/4 work needed).
────────────────────────────────────────────────────────────────────────
PROTO
}

cmd="${1:-}"; shift || true
case "$cmd" in
  export)
    pairs=""; sheet=""
    while [ $# -gt 0 ]; do case "$1" in
      --pairs) pairs="$2"; shift 2;;
      --sheet) sheet="$2"; shift 2;;
      *) echo "unknown arg: $1" >&2; exit 2;;
    esac; done
    [ -n "$pairs" ] && [ -n "$sheet" ] || { echo "usage: $0 export --pairs <f> --sheet <f>" >&2; exit 2; }
    "$PY" "$HERE/blind_eval_export.py" --input "$pairs" --output "$sheet"
    # Also emit the JSONL twin (hidden metadata) needed by ingest, if the sheet is CSV.
    case "$sheet" in
      *.csv) "$PY" "$HERE/blind_eval_export.py" --input "$pairs" --output "${sheet%.csv}.jsonl";;
    esac
    echo
    echo "[blind-eval] wrote rating sheet: $sheet"
    echo "[blind-eval] keep ${sheet%.csv}.jsonl — ingest needs it for the hidden real/model labels."
    print_protocol
    ;;
  ingest)
    ratings=""; exported=""; out="results.json"
    while [ $# -gt 0 ]; do case "$1" in
      --ratings) ratings="$2"; shift 2;;
      --exported) exported="$2"; shift 2;;
      --out) out="$2"; shift 2;;
      *) echo "unknown arg: $1" >&2; exit 2;;
    esac; done
    [ -n "$ratings" ] && [ -n "$exported" ] || { echo "usage: $0 ingest --ratings <f> --exported <f.jsonl> [--out results.json]" >&2; exit 2; }
    "$PY" "$HERE/blind_eval_ingest.py" --input "$ratings" --exported "$exported" --output "$out"
    echo "[blind-eval] aggregate written: $out"
    ;;
  *)
    cat >&2 <<EOF
Wave 5 blind human-eval runner.

  $0 export --pairs <pairs.json> --sheet <sheet.csv>
  $0 ingest --ratings <filled.csv> --exported <sheet.jsonl> [--out results.json]

pairs.json: list of {prompt, real_response, model_response}.
  real_response  <- scripts/build_heldout_corpus.py (your actual messages)
  model_response <- scripts/eval_fidelity_live.py on the live path
EOF
    exit 2
    ;;
esac
