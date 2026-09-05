#!/usr/bin/env bash
# Hermetic tests for the `semantic-gate` line in check-learning-loops.sh and for
# semantic_gate_weekly.sh's refuse/HOLD behaviour. Fake HOME, fake repo, fake
# gate script — no :8741, no model, no real gate run.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
T=$(mktemp -d); export HOME="$T"; export HU_STATE_DIR="$T/.human" HU_REPO_DIR="$T/repo" HU_LOOP_SKIP_CI=1
mkdir -p "$T/.human/logs" "$T/.human/training-data/adapters/fake" "$T/repo/docs/plans/2026-08-02-semantic-retrieval"
fail=0; check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
iso_days_ago() { python3 -c 'import sys;from datetime import datetime,timedelta,timezone;print((datetime.now(timezone.utc)-timedelta(days=int(sys.argv[1]))).isoformat())' "$1"; }
record() { # path verdict days-old
  printf '{"schema":"semantic_live_gate.v2","generated_at":"%s","verdict":"%s","n_paired":40,"recall_coverage":1.0,"reasons":["ei dropped"],"shadow":{"composite":0.919,"ei_mean":4.275,"reality_mean":4.9},"live":{"composite":0.908,"ei_mean":4.175,"reality_mean":5.0}}\n' "$(iso_days_ago "$3")" "$2" > "$1"
}
# Satisfy sections 1-3 so the exit code reflects ONLY the semantic-gate line.
head -c 200000 /dev/zero > "$T/.human/training-data/adapters/fake/adapters.safetensors"
echo '{}' > "$T/.human/blind_ab_gate.json"
PLIST="$T/service-loop.plist"; export HU_SERVICE_PLIST="$PLIST"
printf '<dict><key>HU_SEMANTIC_RECALL</key>\n<string>live</string></dict>\n' > "$PLIST"
CHK="$HERE/check-learning-loops.sh"

# 0. adapters: a quarantined `.rejected-*` dir (nightly-retrain.sh's own convention
#    for a fabricated adapter) must never be picked as the "newest adapter", even
#    when its mtime is newer than the real one.
mkdir -p "$T/.human/training-data/adapters/fake-noop-glm.rejected-1788515667"
head -c 349 /dev/zero > "$T/.human/training-data/adapters/fake-noop-glm.rejected-1788515667/adapters.safetensors"
touch -t 203001010000 "$T/.human/training-data/adapters/fake-noop-glm.rejected-1788515667"
out=$(bash "$CHK" 2>&1)
check "quarantined .rejected adapter dir is ignored" "[[ \"$out\" == *'OK   adapters: fake '* ]] && [[ \"$out\" != *'rejected'* ]]"

# 1. no record at all while LIVE -> DEAD
out=$(bash "$CHK" 2>&1); rc=$?
check "no record while live is DEAD" "[[ \"$out\" == *'DEAD semantic-gate: no semantic-gate-'* ]] && [ $rc -eq 1 ]"
# 2. fresh PROMOTE -> OK, exit 0
record "$T/.human/logs/semantic-gate-a.json" PROMOTE 3
out=$(bash "$CHK" 2>&1); rc=$?
check "fresh PROMOTE is OK" "[[ \"$out\" == *'OK   semantic-gate: semantic-gate-a.json PROMOTE 3d old'* ]] && [ $rc -eq 0 ]"
# 3. stale PROMOTE (12d) -> DEAD
record "$T/.human/logs/semantic-gate-a.json" PROMOTE 12
out=$(bash "$CHK" 2>&1); rc=$?
check "12-day-old PROMOTE is stale/DEAD" "[[ \"$out\" == *'DEAD semantic-gate: newest PROMOTE semantic-gate-a.json is 12d old'* ]] && [ $rc -eq 1 ]"
# 4. newest record HOLD (by generated_at, not filename) beats an older PROMOTE -> DEAD, names the human decision
record "$T/.human/logs/semantic-gate-a.json" PROMOTE 5
record "$T/.human/logs/semantic-gate-b.json" HOLD 1
out=$(bash "$CHK" 2>&1); rc=$?
check "newest HOLD is DEAD and says revert is human" "[[ \"$out\" == *'DEAD semantic-gate: newest record semantic-gate-b.json is HOLD'* ]] && [[ \"$out\" == *'human decision'* ]] && [ $rc -eq 1 ]"
# 5. promotion records under the plan dir count too
rm -f "$T/.human/logs/semantic-gate-b.json"
record "$T/repo/docs/plans/2026-08-02-semantic-retrieval/semantic-live-gate-x.json" PROMOTE 0
out=$(bash "$CHK" 2>&1); rc=$?
check "plan-dir promotion record is found" "[[ \"$out\" == *'OK   semantic-gate: semantic-live-gate-x.json PROMOTE 0d old'* ]]"
# 6. after a revert to shadow the gate is a NOTE, not DEAD
rm -f "$T/.human/logs"/semantic-gate-*.json "$T/repo/docs/plans/2026-08-02-semantic-retrieval"/*.json
printf '<dict><key>HU_SEMANTIC_RECALL</key>\n<string>shadow</string></dict>\n' > "$PLIST"
out=$(bash "$CHK" 2>&1); rc=$?
check "shadow mode -> NOTE and exit 0" "[[ \"$out\" == *'NOTE semantic-gate: HU_SEMANTIC_RECALL=shadow'* ]] && [ $rc -eq 0 ]"

# ── semantic_gate_weekly.sh: refuses write nothing; HOLD writes the alert and flips nothing ──
W="$HERE/semantic_gate_weekly.sh"; TODAY=$(date +%Y-%m-%d)
FAKE="$T/fake_gate.py"; FAKEBIN="$T/human-daemon"; printf '#!/bin/sh\n' > "$FAKEBIN"; chmod +x "$FAKEBIN"
cat > "$FAKE" <<'PY'
import sys, json, os
from datetime import datetime, timezone
out = sys.argv[sys.argv.index("--out") + 1]
mode = os.environ.get("FAKE_GATE_MODE", "HOLD")
if mode == "REFUSE":
    print("REFUSE: only 12 contexts succeeded in BOTH arms", file=sys.stderr); sys.exit(2)
json.dump({"generated_at": datetime.now(timezone.utc).isoformat(), "verdict": mode, "n_paired": 40,
           "recall_coverage": 1.0, "reasons": ["EI dropped 4.27->4.05"],
           "shadow": {"composite": 0.919, "ei_mean": 4.275, "reality_mean": 4.9},
           "live": {"composite": 0.90, "ei_mean": 4.05, "reality_mean": 5.0}}, open(out, "w"))
sys.exit(0 if mode == "PROMOTE" else 1)
PY
export HU_SEMANTIC_GATE_PY="$FAKE" HU_HUMAN_BIN="$FAKEBIN" HU_EVAL_PYTHON=python3
# 7. :8741 down -> refuse, nothing written
out=$(HU_MLX_HEALTH=http://127.0.0.1:1/health bash "$W" 2>&1); rc=$?
check "wrapper refuses when :8741 is down" "[[ \"$out\" == *'REFUSE — :8741 is down'* ]] && [ $rc -eq 2 ] && [ ! -e \"$T/.human/logs/semantic-gate-$TODAY.json\" ]"
export HU_WATCHDOG_SKIP_HEALTH=1
# 8. a peer gate run (argv[0] carries the script name) -> refuse
bash -c 'exec -a eval_semantic_live_gate.py-peer sleep 20' & peer=$!; sleep 0.3
out=$(bash "$W" 2>&1); rc=$?; kill $peer 2>/dev/null; wait $peer 2>/dev/null
check "wrapper refuses while a peer gate run is in progress" "[[ \"$out\" == *'REFUSE — a peer gate run is already in progress'* ]] && [ $rc -eq 2 ] && [ ! -e \"$T/.human/logs/semantic-gate-$TODAY.json\" ]"
# 9. the gate itself refuses (too few contexts) -> no record, no alert, rc propagated
out=$(FAKE_GATE_MODE=REFUSE bash "$W" 2>&1); rc=$?
check "gate refusal leaves no record and no alert" "[[ \"$out\" == *'no record written, no verdict'* ]] && [ $rc -eq 2 ] && [ ! -e \"$T/.human/logs/semantic-gate-$TODAY.json\" ] && [ ! -d \"$T/.human/alerts\" ]"
# 10. HOLD -> record + alert marker + log line; plist untouched
out=$(FAKE_GATE_MODE=HOLD bash "$W" 2>&1); rc=$?
check "HOLD writes the record" "[ -s \"$T/.human/logs/semantic-gate-$TODAY.json\" ] && [ $rc -eq 1 ]"
check "HOLD writes the alert marker with reasons" "grep -q 'reason: EI dropped' \"$T/.human/alerts/semantic-gate-HOLD-$TODAY\""
check "HOLD log line names the verdict and the human decision" "[[ \"$out\" == *'semantic-gate verdict=HOLD'* ]] && [[ \"$out\" == *'NOT changed'* ]] && [[ \"$out\" == *'human decision'* ]]"
check "HOLD did not touch the service-loop plist" "grep -q '<string>shadow</string>' \"$PLIST\""
# 11. PROMOTE -> record, no alert
rm -rf "$T/.human/alerts" "$T/.human/logs/semantic-gate-$TODAY.json"
out=$(FAKE_GATE_MODE=PROMOTE bash "$W" 2>&1); rc=$?
check "PROMOTE writes the record and no alert" "[ -s \"$T/.human/logs/semantic-gate-$TODAY.json\" ] && [ $rc -eq 0 ] && [ ! -d \"$T/.human/alerts\" ] && [[ \"$out\" == *'verdict=PROMOTE n_paired=40 composite 0.919->0.900'* ]]"
rm -rf "$T"; exit $fail
