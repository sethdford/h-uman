#!/usr/bin/env bash
# B3 regression gate — the "always-improving" guard. Runs the synthetic blind A/B
# (gen -> judge -> score) on a fixed eval set after an adapter/code change, and
# FAILS (exit 1) if the detect-rate ROSE vs the last run (i.e. h-uman got MORE
# distinguishable). Detect should trend DOWN toward 0.5. Wire into RL-nightly.
#
# Usage:
#   regression_gate.sh <contexts.json> [--tol 0.05] [--api openai]
# State: ~/.human/blind_ab/last_detect.txt (previous detect-rate).
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BA="$REPO/scripts/blind_ab"
RUN="$(mktemp -d "${TMPDIR:-/tmp}/reggate.XXXXXX")"
STATE="$HOME/.human/blind_ab"; mkdir -p "$STATE"
PREV_FILE="$STATE/last_detect.txt"

CONTEXTS="${1:?usage: regression_gate.sh <contexts.json> [--tol N] [--api openai|anthropic]}"
TOL=0.05; API=openai; shift || true
while [[ $# -gt 0 ]]; do case "$1" in
  --tol) TOL="$2"; shift 2;; --api) API="$2"; shift 2;; *) shift;; esac; done

echo "[gate] generating h-uman replies..."
python3 "$BA/gen_huuman_replies.py" "$CONTEXTS" --out "$RUN/triples.json" --human "$REPO/build/human"
echo "[gate] building rating sheet..."
python3 "$BA/make_rating_sheet.py" "$RUN/triples.json" --out-dir "$RUN"
echo "[gate] synthetic judge (--api $API)..."
python3 "$BA/synthetic_judge.py" "$RUN/rating_sheet.csv" --api "$API" --out "$RUN/judged.csv"
echo "[gate] scoring..."
python3 "$BA/score.py" "$RUN/judged.csv" --key "$RUN/answer_key.json" --json-out "$RUN/results.json" || true

DETECT="$(python3 -c "import json;print(json.load(open('$RUN/results.json'))['detect'])")"
PREV="$(cat "$PREV_FILE" 2>/dev/null || echo "1.0")"

echo "[gate] detect=$DETECT  prev=$PREV  tol=$TOL  (target: trend toward 0.50)"
REGRESSED="$(python3 -c "print(1 if $DETECT > $PREV + $TOL else 0)")"
echo "$DETECT" > "$PREV_FILE"
rm -rf "$RUN"

if [[ "$REGRESSED" == "1" ]]; then
  echo "[gate] FAIL — detect rose ($PREV -> $DETECT): h-uman got MORE distinguishable. Block this change."
  exit 1
fi
echo "[gate] PASS — no regression (detect $PREV -> $DETECT)."
