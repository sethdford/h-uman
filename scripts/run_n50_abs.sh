#!/bin/zsh
# Pipeline: run warmth + formality A/B tests sequentially
# Uses absolute paths to survive session/context changes

set -e

WORKTREE_DIR="/Users/sethford/Projects/h-uman-worktrees/mlx-steering"
SCRIPTS_DIR="${WORKTREE_DIR}/scripts"
RESULTS_DIR="${WORKTREE_DIR}/results/logs"

# Create results directory
mkdir -p "${RESULTS_DIR}"

export MLX_URL="http://127.0.0.1:8745/v1/chat/completions"
echo "[pipeline] starting n=50 steering A/B pipeline (dedicated :8745)"
echo "[pipeline] working in ${WORKTREE_DIR}"

# Gate on server readiness — a prior run burned all 100 trials against a
# still-loading server (216 connection-refused errors -> 0.0% "verdicts").
echo "[pipeline] waiting for :8743 health"
for i in $(seq 1 60); do
  if curl -s --max-time 3 http://127.0.0.1:8745/health | grep -q '"model_loaded": true'; then
    echo "[pipeline] server ready"
    break
  fi
  if [ "$i" -eq 60 ]; then
    echo "[pipeline] FATAL: server not ready after 10 min" >&2
    exit 1
  fi
  sleep 10
done

# Stage 1: warmth A/B
echo "[pipeline] stage 1/2 starting: warmth A/B (n=50, dose=0.4)"
cd "${WORKTREE_DIR}"
python3 -u "${SCRIPTS_DIR}/steering_ab.py" --n 50 --experiment warmth --dose 0.4 --capability-check
WARMTH_EXIT=$?
echo "[pipeline] stage 1/2 done (exit code: ${WARMTH_EXIT}) at $(date '+%Y-%m-%d %H:%M:%S')"

# Stage 2: formality A/B
echo "[pipeline] stage 2/2 starting: formality A/B (n=50, dose=0.4)"
cd "${WORKTREE_DIR}"
python3 -u "${SCRIPTS_DIR}/steering_ab.py" --n 50 --experiment formality --dose 0.4 --capability-check
FORMALITY_EXIT=$?
echo "[pipeline] stage 2/2 done (exit code: ${FORMALITY_EXIT}) at $(date '+%Y-%m-%d %H:%M:%S')"

# Summary
if [ ${WARMTH_EXIT} -eq 0 ] && [ ${FORMALITY_EXIT} -eq 0 ]; then
  echo "[pipeline] SUCCESS: all stages complete"
  exit 0
else
  echo "[pipeline] PARTIAL: warmth=${WARMTH_EXIT} formality=${FORMALITY_EXIT}"
  exit 1
fi
