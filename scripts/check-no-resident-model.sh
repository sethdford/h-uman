#!/usr/bin/env bash
# check-no-resident-model.sh — is it safe to load a model IN-PROCESS right now?
#
# The standing rule on this box is "never two Python LLM instances at once":
# the production mlx-server on :8741 holds ~56 GB wired, and a second loader
# beside it exhausts memory. 2026-09-03 04:31 the nightly fidelity step did
# exactly that (94.8 GB wired, 41 GB swap) and the server died with a Metal
# "Insufficient Memory", lingered as a ?E zombie, and the box was rebooted.
# The 2026-09-04 red-team found the Sunday m3-loop trainer and the orpo
# watcher still load in-process with no check at all.
#
# Exit codes (callers SKIP their in-process step on anything but 0):
#   0  no model server answering, no trainer process, wired memory under limit
#   1  a model server is resident (health URL answered)
#   2  a trainer/loader process is already running
#   3  wired memory already above HU_WIRED_LIMIT_GB (something else is resident)
#
# Env: HU_MLX_HEALTH_URL (default http://127.0.0.1:8741/health)
#      HU_WIRED_LIMIT_GB  (default 70)
#      HU_TRAINER_PATTERN (override the pgrep pattern; tests use this)
# Smoke test: scripts/test-check-no-resident-model.sh
set -u
HEALTH_URL="${HU_MLX_HEALTH_URL:-http://127.0.0.1:8741/health}"
WIRED_LIMIT="${HU_WIRED_LIMIT_GB:-70}"
# Same family nightly_eval.sh counts as "a trainer": anything that holds the
# base weights between phases.
PATTERN="${HU_TRAINER_PATTERN:-mlx_lm.lora|mlx_lm_lora|train-glm-adapter|mlx_tune_train|dpo_mlx_train|kto_mlx_train|grpo_mlx_train|nightly-retrain\.sh|training_loop\.py|steering_extract}"

# curl prints 000 itself on connection failure; take the first three chars
# only (an `|| echo 000` fallback once produced "000000" ≠ "000" and made a
# dead port look resident — caught by the smoke test).
code=$(curl -s -m 5 -o /dev/null -w '%{http_code}' "$HEALTH_URL" 2>/dev/null)
code=${code:0:3}
if [ -n "$code" ] && [ "$code" != "000" ]; then
    echo "RESIDENT: model server answered $HEALTH_URL (HTTP $code) — refusing to load a second model in-process"
    exit 1
fi

# Exclude our own shell (its command line may quote the pattern) and pgrep itself.
if pgrep -f "$PATTERN" 2>/dev/null | grep -qvE "^$$\$"; then
    echo "TRAINER: a loader/trainer process is running: $(pgrep -fl "$PATTERN" | grep -vE "^$$ " | head -1 | cut -c1-100)"
    exit 2
fi

if command -v vm_stat >/dev/null 2>&1; then
    wired=$(vm_stat | awk '/Pages wired down/ {gsub("\\.","",$4); printf "%d", $4*16384/1073741824}')
    if [ -n "$wired" ] && [ "$wired" -gt "$WIRED_LIMIT" ]; then
        echo "WIRED: ${wired} GB wired already (limit ${WIRED_LIMIT} GB) — something is resident"
        exit 3
    fi
fi

echo "CLEAR: no model server on $HEALTH_URL, no trainer process, wired ${wired:-?} GB"
exit 0
