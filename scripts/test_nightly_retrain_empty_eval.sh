#!/usr/bin/env bash
# Hermetic tests for the nightly offline empty-reply eval stage
# (run_empty_reply_eval_stage in scripts/nightly-retrain.sh, gated by
# HU_RETRAIN_EMPTY_EVAL=off|shadow|live, default off).
#
# Sources nightly-retrain.sh with HU_RETRAIN_STAGE_TEST=1 (functions only; no
# window check, no launchctl, no training) under a fake HOME. The "spare
# server" is a FAKE mlx-server.py (stdlib http.server) on a throwaway high
# port: it serves /health with the adapter it was launched with and answers
# /v1/chat/completions with an empty reply on a fixed schedule per adapter.
# The REAL scripts/eval_empty_reply_rate.py measures it (including its
# MLX_EMPTY_RETRY=0 process-environment precondition) and the REAL
# scripts/empty_reply_gate.py writes the manifest. Never touches :8741, :8743,
# launchd, a model, or the real ~/.human.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
SCRIPT="$HERE/nightly-retrain.sh"
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; echo "  --- output ---"; echo "$3" | sed 's/^/  /'; fail=1; fi; }

bash -n "$SCRIPT" || { echo "FAIL bash -n $SCRIPT"; exit 1; }

FAKE_DIR="$(mktemp -d)"
trap 'rm -rf "$FAKE_DIR"' EXIT
cat > "$FAKE_DIR/mlx-server.py" <<'FAKE_SERVER'
#!/usr/bin/env python3
# FAKE mlx-server.py: /health + /v1/chat/completions. Empty-reply schedule by
# adapter dir name: "*bad*" -> every 2nd reply empty, "*serving*" -> every 4th,
# anything else -> never. Loads nothing.
import argparse, itertools, json, os, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int); ap.add_argument("--adapter-path"); ap.add_argument("--model")
ap.add_argument("--kv-bits"); ap.add_argument("--realtime", action="store_true")
a = ap.parse_args()
if os.environ.get("FAKE_SERVER_DIE") == "1":
    sys.exit(3)
name = os.path.basename(a.adapter_path)
every = 2 if "bad" in name else 4 if "serving" in name else 0
counter = itertools.count(1)
with open(os.path.join(os.environ["HOME"], ".fake-server.launches"), "a") as f:
    f.write(f"{a.port} {a.adapter_path} MLX_EMPTY_RETRY={os.environ.get('MLX_EMPTY_RETRY')}\n")
class H(BaseHTTPRequestHandler):
    def log_message(self, *x): pass
    def _send(self, obj):
        b = json.dumps(obj).encode(); self.send_response(200)
        self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b)))
        self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        self._send({"model": a.model, "active_adapter": a.adapter_path, "adapter_applied": True,
                    "tensors_loaded": 80})
    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        i = next(counter)
        content = "" if every and i % every == 0 else "yeah sounds good"
        self._send({"choices": [{"message": {"content": content}, "finish_reason": "stop"}]})
print(f"fake mlx-server on {a.port} adapter={a.adapter_path}", flush=True)
HTTPServer(("127.0.0.1", a.port), H).serve_forever()
FAKE_SERVER

# $1 = fake HOME. Lays out the serving adapter (from config.json, like prod),
# a candidate dir, and an authorship score json with the given verdict ($2).
setup_home() {
    local h=$1 verdict=${2:-PASS}
    mkdir -p "$h/.human/logs" "$h/.human/training-data/adapters/seth-glm-air-serving"
    printf '{"personalization":{"lora_adapter_path":"%s"}}\n' \
        "$h/.human/training-data/adapters/seth-glm-air-serving" > "$h/.human/config.json"
    printf '{"promotion_gate":{"verdict":"%s"}}\n' "$verdict" > "$h/.human/logs/score.json"
}

# $1 = HOME, $2 = candidate dir, $3 = spare port; extra env via the caller.
run_stage() {
    HOME="$1" HU_REPO_DIR="${TEST_REPO_DIR:-$REPO}" HU_RETRAIN_STAGE_TEST=1 HU_RETRAIN_PORT=19741 \
    HU_RETRAIN_EVAL_PORT="$3" HU_RETRAIN_EVAL_SERVER="$FAKE_DIR/mlx-server.py" \
    HU_RETRAIN_EVAL_SERVER_PY="$(command -v python3)" HU_RETRAIN_EVAL_SAMPLES=1 \
    HU_RETRAIN_EVAL_POLL_SECS=1 HU_RETRAIN_EVAL_REAP_SECS=10 \
    HU_TRAINER_PATTERN='no-such-trainer-process-xyz' HU_WIRED_LIMIT_GB=9999 \
    bash -c '
        source "'"$SCRIPT"'"
        serving_stopped=${TEST_SERVING_STOPPED:-1}
        run_empty_reply_eval_stage "$1" "$HOME/.human/logs/score.json"
    ' _ "$2" 2>&1
}
port_free() { ! lsof -nP -iTCP:"$1" -sTCP:LISTEN >/dev/null 2>&1; }
manifest_field() { python3 -c 'import json,sys; m=json.load(open(sys.argv[1]))
v=m
for k in sys.argv[2].split("."): v=v.get(k) if isinstance(v,dict) else None
print(v)' "$1" "$2" 2>/dev/null; }

# ── Case 1: OFF (unset) -- no output, no server, no manifest ────────────────
H1=$(mktemp -d); setup_home "$H1"; C1="$H1/.human/training-data/adapters/seth-glm-air-good-cand"; mkdir -p "$C1"
out1=$(run_stage "$H1" "$C1" 19748)
check "off: prints nothing (byte-identical to today)" "[ -z \"\$out1\" ]" "$out1"
check "off: no promotion manifest" "[ ! -e \"$C1/promotion_manifest.json\" ]" "(n/a)"
check "off: no spare server launched" "[ ! -e \"$H1/.fake-server.launches\" ]" "(n/a)"
out1b=$(HU_RETRAIN_EMPTY_EVAL=bogus run_stage "$H1" "$C1" 19748)
check "unknown mode value is treated as off" "[ -z \"\$out1b\" ]" "$out1b"
rm -rf "$H1"

# ── Case 2: live, candidate better than serving -> PASS, enforced ───────────
H2=$(mktemp -d); setup_home "$H2"; C2="$H2/.human/training-data/adapters/seth-glm-air-good-cand"; mkdir -p "$C2"
out2=$(HU_RETRAIN_EMPTY_EVAL=live HU_RETRAIN_EVAL_DEADLINE=none run_stage "$H2" "$C2" 19749)
check "live+better: manifest verdict PASS" "[ \"\$(manifest_field \"$C2/promotion_manifest.json\" empty_reply.verdict)\" = PASS ]" "$out2"
check "live+better: manifest is enforced" "[ \"\$(manifest_field \"$C2/promotion_manifest.json\" empty_reply.enforce)\" = True ]" "$out2"
check "live+better: candidate rate 0 vs serving 0.25" \
    "[ \"\$(manifest_field \"$C2/promotion_manifest.json\" empty_reply.candidate_rate)\" = 0.0 ] && [ \"\$(manifest_field \"$C2/promotion_manifest.json\" empty_reply.serving_rate)\" = 0.25 ]" "$out2"
check "live+better: combined promotion_gate PASS" "[ \"\$(manifest_field \"$C2/promotion_manifest.json\" promotion_gate.verdict)\" = PASS ]" "$out2"
check "live+better: serving arm ran BEFORE candidate arm, both with MLX_EMPTY_RETRY=0" \
    "[ \"\$(awk '{print \$2\" \"\$3}' \"$H2/.fake-server.launches\" | tr '\n' '|')\" = \"$H2/.human/training-data/adapters/seth-glm-air-serving MLX_EMPTY_RETRY=0|$C2 MLX_EMPTY_RETRY=0|\" ]" \
    "$(cat "$H2/.fake-server.launches" 2>/dev/null)"
check "live+better: spare port released after the stage (no second loader left behind)" "port_free 19749" "$out2"
check "live+better: logs the stage's added downtime" "[[ \"\$out2\" == *'added prod-down time'* ]]" "$out2"
rm -rf "$H2"

# ── Case 3: live, candidate WORSE -> BLOCK, combined BLOCK ──────────────────
H3=$(mktemp -d); setup_home "$H3"; C3="$H3/.human/training-data/adapters/seth-glm-air-bad-cand"; mkdir -p "$C3"
out3=$(HU_RETRAIN_EMPTY_EVAL=live HU_RETRAIN_EVAL_DEADLINE=none run_stage "$H3" "$C3" 19750)
check "live+worse: manifest verdict BLOCK" "[ \"\$(manifest_field \"$C3/promotion_manifest.json\" empty_reply.verdict)\" = BLOCK ]" "$out3"
check "live+worse: combined promotion_gate BLOCK although authorship PASS" \
    "[ \"\$(manifest_field \"$C3/promotion_manifest.json\" promotion_gate.verdict)\" = BLOCK ] && [ \"\$(manifest_field \"$C3/promotion_manifest.json\" authorship.verdict)\" = PASS ]" "$out3"
check "live+worse: log names promotion_gate=BLOCK" "[[ \"\$out3\" == *'promotion_gate=BLOCK'* ]]" "$out3"
check "live+worse: spare port released" "port_free 19750" "$out3"
rm -rf "$H3"

# ── Case 4: shadow, candidate worse -> recorded, NOT enforced ───────────────
H4=$(mktemp -d); setup_home "$H4"; C4="$H4/.human/training-data/adapters/seth-glm-air-bad-cand"; mkdir -p "$C4"
out4=$(HU_RETRAIN_EMPTY_EVAL=shadow HU_RETRAIN_EVAL_DEADLINE=none run_stage "$H4" "$C4" 19751)
check "shadow: verdict recorded (BLOCK)" "[ \"\$(manifest_field \"$C4/promotion_manifest.json\" empty_reply.verdict)\" = BLOCK ]" "$out4"
check "shadow: not enforced" "[ \"\$(manifest_field \"$C4/promotion_manifest.json\" empty_reply.enforce)\" = False ]" "$out4"
check "shadow: combined verdict is the authorship verdict, unchanged" \
    "[ \"\$(manifest_field \"$C4/promotion_manifest.json\" promotion_gate.verdict)\" = PASS ]" "$out4"
rm -rf "$H4"

# ── Case 5: serving NOT stopped -> refuse, never launch a second loader ─────
H5=$(mktemp -d); setup_home "$H5"; C5="$H5/.human/training-data/adapters/seth-glm-air-good-cand"; mkdir -p "$C5"
out5=$(TEST_SERVING_STOPPED=0 HU_RETRAIN_EMPTY_EVAL=live HU_RETRAIN_EVAL_DEADLINE=none run_stage "$H5" "$C5" 19752)
check "serving up: no spare server launched" "[ ! -e \"$H5/.fake-server.launches\" ]" "$out5"
check "serving up: INCONCLUSIVE recorded and enforced" \
    "[ \"\$(manifest_field \"$C5/promotion_manifest.json\" empty_reply.verdict)\" = INCONCLUSIVE ] && [ \"\$(manifest_field \"$C5/promotion_manifest.json\" empty_reply.enforce)\" = True ]" "$out5"
rm -rf "$H5"

# ── Case 6: past the deadline -> no arm starts, INCONCLUSIVE ────────────────
H6=$(mktemp -d); setup_home "$H6"; C6="$H6/.human/training-data/adapters/seth-glm-air-good-cand"; mkdir -p "$C6"
out6=$(HU_RETRAIN_EMPTY_EVAL=live HU_RETRAIN_EVAL_DEADLINE=00:00 run_stage "$H6" "$C6" 19753)
check "deadline: no spare server launched" "[ ! -e \"$H6/.fake-server.launches\" ]" "$out6"
check "deadline: INCONCLUSIVE names the deadline" \
    "[ \"\$(manifest_field \"$C6/promotion_manifest.json\" empty_reply.verdict)\" = INCONCLUSIVE ] && [[ \"\$(manifest_field \"$C6/promotion_manifest.json\" empty_reply.reason)\" == *deadline* ]]" "$out6"
rm -rf "$H6"

# ── Case 7: spare server dies on load -> INCONCLUSIVE, nothing left running ─
H7=$(mktemp -d); setup_home "$H7"; C7="$H7/.human/training-data/adapters/seth-glm-air-good-cand"; mkdir -p "$C7"
out7=$(FAKE_SERVER_DIE=1 HU_RETRAIN_EMPTY_EVAL=live HU_RETRAIN_EVAL_DEADLINE=none run_stage "$H7" "$C7" 19754)
check "server dies: INCONCLUSIVE" "[ \"\$(manifest_field \"$C7/promotion_manifest.json\" empty_reply.verdict)\" = INCONCLUSIVE ]" "$out7"
check "server dies: candidate arm never started" "[[ \"\$out7\" != *'candidate arm'* ]]" "$out7"
rm -rf "$H7"

# ── Case 8: spare port already taken -> refuse, INCONCLUSIVE ────────────────
H8=$(mktemp -d); setup_home "$H8"; C8="$H8/.human/training-data/adapters/seth-glm-air-good-cand"; mkdir -p "$C8"
python3 -c "import socket,time; s=socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('127.0.0.1',19755)); s.listen(1); time.sleep(20)" &
BUSY=$!; sleep 1
out8=$(HU_RETRAIN_EMPTY_EVAL=live HU_RETRAIN_EVAL_DEADLINE=none run_stage "$H8" "$C8" 19755)
kill "$BUSY" 2>/dev/null; wait "$BUSY" 2>/dev/null
check "busy spare port: no server launched" "[ ! -e \"$H8/.fake-server.launches\" ]" "$out8"
check "busy spare port: INCONCLUSIVE" "[ \"\$(manifest_field \"$C8/promotion_manifest.json\" empty_reply.verdict)\" = INCONCLUSIVE ]" "$out8"
rm -rf "$H8"

# ── Case 9: production ports are refused as the spare ───────────────────────
H9=$(mktemp -d); setup_home "$H9"; C9="$H9/.human/training-data/adapters/seth-glm-air-good-cand"; mkdir -p "$C9"
out9=$(HU_RETRAIN_EMPTY_EVAL=live HU_RETRAIN_EVAL_DEADLINE=none run_stage "$H9" "$C9" 8743)
check "port 8743 refused as the spare" "[ ! -e \"$H9/.fake-server.launches\" ] && [[ \"\$out9\" == *'8743'* ]]" "$out9"
rm -rf "$H9"

# ── Case 10: the EXIT-trap helper kills a spare left running ────────────────
out10=$(HU_RETRAIN_STAGE_TEST=1 bash -c '
    source "'"$SCRIPT"'"
    sleep 300 >/dev/null 2>&1 & EMPTY_EVAL_SPARE_PID=$!; p=$EMPTY_EVAL_SPARE_PID
    stop_empty_eval_spare
    kill -0 "$p" 2>/dev/null && echo STILL_ALIVE || echo REAPED
')
check "stop_empty_eval_spare reaps the spare server" "[[ \"\$out10\" == *REAPED* ]]" "$out10"
check "restore_serving stops the spare BEFORE bringing prod back" \
    "awk '/^restore_serving\\(\\)/{f=1} f&&/stop_empty_eval_spare/{print \"ok\"; exit} f&&/launchctl bootstrap/{exit}' \"$SCRIPT\" | grep -q ok" "(textual wiring check)"


# ── Case 11-13: empty_reply_gate.py ITSELF crashes (critic HIGH 2026-10-02) ─
# A fake repo whose empty_reply_gate.py dies. 11: it truncates the manifest
# and exits 1 (crash mid-write); 12: it dies before writing anything. Serving
# is "not stopped" so no arm runs and only the gate script is exercised. The
# manifest left behind must be read as a BLOCKING verdict by the REAL
# scripts/empty_reply_gate.py reader m3_promote.py uses -- the schema contract
# between the shell fallback and the Python reader is what this pins.
CRASH_REPO=$(mktemp -d); mkdir -p "$CRASH_REPO/scripts"
cat > "$CRASH_REPO/scripts/empty_reply_gate.py" <<'CRASH_GATE'
import os, sys
out = sys.argv[sys.argv.index("--out") + 1]
if os.environ.get("CRASH_MID_WRITE") == "1":
    open(out, "w").write('{"schema": 1, "empty_reply": {"verd')
raise SystemExit(1)
CRASH_GATE
reader_verdict() { python3 -c 'import sys; sys.path.insert(0, sys.argv[1]); import empty_reply_gate as g
v = g.enforced_empty_reply_verdict(sys.argv[2]); print(None if v is None else v.get("verdict"))' "$REPO/scripts" "$1"; }
for variant in mid_write before_write; do
    HC=$(mktemp -d); setup_home "$HC"; CC="$HC/.human/training-data/adapters/seth-glm-air-good-cand"; mkdir -p "$CC"
    outc=$(CRASH_MID_WRITE=$([ $variant = mid_write ] && echo 1 || echo 0) TEST_REPO_DIR="$CRASH_REPO" \
           TEST_SERVING_STOPPED=0 HU_RETRAIN_EMPTY_EVAL=live HU_RETRAIN_EVAL_DEADLINE=none run_stage "$HC" "$CC" 19756)
    check "gate crash ($variant), live: manifest is INCONCLUSIVE + enforced" \
        "[ \"\$(manifest_field \"$CC/promotion_manifest.json\" empty_reply.verdict)\" = INCONCLUSIVE ] && [ \"\$(manifest_field \"$CC/promotion_manifest.json\" empty_reply.enforce)\" = True ]" "$outc"
    check "gate crash ($variant), live: m3_promote's reader BLOCKS (enforced INCONCLUSIVE, not None)" \
        "[ \"\$(reader_verdict \"$CC\")\" = INCONCLUSIVE ]" "$outc"
    check "gate crash ($variant), live: log names the gate's exit status" "[[ \"\$outc\" == *'empty_reply_gate.py exited 1'* ]]" "$outc"
    check "gate crash ($variant), live: no temp file left beside the manifest" \
        "[ \"\$(ls \"$CC\" | tr '\n' ' ')\" = 'promotion_manifest.json ' ]" "$(ls -a "$CC")"
    rm -rf "$HC"
done
HS=$(mktemp -d); setup_home "$HS"; CS="$HS/.human/training-data/adapters/seth-glm-air-good-cand"; mkdir -p "$CS"
outs=$(TEST_REPO_DIR="$CRASH_REPO" TEST_SERVING_STOPPED=0 HU_RETRAIN_EMPTY_EVAL=shadow \
       HU_RETRAIN_EVAL_DEADLINE=none run_stage "$HS" "$CS" 19757)
check "gate crash, shadow: nothing enforced (no manifest written by the fallback)" \
    "[ \"\$(reader_verdict \"$CS\")\" = None ]" "$outs"
check "gate crash, shadow: the failure is still logged" "[[ \"\$outs\" == *'empty_reply_gate.py exited 1'* ]]" "$outs"
rm -rf "$HS" "$CRASH_REPO"

exit $fail
