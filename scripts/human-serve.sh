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

# Prefer Python 3.13 venv (Python 3.14 has loky semaphore crash bug)
VENV_PYTHON="$HOME/Documents/gemma-realtime-1/.venv313/bin/python3.13"
if [[ -x "$VENV_PYTHON" ]]; then
    PYTHON="$VENV_PYTHON"
else
    PYTHON="python3"
fi

# Enable self-RAG verification and streaming by default
export HU_SELF_RAG_MODE=soft
export HU_SELF_RAG_STREAMING=1

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
    if lsof -ti:"$PORT" &>/dev/null; then
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
    if [[ -f "$PIDFILE" ]]; then
        local pid
        pid=$(cat "$PIDFILE" 2>/dev/null)
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            echo "Stopping MLX server (PID $pid)..."
            kill "$pid" 2>/dev/null
            rm -f "$PIDFILE"
            echo "Stopped."
            return 0
        fi
    fi
    local pids
    pids=$(lsof -ti:"$PORT" 2>/dev/null || true)
    if [[ -n "$pids" ]]; then
        echo "Stopping processes on port $PORT..."
        echo "$pids" | xargs kill 2>/dev/null
        rm -f "$PIDFILE"
        echo "Stopped."
    else
        echo "MLX server not running."
    fi
}

do_status() {
    read_config
    if is_running; then
        local pid
        pid=$(lsof -ti:"$PORT" 2>/dev/null | head -1)
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

case "${1:-status}" in
    start)   do_start ;;
    stop)    do_stop ;;
    status)  do_status ;;
    restart) do_stop; sleep 2; do_start ;;
    ensure)  do_ensure ;;
    *)
        echo "Usage: $0 {start|stop|status|restart|ensure}"
        exit 1
        ;;
esac
