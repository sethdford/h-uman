#!/usr/bin/env bash
# semantic_gate_weekly.sh — weekly re-measurement of the HU_SEMANTIC_RECALL LIVE gate.
#
# Why: semantic recall was flipped shadow->live on 2026-09-03 (daemon a28d7c9b0)
# on ONE measurement (docs/plans/2026-08-02-semantic-retrieval/
# semantic-live-gate-2026-09-03-content-filter.json: n_paired 40, composite
# 0.919->0.908, EI 4.275->4.175, PROMOTE). EI dropped ~0.1 on each of three
# runs; one more such step crosses the 0.15 tolerance. Nothing re-measured it
# after the flip. This wrapper re-runs contract C1 with the SAME arguments as
# the 09-03 run, weekly, via scripts/nightly-watchdog.sh's "semantic-gate" job
# (marker: ~/.human/logs/semantic-gate-DATE.json, lookback 7 days).
#
# It never loads a model: every request is HTTP to the already-running :8741
# (chat + embeddings, X-HU-Priority: batch, one in flight). It never flips
# anything: on HOLD it writes ~/.human/alerts/semantic-gate-HOLD-<date> and a
# log line for check-learning-loops.sh / doctor-nightly; the revert
# (HU_SEMANTIC_RECALL=shadow in the service-loop plist) is a HUMAN decision —
# see spec.md "Operating LIVE".
#
# Refuses (exit 2, writes nothing) when :8741 is down, a peer gate run is in
# progress, or the production binary is missing. eval_semantic_live_gate.py
# itself refuses (exit 2, no JSON) when fewer than --min-n contexts succeed in
# BOTH arms — a verdict is never fabricated
# (.claude/rules/no-number-without-a-measurement.md).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATE="$(date +%Y-%m-%d)"
LOGDIR="$HOME/.human/logs"; ALERTDIR="$HOME/.human/alerts"
OUT="$LOGDIR/semantic-gate-${DATE}.json"
MLX="${HU_MLX_HEALTH:-http://127.0.0.1:8741/health}"
GATE="${HU_SEMANTIC_GATE_PY:-$HERE/eval_semantic_live_gate.py}"
PY="${HU_EVAL_PYTHON:-/opt/homebrew/bin/python3}"; [ -x "$PY" ] || PY="python3"
# The INSTALLED daemon is the binary whose `memory search --semantic` and
# `eval score` production actually runs; a worktree build/human may be stale.
HUMAN_BIN="${HU_HUMAN_BIN:-$HOME/.local/bin/human-daemon}"
[ -x "$HUMAN_BIN" ] || HUMAN_BIN="$HERE/../build/human"
mkdir -p "$LOGDIR"

say()    { echo "[$DATE] semantic_gate_weekly: $*"; }
refuse() { say "REFUSE — $* (nothing written)"; exit 2; }

if [ "${HU_WATCHDOG_SKIP_HEALTH:-0}" != 1 ]; then
    curl -s -m 5 "$MLX" >/dev/null 2>&1 || refuse ":8741 is down ($MLX)"
fi
peer="$(pgrep -f eval_semantic_live_gate 2>/dev/null | head -1 || true)"
[ -n "$peer" ] && refuse "a peer gate run is already in progress (pid $peer)"
[ -x "$HUMAN_BIN" ] || refuse "no production binary at $HUMAN_BIN"
[ -f "$GATE" ] || refuse "gate script missing at $GATE"

# Same arguments as the 2026-09-03 PROMOTE run (see that JSON's server /
# embed_url / top_k / tolerances / min_recall_coverage; n and min_n are the
# contract defaults it ran with). Contexts: the script's default corpus.
say "running $GATE against :8741 -> $OUT"
"$PY" "$GATE" \
    --server http://127.0.0.1:8741 --embed-url http://127.0.0.1:8741 \
    --n 40 --min-n 30 --top-k 5 --min-recall-coverage 0.5 \
    --composite-tolerance 0.02 --ei-tolerance 0.15 --reality-tolerance 0.15 \
    --human-bin "$HUMAN_BIN" --out "$OUT"
rc=$?
if [ ! -s "$OUT" ]; then
    say "gate refused or died (rc=$rc) — no record written, no verdict"
    exit "$rc"
fi

verdict="$("$PY" -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d.get("verdict","?"))' "$OUT" 2>/dev/null || echo '?')"
summary="$("$PY" -c 'import json,sys; d=json.load(open(sys.argv[1])); s=d.get("shadow",{}); l=d.get("live",{})
print("n_paired=%s composite %.3f->%.3f EI %.3f->%.3f reality %.3f->%.3f coverage %.2f" % (
 d.get("n_paired"), s.get("composite",0), l.get("composite",0), s.get("ei_mean",0), l.get("ei_mean",0),
 s.get("reality_mean",0), l.get("reality_mean",0), d.get("recall_coverage",0)))' "$OUT" 2>/dev/null || true)"
case "$verdict" in
    PROMOTE)
        say "semantic-gate verdict=PROMOTE $summary out=$OUT" ;;
    HOLD)
        mkdir -p "$ALERTDIR"
        {
            echo "verdict=HOLD date=$DATE record=$OUT"
            echo "$summary"
            "$PY" -c 'import json,sys; [print("reason:", r) for r in json.load(open(sys.argv[1])).get("reasons",[])]' "$OUT" 2>/dev/null
            echo "HU_SEMANTIC_RECALL was NOT changed. Revert is a human decision:"
            echo "docs/plans/2026-08-02-semantic-retrieval/spec.md — 'Operating LIVE'"
        } > "$ALERTDIR/semantic-gate-HOLD-${DATE}"
        say "semantic-gate verdict=HOLD $summary out=$OUT — HU_SEMANTIC_RECALL NOT changed; alert $ALERTDIR/semantic-gate-HOLD-${DATE}; revert is a human decision (spec.md 'Operating LIVE')" ;;
    *)
        say "semantic-gate verdict=$verdict $summary out=$OUT — not a PROMOTE; nothing changed" ;;
esac
exit "$rc"
