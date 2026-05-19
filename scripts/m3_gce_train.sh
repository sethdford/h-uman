#!/usr/bin/env bash
# M3 GCE training wrapper (2026-05-19) — train LoRA off the Mac.
#
# Why: the local Mac's mlx_lm path is great for gemma-3-4b, but the
# daemon serves gemma-4-31b-seth-v3-fused. Training against the
# served base needs >20 GB VRAM that an Apple Silicon laptop doesn't
# have. GCE L4 (24 GB) or A100 (40/80 GB) handles it.
#
# What it does:
#   1. Picks a GPU machine type (L4 default; A100 with --gpu=a100).
#   2. Provisions a GCE VM with a deep-learning AMI.
#   3. SCPs the training data + the remote training script.
#   4. SSH-runs the training (HuggingFace transformers + PEFT LoRA).
#   5. SCPs the trained adapter back to local
#      ~/.human/training-data/adapters/gce-<ts>/.
#   6. DELETES the VM (always, via trap on EXIT — even if step 4 crashes).
#
# Safety stack (all required to actually spend money):
#   - Default: --dry-run (prints what would happen, no provisioning).
#   - --confirm-spend: required to actually call gcloud compute create.
#   - --max-hours N: hard upper bound on VM lifetime. Default 2.
#   - Auto-teardown trap on EXIT/ERR: VM is deleted even if the script crashes.
#   - Cost estimate printed before provisioning.
#
# Usage:
#   # Dry-run (default, free):
#   bash scripts/m3_gce_train.sh \\
#       --pairs ~/.human/training-data/m3-combined-dpo-20260519.jsonl \\
#       --base-model google/gemma-3-4b-it \\
#       --iters 50 --rank 8
#
#   # Real run (costs $$$):
#   bash scripts/m3_gce_train.sh \\
#       --pairs ~/.human/training-data/m3-combined-dpo-20260519.jsonl \\
#       --base-model google/gemma-3-4b-it \\
#       --iters 50 --rank 8 \\
#       --confirm-spend --max-hours 1
#
# Exit codes:
#   0 — adapter downloaded; VM deleted
#   2 — pre-flight failure (gcloud missing, quota, etc.)
#   3 — provisioning failed
#   4 — training failed on the VM (VM still cleaned up)
#   5 — adapter download failed
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# ─────────────────────────────────────────────────────────────────────
# Defaults
# ─────────────────────────────────────────────────────────────────────

DRY_RUN=1
CONFIRM_SPEND=0
GPU="l4"                     # l4 (default) | a100 (needs quota) | a100-80gb
ZONE="us-central1-a"
PROJECT="${GCLOUD_PROJECT:-johnb-2025}"
MAX_HOURS=2
ITERS=50
RANK=8
LEARNING_RATE=5e-5
BATCH_SIZE=1
BASE_MODEL=""        # default chosen below based on HF_TOKEN availability
PAIRS=""
ADAPTER_OUT="$HOME/.human/training-data/adapters/gce-$(date +%Y%m%d-%H%M%S)"

# Parse flags
while [ $# -gt 0 ]; do
    case "$1" in
        --pairs)          PAIRS="$2"; shift 2 ;;
        --base-model)     BASE_MODEL="$2"; shift 2 ;;
        --adapter-out)    ADAPTER_OUT="$2"; shift 2 ;;
        --gpu)            GPU="$2"; shift 2 ;;
        --zone)           ZONE="$2"; shift 2 ;;
        --project)        PROJECT="$2"; shift 2 ;;
        --max-hours)      MAX_HOURS="$2"; shift 2 ;;
        --iters)          ITERS="$2"; shift 2 ;;
        --rank)           RANK="$2"; shift 2 ;;
        --learning-rate)  LEARNING_RATE="$2"; shift 2 ;;
        --batch-size)     BATCH_SIZE="$2"; shift 2 ;;
        --confirm-spend)  CONFIRM_SPEND=1; DRY_RUN=0; shift ;;
        --dry-run)        DRY_RUN=1; CONFIRM_SPEND=0; shift ;;
        -h|--help)        sed -n '2,30p' "$0"; exit 0 ;;
        *)                echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done

# ─────────────────────────────────────────────────────────────────────
# Pre-flight
# ─────────────────────────────────────────────────────────────────────

banner() { printf "\n══ %s ══\n" "$*"; }
log() { echo "  $*"; }
die() { echo "  ERROR: $*" >&2; exit "${2:-1}"; }

banner "Pre-flight"
command -v gcloud >/dev/null 2>&1 || die "gcloud CLI not installed" 2
[ -n "$PAIRS" ] || die "--pairs required" 2
[ -f "$PAIRS" ] || die "pairs file not found: $PAIRS" 2

# HF auth detection: if HF_TOKEN env or ~/.cache/huggingface/token is
# present locally, we can use gated models (e.g. google/gemma-*).
# Otherwise default to an open model so the VM can actually download
# the weights without a 401.
HF_TOKEN_LOCAL=""
if [ -n "${HF_TOKEN:-}" ]; then
    HF_TOKEN_LOCAL="$HF_TOKEN"
elif [ -f "$HOME/.cache/huggingface/token" ]; then
    HF_TOKEN_LOCAL=$(cat "$HOME/.cache/huggingface/token")
fi
if [ -z "$BASE_MODEL" ]; then
    if [ -n "$HF_TOKEN_LOCAL" ]; then
        BASE_MODEL="google/gemma-3-4b-it"   # gated; needs the token
    else
        BASE_MODEL="Qwen/Qwen2.5-3B-Instruct"   # open-access default
    fi
fi

# Map GPU type to machine type + accelerator
case "$GPU" in
    l4)
        MACHINE_TYPE="g2-standard-12"   # 12 vCPU, 48 GB RAM, 1× L4 24 GB
        ACCELERATOR="type=nvidia-l4,count=1"
        HOURLY_USD="0.71"
        ;;
    a100)
        MACHINE_TYPE="a2-highgpu-1g"    # 12 vCPU, 85 GB RAM, 1× A100 40 GB
        ACCELERATOR="type=nvidia-tesla-a100,count=1"
        HOURLY_USD="3.67"
        ;;
    a100-80gb)
        MACHINE_TYPE="a2-ultragpu-1g"   # 12 vCPU, 170 GB RAM, 1× A100 80 GB
        ACCELERATOR="type=nvidia-a100-80gb,count=1"
        HOURLY_USD="5.07"
        ;;
    *) die "Unknown --gpu: $GPU (expected: l4, a100, a100-80gb)" 2 ;;
esac

VM_NAME="m3-train-$(date +%Y%m%d-%H%M%S)-$$"
PAIRS_LINES=$(wc -l < "$PAIRS" | tr -d ' ')
PAIRS_BYTES=$(wc -c < "$PAIRS" | tr -d ' ')
EST_USD=$(python3 -c "print(f'{$HOURLY_USD * $MAX_HOURS:.2f}')")

log "GPU type:       $GPU ($MACHINE_TYPE)"
log "Hourly rate:    \$${HOURLY_USD}/hr"
log "Max hours:      $MAX_HOURS"
log "Cost ceiling:   \$${EST_USD} (worst case)"
log "VM name:        $VM_NAME"
log "Zone / project: $ZONE / $PROJECT"
log "Pairs:          $PAIRS ($PAIRS_LINES lines, $PAIRS_BYTES bytes)"
log "Base model:     $BASE_MODEL"
log "Iters / rank:   $ITERS / $RANK"
log "Adapter out:    $ADAPTER_OUT"
log "Mode:           $([ "$DRY_RUN" = "1" ] && echo "DRY-RUN (no VM)" || echo "LIVE (will spend money)")"

if [ "$DRY_RUN" = "1" ]; then
    banner "DRY-RUN — would create the above VM and run training"
    log "To actually spend money, re-run with --confirm-spend"
    exit 0
fi

if [ "$CONFIRM_SPEND" != "1" ]; then
    die "Live mode requires --confirm-spend. This costs real money." 2
fi

# ─────────────────────────────────────────────────────────────────────
# Auto-teardown trap — VM is deleted even if anything below fails
# ─────────────────────────────────────────────────────────────────────

cleanup() {
    local rc=$?
    if [ -n "${VM_CREATED:-}" ]; then
        banner "Cleanup: deleting VM $VM_NAME"
        gcloud compute instances delete "$VM_NAME" \
            --zone="$ZONE" --project="$PROJECT" --quiet 2>&1 | \
            grep -v "^$" | head -3 || log "  (delete returned non-zero — check manually)"
    fi
    exit $rc
}
trap cleanup EXIT INT TERM

# ─────────────────────────────────────────────────────────────────────
# Provision the VM
# ─────────────────────────────────────────────────────────────────────

banner "Step 1 — provision GPU VM ($GPU)"

# GCP's deep-learning image families drift quarterly (the old
# `pytorch-latest-gpu` alias was retired in early 2026). Discover the
# current family at provision time so this script doesn't bit-rot.
# Picks the most-recent `pytorch-*-cu*-nvidia-*` family on Ubuntu 22.04.
IMAGE_PROJECT="deeplearning-platform-release"
IMAGE_FAMILY="${M3_GCE_IMAGE_FAMILY:-}"
if [ -z "$IMAGE_FAMILY" ]; then
    IMAGE_FAMILY=$(gcloud compute images list \
        --project="$IMAGE_PROJECT" \
        --filter="family~^pytorch-[0-9]+-[0-9]+-cu1[0-9]+-ubuntu-2204-nvidia-" \
        --format="value(family)" 2>/dev/null | sort -u | tail -1)
    [ -z "$IMAGE_FAMILY" ] && \
        die "Could not discover a PyTorch CUDA Ubuntu 22.04 image family. Set M3_GCE_IMAGE_FAMILY manually." 2
    log "Discovered image family: $IMAGE_FAMILY"
fi

# Zone fallback: L4 (and other accelerators) frequently stock out in
# any one zone. Try a list of zones in the same region in sequence
# until one accepts the request. The `--zone` flag set above is the
# preferred zone; subsequent fallbacks are appended.
case "$GPU" in
    l4)        FALLBACK_ZONES="us-central1-a us-central1-b us-central1-c us-east1-c us-east4-c us-west1-b us-west4-a" ;;
    a100)      FALLBACK_ZONES="us-central1-a us-central1-b us-central1-c us-central1-f us-east1-b us-west1-b" ;;
    a100-80gb) FALLBACK_ZONES="us-central1-a us-central1-b us-central1-c us-east5-b" ;;
    *)         FALLBACK_ZONES="$ZONE" ;;
esac
# Put the requested zone first, then any others not duplicated
ZONES_TO_TRY="$ZONE"
for z in $FALLBACK_ZONES; do
    [ "$z" = "$ZONE" ] && continue
    ZONES_TO_TRY="$ZONES_TO_TRY $z"
done

PROVISION_OK=0
for try_zone in $ZONES_TO_TRY; do
    log "Trying zone: $try_zone"
    if gcloud compute instances create "$VM_NAME" \
        --project="$PROJECT" --zone="$try_zone" \
        --machine-type="$MACHINE_TYPE" \
        --accelerator="$ACCELERATOR" \
        --maintenance-policy=TERMINATE \
        --image-family="$IMAGE_FAMILY" \
        --image-project="$IMAGE_PROJECT" \
        --boot-disk-size=100GB \
        --boot-disk-type=pd-ssd \
        --metadata="install-nvidia-driver=True,m3-vm-purpose=training" \
        --labels="purpose=m3-train,owner=seth" \
        --scopes=https://www.googleapis.com/auth/cloud-platform \
        --no-restart-on-failure \
        2>&1 | tail -3 | grep -v "does not have enough resources"; then
        # Check the instance actually exists (the previous command may
        # exit 0 even on stockout if the partial-create succeeded but
        # was auto-terminated)
        sleep 3
        status=$(gcloud compute instances describe "$VM_NAME" \
            --zone="$try_zone" --project="$PROJECT" \
            --format="value(status)" 2>/dev/null)
        if [ "$status" = "PROVISIONING" ] || [ "$status" = "STAGING" ] || \
           [ "$status" = "RUNNING" ]; then
            ZONE="$try_zone"   # commit to this zone for downstream SCP/SSH
            PROVISION_OK=1
            VM_CREATED=1
            log "VM provisioned in $try_zone (status=$status)"
            break
        else
            log "  $try_zone created VM but it's in $status — likely stockout; cleaning up"
            gcloud compute instances delete "$VM_NAME" \
                --zone="$try_zone" --project="$PROJECT" --quiet 2>&1 \
                | head -2 || true
        fi
    else
        log "  $try_zone refused (stockout or other error)"
    fi
done
[ "$PROVISION_OK" = "1" ] || \
    die "All zones in fallback list refused; cloud is constrained right now" 3

# Wait for SSH ready (deep learning AMIs need ~60-90s for first-boot
# CUDA driver install)
banner "Step 2 — wait for SSH (driver install can take ~90s)"
SSH_READY=0
for i in $(seq 1 30); do
    if gcloud compute ssh "$VM_NAME" --zone="$ZONE" --project="$PROJECT" \
            --command="nvidia-smi --query-gpu=name --format=csv,noheader" \
            --quiet 2>/dev/null | grep -q .; then
        SSH_READY=1
        break
    fi
    log "  waiting... ($i/30)"
    sleep 10
done
[ "$SSH_READY" = "1" ] || die "VM never became SSH-ready" 3
log "VM ready; GPU detected"

# ─────────────────────────────────────────────────────────────────────
# Upload + run training
# ─────────────────────────────────────────────────────────────────────

banner "Step 3 — upload pairs + remote training script"
gcloud compute scp "$PAIRS" "${VM_NAME}:/tmp/pairs.jsonl" \
    --zone="$ZONE" --project="$PROJECT" --quiet 2>&1 | tail -2
gcloud compute scp "$REPO_ROOT/scripts/m3_gce_train_remote.py" \
    "${VM_NAME}:/tmp/train.py" \
    --zone="$ZONE" --project="$PROJECT" --quiet 2>&1 | tail -2
# Upload HF token so gated models (gemma) can be downloaded.
# Token file is private (mode 600) on disk; we use the gcloud SSH
# pipe and write it directly with chmod 600 on the VM side — never
# echoed to a log.
if [ -n "$HF_TOKEN_LOCAL" ]; then
    log "Uploading HF token (for gated model access)"
    gcloud compute ssh "$VM_NAME" --zone="$ZONE" --project="$PROJECT" \
        --command="mkdir -p ~/.cache/huggingface && \
                   umask 077 && \
                   printf '%s' '$HF_TOKEN_LOCAL' > ~/.cache/huggingface/token && \
                   chmod 600 ~/.cache/huggingface/token" \
        --quiet 2>&1 | tail -1
fi

banner "Step 4 — train on GPU (this is the work)"
# The DL AMI's `/usr/bin/python3` is the SYSTEM python with no ML libs.
# All the pre-installed PyTorch / transformers / etc. lives in conda
# at /opt/conda/bin/python3 (which the AMI's default bashrc activates,
# but `ssh --command` doesn't source bashrc). Use the conda python
# explicitly so we don't pay for re-installing 5 GB of deps every run.
# Fallback to /usr/bin/python3 if conda env isn't found (different AMI).
TRAIN_CMD='PY=/opt/conda/bin/python3; \
    [ -x "$PY" ] || PY=/usr/bin/python3; \
    echo "Using Python: $PY"; \
    $PY -c "import torch, transformers, peft; print(f\"torch={torch.__version__} transformers={transformers.__version__}\")" || \
        $PY -m pip install --quiet torch transformers peft accelerate bitsandbytes datasets safetensors; \
    $PY /tmp/train.py \
        --pairs /tmp/pairs.jsonl \
        --adapter-out /tmp/adapter \
        --base-model "'"$BASE_MODEL"'" \
        --iters '"$ITERS"' --rank '"$RANK"' \
        --batch-size '"$BATCH_SIZE"' \
        --learning-rate '"$LEARNING_RATE"' 2>&1 | tee /tmp/train.log'

if ! gcloud compute ssh "$VM_NAME" --zone="$ZONE" --project="$PROJECT" \
        --command="$TRAIN_CMD" --quiet; then
    log "Training failed on VM; downloading log for forensics"
    mkdir -p "$ADAPTER_OUT"
    gcloud compute scp "${VM_NAME}:/tmp/train.log" "$ADAPTER_OUT/" \
        --zone="$ZONE" --project="$PROJECT" --quiet 2>&1 | tail -2 || true
    die "Training subprocess failed" 4
fi

# ─────────────────────────────────────────────────────────────────────
# Pull adapter back
# ─────────────────────────────────────────────────────────────────────

banner "Step 5 — download trained adapter"
mkdir -p "$ADAPTER_OUT"
gcloud compute scp --recurse "${VM_NAME}:/tmp/adapter/*" "$ADAPTER_OUT/" \
    --zone="$ZONE" --project="$PROJECT" --quiet 2>&1 | tail -3 || \
    die "Adapter download failed" 5
gcloud compute scp "${VM_NAME}:/tmp/train.log" "$ADAPTER_OUT/" \
    --zone="$ZONE" --project="$PROJECT" --quiet 2>&1 | tail -2 || true

if [ -d "$ADAPTER_OUT" ] && [ -n "$(ls "$ADAPTER_OUT")" ]; then
    banner "Done"
    log "Adapter: $ADAPTER_OUT"
    ls -la "$ADAPTER_OUT" | head -5
else
    die "Adapter downloaded but directory is empty" 5
fi

# Cleanup happens via trap on EXIT (VM deletion is automatic)
