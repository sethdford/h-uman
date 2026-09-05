#!/usr/bin/env bash
# check-learning-loops.sh — did the self-learning loops actually PRODUCE anything?
#
# Why (2026-09-01/02): `doctor` reported 0 errors while the LoRA retrain probe
# had failed 1,558 times (the scheduler exec'd a May binary), eval-nightly had
# been dark since 08-08, the nightly retrain "succeeded" with a 349-byte
# adapter in 1 s, and main CI had been red for 5 commits. Every loop reported
# health by the absence of errors. This script measures ARTIFACTS instead
# (.claude/rules/reports-success-does-nothing.md): a loop that ran and produced
# nothing counts as dead.
#
# Contract (LOOP pattern, tool-creation.md): exit 0 = every loop alive (one
# line per loop); exit 1 = at least one loop dead/degraded, with the reason and
# where to look. Read-only; safe from cron, the nightly doctor, or a pre-push.
#
# Env overrides: HU_LOOP_ADAPTER_MAX_DAYS (7), HU_LOOP_EVAL_MAX_DAYS (3),
#                HU_LOOP_ADAPTER_MIN_BYTES (100000), HU_LOOP_SKIP_CI=1,
#                HU_LOOP_SEMANTIC_MAX_DAYS (10), HU_SERVICE_PLIST (service-loop plist)
set -uo pipefail

ADAPTER_MAX_DAYS="${HU_LOOP_ADAPTER_MAX_DAYS:-7}"
EVAL_MAX_DAYS="${HU_LOOP_EVAL_MAX_DAYS:-3}"
SEMANTIC_MAX_DAYS="${HU_LOOP_SEMANTIC_MAX_DAYS:-10}"
SERVICE_PLIST="${HU_SERVICE_PLIST:-$HOME/Library/LaunchAgents/ai.human.service-loop.plist}"
ADAPTER_MIN_BYTES="${HU_LOOP_ADAPTER_MIN_BYTES:-100000}"
HUMAN_DIR="${HU_STATE_DIR:-$HOME/.human}"
REPO="${HU_REPO_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
LOG="$HUMAN_DIR/logs/service-loop-error.log"
now="$(date +%s)"
fails=0

ok()   { echo "loops: OK   $*"; }
bad()  { echo "loops: DEAD $*" >&2; fails=$((fails + 1)); }
note() { echo "loops: NOTE $*"; }

age_days() { # file -> integer days since mtime
    local m; m="$(stat -f %m "$1" 2>/dev/null || stat -c %Y "$1" 2>/dev/null || echo 0)"
    echo $(( (now - m) / 86400 ))
}

# ── 1. LoRA adapters: newest adapter is recent AND has real weights ──────
ADIR="$HUMAN_DIR/training-data/adapters"
# nightly-retrain.sh quarantines a fabricated adapter by renaming it
# <dir>.rejected-<epoch>; mtime survives the rename, so without the filter a
# quarantined no-op is "newest" and this line reports the wrong thing.
newest="$(ls -dt "$ADIR"/*/ 2>/dev/null | grep -v '\.rejected-' | head -1)"
if [[ -z "$newest" ]]; then
    bad "adapters: none under $ADIR"
else
    d="$(age_days "$newest")"
    bytes="$(find "$newest" -maxdepth 1 -type f \( -name '*.safetensors' -o -name '*.npz' \) -exec stat -f %z {} \; 2>/dev/null | sort -n | tail -1)"
    bytes="${bytes:-0}"
    if (( d > ADAPTER_MAX_DAYS )); then
        bad "adapters: newest $(basename "$newest") is ${d}d old (cap ${ADAPTER_MAX_DAYS}d) — retrain loop not producing"
    elif (( bytes < ADAPTER_MIN_BYTES )); then
        bad "adapters: newest $(basename "$newest") weights file is ${bytes} B (< ${ADAPTER_MIN_BYTES}) — a no-op 'success' (rc=0 in 1 s shape)"
    else
        ok "adapters: $(basename "$newest") ${d}d old, weights ${bytes} B"
    fi
fi

# ── 2. W14 retrain probe: the scheduler must be able to count pairs ──────
if [[ -f "$LOG" ]]; then
    # Count only the CURRENT daemon run: everything after the last startup
    # marker. Otherwise the 1,500 pre-fix failures keep the check red for
    # days after the fix ships (seen 2026-09-02).
    probe_fail="$(tail -c 3000000 "$LOG" | awk '/resuming from persisted rowid/{n=0; next} /lora_retrain_probe_failed/{n++} END{print n+0}' || true)"
    if (( probe_fail > 0 )); then
        bad "retrain-probe: ${probe_fail} 'lora_retrain_probe_failed' in the recent log — the pair-count exec is broken (stale binary on PATH? see src/ml/lora_retrain_runner.c miner_argv0)"
    else
        ok "retrain-probe: no probe failures in the recent log"
    fi
else
    note "retrain-probe: no service log at $LOG"
fi

# ── 3. Nightly eval: the product gate must have written a verdict recently ─
evf=""
for cand in "$REPO/docs/evaluation/blind_ab_gate.json" "$HUMAN_DIR/blind_ab_gate.json" "$REPO/data/eval_blinded_ab.json"; do
    [[ -f "$cand" ]] && { evf="$cand"; break; }
done
if [[ -z "$evf" ]]; then
    bad "eval-nightly: no gate/results file found"
else
    d="$(age_days "$evf")"
    if (( d > EVAL_MAX_DAYS )); then
        bad "eval-nightly: $(basename "$evf") is ${d}d old (cap ${EVAL_MAX_DAYS}d) — nightly not producing a verdict"
    else
        ok "eval-nightly: $(basename "$evf") ${d}d old"
    fi
fi

# ── 4. Main CI colour: red main means the deploy path is unverified ──────
if [[ "${HU_LOOP_SKIP_CI:-0}" != "1" ]] && command -v gh >/dev/null 2>&1; then
    ci="$(cd "$REPO" && gh run list --workflow 'Human CI' --branch main --limit 1 --json conclusion --jq '.[0].conclusion // "pending"' 2>/dev/null || echo "unknown")"
    case "$ci" in
        success) ok "ci: Human CI on main is green" ;;
        failure) bad "ci: Human CI on main is RED — fix before the next deploy (gh run list --workflow 'Human CI' --branch main)" ;;
        ""|null|pending|in_progress|queued) note "ci: Human CI on main is still running (${ci:-pending})" ;;
        *)       note "ci: Human CI on main is '$ci'" ;;
    esac
fi

# ── 5. Semantic-recall LIVE gate: the weekly re-measurement must be a fresh PROMOTE ─
# HU_SEMANTIC_RECALL went live 2026-09-03 on one measurement; the watchdog's
# semantic-gate job (scripts/semantic_gate_weekly.sh) re-runs it weekly.
# Newest record by generated_at across the weekly logs and the promotion
# records in the plan dir. HOLD, INCONCLUSIVE, or stale => DEAD. This script
# never flips anything: the revert is a human decision (spec.md, Operating LIVE).
recall_mode="live"
if [[ -f "$SERVICE_PLIST" ]]; then
    m="$(grep -A1 '<key>HU_SEMANTIC_RECALL</key>' "$SERVICE_PLIST" 2>/dev/null | sed -n 's/.*<string>\(.*\)<\/string>.*/\1/p' | head -1)"
    [[ -n "$m" ]] && recall_mode="$m"
fi
if [[ "$recall_mode" != "live" ]]; then
    note "semantic-gate: HU_SEMANTIC_RECALL=$recall_mode in $(basename "$SERVICE_PLIST") — LIVE gate not required"
else
    sg="$(python3 - "$HUMAN_DIR/logs" "$REPO/docs/plans/2026-08-02-semantic-retrieval" <<'PY'
import glob, json, os, sys
from datetime import datetime, timezone
best = None
for d in sys.argv[1:]:
    for p in glob.glob(os.path.join(d, "semantic-gate-*.json")) + glob.glob(os.path.join(d, "semantic-live-gate-*.json")):
        try:
            j = json.load(open(p))
            t = datetime.fromisoformat(str(j["generated_at"]).replace("Z", "+00:00"))
        except Exception:
            continue
        if t.tzinfo is None:
            t = t.replace(tzinfo=timezone.utc)
        if best is None or t > best[0]:
            best = (t, str(j.get("verdict", "?")), p)
if best is None:
    print("none 0 -")
else:
    print(best[1], (datetime.now(timezone.utc) - best[0]).days, best[2])
PY
)"
    read -r sg_verdict sg_age sg_path <<<"$sg"
    if [[ "$sg_verdict" == "none" ]]; then
        bad "semantic-gate: no semantic-gate-*.json record found — LIVE recall has never been re-measured (scripts/semantic_gate_weekly.sh)"
    elif [[ "$sg_verdict" != "PROMOTE" ]]; then
        bad "semantic-gate: newest record $(basename "$sg_path") is $sg_verdict (${sg_age}d old) — HU_SEMANTIC_RECALL is still live; revert is a human decision (spec.md, Operating LIVE)"
    elif (( sg_age > SEMANTIC_MAX_DAYS )); then
        bad "semantic-gate: newest PROMOTE $(basename "$sg_path") is ${sg_age}d old (cap ${SEMANTIC_MAX_DAYS}d) — weekly re-gate not producing"
    else
        ok "semantic-gate: $(basename "$sg_path") PROMOTE ${sg_age}d old"
    fi
fi

if (( fails > 0 )); then
    echo "loops: $fails loop(s) dead — RESULT_learning_loops=FAIL" >&2
    exit 1
fi
echo "loops: all alive — RESULT_learning_loops=PASS"
exit 0
