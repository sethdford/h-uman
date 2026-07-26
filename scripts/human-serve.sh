#!/usr/bin/env bash
# human-serve.sh — Manage the local MLX model server for h-uman.
#
# Usage:
#   human-serve.sh start    # Start the server (background)
#   human-serve.sh stop     # Stop the server
#   human-serve.sh status   # Check if running
#   human-serve.sh restart  # Stop + start
#   human-serve.sh ensure   # Start only if not already running (for auto-start)
#
# Reads config from ~/.human/config.json for model/adapter/port.
# Prefers gemma-realtime mlx-server.py (TurboQuant+, speculative decode, PLE-safe).
# Falls back to turbo-serve.py → mlx_lm.server if gemma-realtime not found.

set -euo pipefail

CONFIG="$HOME/.human/config.json"
PIDFILE="$HOME/.human/mlx-server.pid"
LOGFILE="$HOME/.human/mlx-server.log"

DEFAULT_MODEL="mlx-community/gemma-4-26b-a4b-it-4bit"
DEFAULT_ADAPTER="$HOME/.human/adapters/persona"
DEFAULT_PORT=8741

GEMMA_RT_PATHS=(
    "$HOME/Documents/gemma-realtime-1/scripts/mlx-server.py"
    "$HOME/Documents/gemma-realtime/scripts/mlx-server.py"
    "$HOME/gemma-realtime/scripts/mlx-server.py"
)

# Prefer Python 3.12 venv (Python 3.14 has loky semaphore crash bug;
# python@3.13 was uninstalled 2026-07-25, killing the old .venv313)
VENV_PYTHON="$HOME/Documents/gemma-realtime-1/.venv312/bin/python3.12"
if [[ -x "$VENV_PYTHON" ]]; then
    PYTHON="$VENV_PYTHON"
else
    PYTHON="python3"
fi

# Enable self-RAG verification and streaming by default
export HU_SELF_RAG_MODE=soft
export HU_SELF_RAG_STREAMING=1

# Skip Gemma 4's +512 thinking-token budget on non-stream calls (20-40% faster).
# Set GEMMA_DISABLE_THINKING=0 in your shell before `human-serve.sh start` if you want
# the model to use its <|channel>thought ... <channel|> reasoning blocks.
export GEMMA_DISABLE_THINKING="${GEMMA_DISABLE_THINKING:-1}"

read_config() {
    if [[ -f "$CONFIG" ]] && command -v python3 &>/dev/null; then
        eval "$(python3 -c "
import json, os
try:
    with open('$CONFIG') as f:
        c = json.load(f)
    mlx = c.get('mlx_local', {})
    print(f'MODEL={mlx.get(\"model\", c.get(\"default_model\", \"$DEFAULT_MODEL\"))}')
    print(f'ADAPTER={os.path.expanduser(mlx.get(\"adapter_path\", \"$DEFAULT_ADAPTER\"))}')
    print(f'PORT={mlx.get(\"port\", $DEFAULT_PORT)}')
    print(f'REALTIME={\"true\" if mlx.get(\"realtime\", False) else \"false\"}')
    print(f'KV_BITS={mlx.get(\"kv_bits\", \"\")}')
    print(f'KV_ASYMMETRIC={\"true\" if mlx.get(\"kv_asymmetric\", False) else \"false\"}')
    print(f'SPECULATIVE_DRAFT={mlx.get(\"speculative_draft\", \"\")}')
    print(f'SPECULATIVE_DRAFT_ADAPTER={os.path.expanduser(mlx.get(\"speculative_draft_adapter\", \"\"))}')
except Exception:
    print(f'MODEL=$DEFAULT_MODEL')
    print(f'ADAPTER=$DEFAULT_ADAPTER')
    print(f'PORT=$DEFAULT_PORT')
    print('REALTIME=false')
    print('KV_BITS=')
    print('KV_ASYMMETRIC=false')
    print('SPECULATIVE_DRAFT=')
    print('SPECULATIVE_DRAFT_ADAPTER=')
" 2>/dev/null)"
    else
        MODEL="$DEFAULT_MODEL"
        ADAPTER="$DEFAULT_ADAPTER"
        PORT="$DEFAULT_PORT"
        REALTIME="false"
        KV_BITS=""
        KV_ASYMMETRIC="false"
        SPECULATIVE_DRAFT=""
        SPECULATIVE_DRAFT_ADAPTER=""
    fi
}

find_server_script() {
    for p in "${GEMMA_RT_PATHS[@]}"; do
        if [[ -f "$p" ]]; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

is_running() {
    if [[ -f "$PIDFILE" ]]; then
        local pid
        pid=$(cat "$PIDFILE" 2>/dev/null)
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            return 0
        fi
    fi
    if lsof -ti:"$PORT" -sTCP:LISTEN &>/dev/null; then
        return 0
    fi
    return 1
}

do_start() {
    read_config
    if is_running; then
        echo "MLX server already running on port $PORT"
        return 0
    fi

    echo "Starting MLX model server..."
    echo "  Model:   $MODEL"
    echo "  Adapter: $ADAPTER"
    echo "  Port:    $PORT"
    echo "  Log:     $LOGFILE"

    local cmd=""
    local server_script
    if server_script=$(find_server_script); then
        cmd="$PYTHON $server_script --model $MODEL --port $PORT"
        echo "  Engine:  gemma-realtime mlx-server.py"

        if [[ "$REALTIME" == "true" ]]; then
            cmd="$cmd --realtime"
            echo "  Mode:    real-time voice (TurboQuant+ KV compression)"
        fi
        if [[ -n "$KV_BITS" ]]; then
            cmd="$cmd --kv-bits $KV_BITS"
            echo "  KV bits: $KV_BITS"
        fi
        if [[ "$KV_ASYMMETRIC" == "true" ]]; then
            cmd="$cmd --kv-asymmetric"
            echo "  KV mode: asymmetric (K=FP16, V=turbo)"
        fi
        if [[ -n "$SPECULATIVE_DRAFT" ]]; then
            cmd="$cmd --speculative-draft $SPECULATIVE_DRAFT"
            echo "  Draft:   $SPECULATIVE_DRAFT"
        fi
        if [[ -n "$SPECULATIVE_DRAFT_ADAPTER" ]] && [[ -d "$SPECULATIVE_DRAFT_ADAPTER" ]]; then
            cmd="$cmd --speculative-draft-adapter $SPECULATIVE_DRAFT_ADAPTER"
        fi
    else
        cmd="$PYTHON -m mlx_lm.server --model $MODEL --port $PORT"
        echo "  Engine:  mlx_lm.server (fallback — install gemma-realtime for TurboQuant+)"
    fi

    if [[ -d "$ADAPTER" ]] && [[ -f "$ADAPTER/adapters.safetensors" ]]; then
        cmd="$cmd --adapter-path $ADAPTER"
        echo "  LoRA:    persona adapter loaded"
    fi

    nohup $cmd > "$LOGFILE" 2>&1 &
    local pid=$!
    echo "$pid" > "$PIDFILE"

    echo -n "  Waiting for server"
    for i in $(seq 1 60); do
        if curl -sf "http://127.0.0.1:$PORT/v1/models" &>/dev/null; then
            echo " ready!"
            echo "  PID:     $pid"
            echo "  URL:     http://127.0.0.1:$PORT"
            # Show TurboQuant+ status if using gemma-realtime
            if [[ -n "$server_script" ]]; then
                local tq
                tq=$(curl -sf "http://127.0.0.1:$PORT/health" 2>/dev/null | python3 -c "
import sys, json
try:
    h = json.loads(sys.stdin.read())
    tq = h.get('turboquant_plus', False)
    kv = h.get('kv_bits')
    tps = h.get('avg_tok_per_sec', 0)
    if tq: print(f'  TQ+:     {kv}b KV cache compression active')
    if tps > 0: print(f'  Speed:   {tps:.1f} tok/s')
except: pass
" 2>/dev/null)
                [[ -n "$tq" ]] && echo "$tq"
            fi
            return 0
        fi
        echo -n "."
        sleep 1
    done
    echo " timeout (server may still be loading model)"
    echo "  Check: tail -f $LOGFILE"
}

do_stop() {
    read_config
    # Collect EVERY process holding the server: the recorded PID plus anything
    # bound to the port. The previous version killed only the PIDFILE pid and
    # returned immediately — but `nohup $cmd &` records the wrapper pid (not the
    # python child), so the real server orphaned and a duplicate kept the port,
    # making the next `start` see "already running" and never restart cleanly.
    local pids=""
    if [[ -f "$PIDFILE" ]]; then
        local fp
        fp=$(cat "$PIDFILE" 2>/dev/null)
        [[ -n "$fp" ]] && kill -0 "$fp" 2>/dev/null && pids="$fp"
    fi
    # -sTCP:LISTEN is LOAD-BEARING. Without it `lsof -ti:$PORT` also returns
    # every CLIENT holding an established connection to the port — including
    # the h-uman daemon, which keeps one open while a request is in flight.
    # `stop` then SIGTERMs the daemon along with the server. Observed
    # 2026-07-26 during the GLM base flip: this printed 3 PIDs when only 1 was
    # ever a listener, killed the daemon (pid 1496), and a scheduled message
    # due at 04:21 never fired because nothing was alive to flush it (launchd
    # KeepAlive resurrected the daemon ~10 min later). Intermittent by nature —
    # it only bites when the daemon happens to be mid-request at stop time.
    local port_pids
    port_pids=$(lsof -ti:"$PORT" -sTCP:LISTEN 2>/dev/null || true)
    pids=$(printf '%s\n%s\n' "$pids" "$port_pids" | grep -v '^$' | sort -u || true)
    rm -f "$PIDFILE"
    if [[ -z "$pids" ]]; then
        echo "MLX server not running."
        return 0
    fi
    echo "Stopping MLX server (PIDs: $(echo "$pids" | tr '\n' ' '))..."
    echo "$pids" | xargs kill 2>/dev/null || true
    # WAIT for the port to actually free before returning, so a follow-up start
    # finds a clean port. Escalate to SIGKILL if it doesn't die within 10s.
    local waited=0
    while lsof -ti:"$PORT" -sTCP:LISTEN &>/dev/null; do
        if (( waited >= 10 )); then
            echo "  still bound after ${waited}s — sending SIGKILL"
            lsof -ti:"$PORT" -sTCP:LISTEN 2>/dev/null | xargs kill -9 2>/dev/null || true
            sleep 1
            break
        fi
        sleep 1
        ((waited++))
    done
    if lsof -ti:"$PORT" -sTCP:LISTEN &>/dev/null; then
        echo "WARNING: port $PORT still bound after SIGKILL."
    else
        echo "Stopped."
    fi
}

do_status() {
    read_config
    if is_running; then
        local pid
        pid=$(lsof -ti:"$PORT" -sTCP:LISTEN 2>/dev/null | head -1)
        echo "MLX server running on port $PORT (PID: ${pid:-unknown})"
        echo "  Model:   $MODEL"
        if [[ -d "$ADAPTER" ]]; then
            echo "  Adapter: $ADAPTER"
        fi
        curl -sf "http://127.0.0.1:$PORT/health" 2>/dev/null | python3 -c "
import sys, json
try:
    h = json.loads(sys.stdin.read())
    e = h.get('engine', 'unknown')
    tq = h.get('turboquant_plus', False)
    kv = h.get('kv_bits')
    tps = h.get('avg_tok_per_sec', 0)
    reqs = h.get('total_requests', 0)
    print(f'  Engine:  {e}')
    if tq: print(f'  TQ+:     {kv}b KV cache compression')
    if tps > 0: print(f'  Speed:   {tps:.1f} tok/s ({reqs} requests)')
    hw = h.get('hardware', {})
    chip = hw.get('chip', '')
    mem = hw.get('unified_memory_gb', 0)
    if chip: print(f'  HW:      {chip}, {mem} GB unified')
except: pass
" 2>/dev/null
    else
        echo "MLX server not running"
        return 1
    fi
}

do_ensure() {
    read_config
    if is_running; then
        return 0
    fi
    do_start
}

# do_foreground — exec the python child IN-PLACE so it becomes the
# launchd-managed process. This is the canonical launchd idiom: the
# daemon IS the launched program, and KeepAlive=true in the plist
# handles restart-on-exit.
#
# Why this exists: `do_start` uses `nohup $cmd &` to background the
# python child and returns. When launchd runs `do_start` (via the
# `ensure` subcommand), the wrapper script exits success → launchd's
# session cleanup reaps the entire process group, killing the
# nohup'd child within seconds. Empirically validated 2026-05-24:
# direct manual `start` keeps MLX alive >50s; launchd-invoked
# `ensure` killed it in ~5s every cycle.
#
# `exec` (no `&`, no nohup, no PIDFILE write) makes the python
# child REPLACE this shell in the same PID, so launchd tracks the
# real daemon and there is no parent to clean up.
#
# Companion change in ~/Library/LaunchAgents/ai.human.mlx-server.plist:
#   - ProgramArguments points at this subcommand (`foreground`)
#   - KeepAlive: { Crashed=true, SuccessfulExit=false }
#   - StartInterval removed (KeepAlive replaces it)
#   - ThrottleInterval=10 (prevent restart storm on immediate crash)
do_foreground() {
    read_config
    echo "Starting MLX model server (foreground / exec mode)..."
    echo "  Model:   $MODEL"
    echo "  Adapter: $ADAPTER"
    echo "  Port:    $PORT"
    echo "  Log:     stdout/stderr inherited (launchd captures)"

    local cmd_array=()
    local server_script
    if server_script=$(find_server_script); then
        cmd_array=("$PYTHON" "$server_script" --model "$MODEL" --port "$PORT")
        echo "  Engine:  gemma-realtime mlx-server.py"
        if [[ "$REALTIME" == "true" ]]; then
            cmd_array+=(--realtime)
            echo "  Mode:    real-time voice (TurboQuant+ KV compression)"
        fi
        if [[ -n "$KV_BITS" ]]; then
            cmd_array+=(--kv-bits "$KV_BITS")
            echo "  KV bits: $KV_BITS"
        fi
        if [[ "$KV_ASYMMETRIC" == "true" ]]; then
            cmd_array+=(--kv-asymmetric)
        fi
        if [[ -n "$SPECULATIVE_DRAFT" ]]; then
            cmd_array+=(--speculative-draft "$SPECULATIVE_DRAFT")
        fi
        if [[ -n "$SPECULATIVE_DRAFT_ADAPTER" ]] && [[ -d "$SPECULATIVE_DRAFT_ADAPTER" ]]; then
            cmd_array+=(--speculative-draft-adapter "$SPECULATIVE_DRAFT_ADAPTER")
        fi
    else
        cmd_array=("$PYTHON" -m mlx_lm.server --model "$MODEL" --port "$PORT")
        echo "  Engine:  mlx_lm.server (fallback)"
    fi

    if [[ -d "$ADAPTER" ]] && [[ -f "$ADAPTER/adapters.safetensors" ]]; then
        cmd_array+=(--adapter-path "$ADAPTER")
        echo "  LoRA:    persona adapter loaded"
    fi

    # Belt-and-suspenders: if a stale instance is still listening (e.g.
    # from a manual `start`), refuse rather than fight for the port.
    if lsof -ti:"$PORT" -sTCP:LISTEN &>/dev/null; then
        echo "ERROR: port $PORT already in use; refusing to exec"
        exit 1
    fi

    # exec replaces this shell with the python process — same PID.
    # No backgrounding, no PIDFILE write, no parent to be reaped.
    exec "${cmd_array[@]}"
}

case "${1:-status}" in
    start)       do_start ;;
    stop)        do_stop ;;
    status)      do_status ;;
    restart)     do_stop; sleep 2; do_start ;;
    ensure)      do_ensure ;;
    foreground)  do_foreground ;;
    *)
        echo "Usage: $0 {start|stop|status|restart|ensure|foreground}"
        exit 1
        ;;
esac
