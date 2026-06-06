#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────
# run_orpo_ssh.sh — provider-agnostic ORPO launch over plain SSH.
#
# Why: johnb-2025 has zero GCE GPU quota in every region (2026-06-05), but
# m3_gce_orpo_remote.py is plain HuggingFace + PEFT + TRL — it runs unchanged
# on ANY CUDA box. This script does what m3_gce_train.sh does (ship trainer +
# pairs + HF token, run, fetch adapter) but over `ssh`/`scp` against a host
# YOU rented (Lambda Cloud, RunPod-with-SSH, a bare CUDA VM, etc.), so there
# is no Google quota wait and no VM lifecycle to manage here.
#
# This script does NOT provision or bill anything — you bring an already-running
# GPU host. It only uploads, runs, and downloads. (Your rented GPU bills while
# it's up; that's on the provider side.)
#
# Prereqs on the remote host: NVIDIA GPU (>=40GB for 31B QLoRA-ORPO; 80GB
# recommended), a python3 with CUDA torch (the trainer pip-installs the rest),
# and your SSH key authorized.
#
# Usage:
#   HF_TOKEN=hf_xxx scripts/run_orpo_ssh.sh \
#       --host ubuntu@1.2.3.4 \
#       --pairs ~/.human/training-data/orpo_deliberation/train.jsonl \
#       --base-model <confirmed gemma-4-31b HF base> \
#       --iters 300 --rank 16 --learning-rate 5e-6 --beta 0.1 \
#       --adapter-out ~/.human/training-data/adapters/orpo-$(date +%Y%m%d-%H%M%S)
#
#   # preview only, no upload/run:
#   scripts/run_orpo_ssh.sh --host ubuntu@1.2.3.4 --pairs ... --base-model ... --dry-run
#
# HF token resolution order: --hf-token ARG > $HF_TOKEN env > ~/.cache/huggingface/token
# ──────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REMOTE_TRAINER="m3_gce_orpo_remote.py"   # the portable HF/TRL ORPO trainer

HOST=""
SSH_KEY=""
SSH_PORT=22
PAIRS=""
BASE_MODEL=""
HF_TOKEN_ARG=""
REMOTE_PY="python3"
ITERS=300
RANK=16
LEARNING_RATE=5e-6
BETA=0.1
BATCH_SIZE=1
GRAD_ACCUM=8
ADAPTER_OUT="$HOME/.human/training-data/adapters/orpo-ssh-$(date +%Y%m%d-%H%M%S)"
DRY_RUN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --host)           HOST="$2"; shift 2 ;;
        --ssh-key)        SSH_KEY="$2"; shift 2 ;;
        --port)           SSH_PORT="$2"; shift 2 ;;
        --pairs)          PAIRS="$2"; shift 2 ;;
        --base-model)     BASE_MODEL="$2"; shift 2 ;;
        --hf-token)       HF_TOKEN_ARG="$2"; shift 2 ;;
        --remote-python)  REMOTE_PY="$2"; shift 2 ;;
        --iters)          ITERS="$2"; shift 2 ;;
        --rank)           RANK="$2"; shift 2 ;;
        --learning-rate)  LEARNING_RATE="$2"; shift 2 ;;
        --beta)           BETA="$2"; shift 2 ;;
        --batch-size)     BATCH_SIZE="$2"; shift 2 ;;
        --grad-accum)     GRAD_ACCUM="$2"; shift 2 ;;
        --adapter-out)    ADAPTER_OUT="$2"; shift 2 ;;
        --dry-run)        DRY_RUN=1; shift ;;
        -h|--help)        sed -n '2,40p' "$0"; exit 0 ;;
        *)                echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done

log()  { echo "  $*"; }
die()  { echo "  ERROR: $*" >&2; exit "${2:-1}"; }
banner(){ printf "\n══ %s ══\n" "$*"; }

# ── Pre-flight ────────────────────────────────────────────────────────
[ -n "$HOST" ]       || die "--host user@ip required" 2
[ -n "$PAIRS" ]      || die "--pairs required" 2
[ -f "$PAIRS" ]      || die "pairs file not found: $PAIRS" 2
[ -n "$BASE_MODEL" ] || die "--base-model required" 2
[ -f "$REPO_ROOT/scripts/$REMOTE_TRAINER" ] || die "trainer missing: scripts/$REMOTE_TRAINER" 2

# Resolve HF token (never printed).
HF_TOKEN_RESOLVED="$HF_TOKEN_ARG"
[ -z "$HF_TOKEN_RESOLVED" ] && HF_TOKEN_RESOLVED="${HF_TOKEN:-}"
if [ -z "$HF_TOKEN_RESOLVED" ] && [ -f "$HOME/.cache/huggingface/token" ]; then
    HF_TOKEN_RESOLVED="$(cat "$HOME/.cache/huggingface/token")"
fi
[ -n "$HF_TOKEN_RESOLVED" ] || die "no HF token (--hf-token | \$HF_TOKEN | ~/.cache/huggingface/token); gated base needs it" 2

SSH_OPTS=(-p "$SSH_PORT" -o StrictHostKeyChecking=accept-new -o ConnectTimeout=15)
SCP_OPTS=(-P "$SSH_PORT" -o StrictHostKeyChecking=accept-new -o ConnectTimeout=15)
[ -n "$SSH_KEY" ] && { SSH_OPTS+=(-i "$SSH_KEY"); SCP_OPTS+=(-i "$SSH_KEY"); }

PAIRS_LINES=$(wc -l < "$PAIRS" | tr -d ' ')

banner "Pre-flight — provider-agnostic ORPO over SSH"
log "Host:           $HOST (port $SSH_PORT)${SSH_KEY:+, key $SSH_KEY}"
log "Trainer:        scripts/$REMOTE_TRAINER (portable HF/PEFT/TRL)"
log "Pairs:          $PAIRS ($PAIRS_LINES lines)"
log "Base model:     $BASE_MODEL"
log "Hyperparams:    iters=$ITERS rank=$RANK lr=$LEARNING_RATE beta=$BETA  eff_batch=$((BATCH_SIZE*GRAD_ACCUM))"
log "Adapter out:    $ADAPTER_OUT"
log "HF token:       resolved (${#HF_TOKEN_RESOLVED} chars, not shown)"

if [ "$DRY_RUN" = "1" ]; then
    banner "DRY-RUN — would upload, run, and download. No SSH performed."
    exit 0
fi

# ── Connectivity check ────────────────────────────────────────────────
banner "Step 0 — verify host reachable + GPU present"
ssh "${SSH_OPTS[@]}" "$HOST" 'echo connected; (nvidia-smi --query-gpu=name,memory.total --format=csv,noheader 2>/dev/null || echo "NO GPU / nvidia-smi missing")' \
    || die "cannot ssh to $HOST" 1

# ── Upload trainer + pairs ────────────────────────────────────────────
banner "Step 1 — upload trainer + pairs"
scp "${SCP_OPTS[@]}" "$REPO_ROOT/scripts/$REMOTE_TRAINER" "$HOST:/tmp/orpo_train.py"
scp "${SCP_OPTS[@]}" "$PAIRS" "$HOST:/tmp/pairs.jsonl"

# ── Upload HF token (stdin → file, umask 077, never echoed) ───────────
banner "Step 2 — install HF token on host (mode 600, not logged)"
printf '%s' "$HF_TOKEN_RESOLVED" | ssh "${SSH_OPTS[@]}" "$HOST" \
    'umask 077 && mkdir -p ~/.cache/huggingface && cat > ~/.cache/huggingface/token && chmod 600 ~/.cache/huggingface/token && echo "token installed"'

# ── Train ─────────────────────────────────────────────────────────────
banner "Step 3 — ORPO train on remote GPU (this is the work)"
REMOTE_CMD="export HF_TOKEN=\$(cat ~/.cache/huggingface/token); \
    PY=$REMOTE_PY; command -v \$PY >/dev/null || PY=python3; \
    echo \"Using python: \$PY\"; \
    \$PY /tmp/orpo_train.py \
        --pairs /tmp/pairs.jsonl \
        --adapter-out /tmp/orpo_adapter \
        --base-model '$BASE_MODEL' \
        --iters $ITERS --rank $RANK --batch-size $BATCH_SIZE \
        --grad-accum $GRAD_ACCUM --learning-rate $LEARNING_RATE --beta $BETA \
        2>&1 | tee /tmp/orpo_train.log"

if ! ssh "${SSH_OPTS[@]}" "$HOST" "$REMOTE_CMD"; then
    log "Training failed on host; fetching log for forensics"
    mkdir -p "$ADAPTER_OUT"
    scp "${SCP_OPTS[@]}" "$HOST:/tmp/orpo_train.log" "$ADAPTER_OUT/" 2>/dev/null || true
    die "remote training failed; see $ADAPTER_OUT/orpo_train.log" 3
fi

# ── Download adapter + log ────────────────────────────────────────────
banner "Step 4 — download adapter + log"
mkdir -p "$ADAPTER_OUT"
scp "${SCP_OPTS[@]}" -r "$HOST:/tmp/orpo_adapter/*" "$ADAPTER_OUT/" 2>&1 | tail -2 || true
scp "${SCP_OPTS[@]}" "$HOST:/tmp/orpo_train.log" "$ADAPTER_OUT/" 2>/dev/null || true

if [ -n "$(ls -A "$ADAPTER_OUT" 2>/dev/null)" ]; then
    banner "DONE"
    log "Adapter: $ADAPTER_OUT"
    ls -la "$ADAPTER_OUT" | head -8
    log ""
    log "Next: evaluate vs seth-lora-v4-repair on the casual+multiturn sweep,"
    log "then load via the daemon's /v1/adapters/swap if it reduces deliberation."
else
    die "no adapter files downloaded — check $ADAPTER_OUT/orpo_train.log" 3
fi
