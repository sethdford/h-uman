#!/usr/bin/env bash
# Restart the standalone embedder when it dies; log every death with a timestamp.
PY="${HU_EMBED_PY:-$HOME/Documents/gemma-realtime-1/.venv312/bin/python3.12}"
PORT="${1:-8749}"; LOG="$HOME/.human/logs/embed-server-$PORT.log"
while true; do
  echo "[$(date '+%F %T')] starting embed_server on :$PORT" >> "$LOG"
  "$PY" "$(dirname "$0")/embed_server.py" --port "$PORT" >> "$LOG" 2>&1
  echo "[$(date '+%F %T')] embed_server exited rc=$?" >> "$LOG"
  sleep 2
done
