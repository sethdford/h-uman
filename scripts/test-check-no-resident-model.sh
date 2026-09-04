#!/usr/bin/env bash
# Smoke test for scripts/check-no-resident-model.sh. Proves the guard
# discriminates: it must refuse when a server answers, refuse when a
# trainer-shaped process exists, and pass when neither holds.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
G="$HERE/check-no-resident-model.sh"
fail=0
expect() {  # expect <code> <label> -- command...
    local want=$1 label=$2; shift 2
    local out; out=$("$@" 2>&1); local got=$?
    if [ "$got" = "$want" ]; then echo "PASS  $label (exit $got)"; else echo "FAIL  $label: want $want got $got: $out"; fail=1; fi
}

# A dead port: nothing listens on :1 → CLEAR (no trainer pattern matches a nonsense token).
expect 0 "nothing resident" env HU_MLX_HEALTH_URL=http://127.0.0.1:1/health HU_TRAINER_PATTERN='zz_no_such_trainer_zz' HU_WIRED_LIMIT_GB=100000 "$G"

# A live HTTP server on a random port stands in for mlx-server → RESIDENT.
python3 -c 'import http.server,sys
s=http.server.HTTPServer(("127.0.0.1",0),http.server.SimpleHTTPRequestHandler)
print(s.server_address[1],flush=True); s.serve_forever()' > /tmp/hu_guard_port.$$ 2>/dev/null &
SRV=$!
for _ in $(seq 1 50); do [ -s /tmp/hu_guard_port.$$ ] && break; sleep 0.1; done
PORT=$(cat /tmp/hu_guard_port.$$)
expect 1 "model server resident" env HU_MLX_HEALTH_URL="http://127.0.0.1:${PORT}/health" HU_TRAINER_PATTERN='zz_no_such_trainer_zz' HU_WIRED_LIMIT_GB=100000 "$G"
kill $SRV 2>/dev/null; wait $SRV 2>/dev/null; rm -f /tmp/hu_guard_port.$$

# A trainer-shaped process (argv[0] renamed) → TRAINER.
bash -c 'exec -a hu_fake_mlx_lm_lora_probe sleep 5' &
FAKE=$!
sleep 0.3
expect 2 "trainer running" env HU_MLX_HEALTH_URL=http://127.0.0.1:1/health HU_TRAINER_PATTERN='hu_fake_mlx_lm_lora_probe' HU_WIRED_LIMIT_GB=100000 "$G"
kill $FAKE 2>/dev/null; wait $FAKE 2>/dev/null

# Wired limit of 0 GB → WIRED (whatever the box holds exceeds it).
expect 3 "wired above limit" env HU_MLX_HEALTH_URL=http://127.0.0.1:1/health HU_TRAINER_PATTERN='zz_no_such_trainer_zz' HU_WIRED_LIMIT_GB=0 "$G"

[ $fail = 0 ] && echo "OK  check-no-resident-model smoke test passed" || { echo "FAILED"; exit 1; }
