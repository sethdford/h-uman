#!/usr/bin/env bash
# Phase 6 sub-slice (Gemma throughput program) — persona-aligned draft
# adapter training orchestration.
#
# Composes three existing tools into one operator command:
#   1. `human ml lora-persona --export-jsonl` (persona bank → Alpaca JSONL)
#   2. `python3 -m mlx_lm.lora` (JSONL + base model → trained adapter)
#   3. Prints the HU_LLAMACPP_DRAFT_MODEL env var the operator should set
#
# This is a Phase 6 SUB-SLICE — Phase 6 proper requires the M3 Bridge B
# inference path landing real tensors (in progress on
# claude/blissful-agnesi-22cfd6). The training side this script
# enables can happen INDEPENDENTLY of inference being wired — the
# adapter exists as a .safetensors on disk either way; once Bridge B
# lands, it just gets consumed by the chat path.
#
# Usage:
#   scripts/train-persona-draft.sh \
#     --persona seth \
#     --base google/gemma-4-E2B-it \
#     --output ~/.human/models/seth-draft.safetensors \
#     --iters 200
#
# Required args:
#   --persona NAME           Persona to source examples from
#                            (must exist in ~/.human/personas/)
#   --base ID-OR-PATH        Base draft model (HF id or local path).
#                            CHOOSING A DRAFT FOR SPEC DECODE:
#                            The draft MUST share a tokenizer with the
#                            target. All Gemma 3 and Gemma 4 models use
#                            the same 262,144-token SentencePiece vocab,
#                            so any Gemma works as a draft for any other
#                            Gemma — but same-family raises acceptance.
#                            Recommended for gemma-4-31B targets:
#                              google/gemma-4-E2B-it (2B params,
#                              model_type=gemma4, loads with mlx_lm
#                              0.31.2, NOT gated)
#                            Future / ideal (when mlx_lm adds support):
#                              google/gemma-4-31B-it-assistant (470M,
#                              4 layers, purpose-built for spec decode,
#                              model_type=gemma4_assistant — NOT supported
#                              by mlx_lm 0.31.2 yet; track upstream)
#                            Fallback for constrained RAM:
#                              google/gemma-3-270m-it (gated; visit
#                              https://huggingface.co/google/gemma-3-270m-it
#                              and click "Acknowledge license" once)
#                            All google/gemma-4-* repos are currently NOT
#                            gated; google/gemma-3-* repos ARE gated.
#   --output PATH            Where to save the trained adapter
#
# Optional args:
#   --iters N                Training iterations (default 200)
#   --rank N                 LoRA rank (default 8)
#   --learning-rate F        Learning rate (default 1e-4)
#   --batch-size N           Batch size (default 4)
#   --max-seq-length N       Max sequence length (default 2048)
#   --export-only            Skip training; just write the JSONL.
#                            Useful for operator review before paying
#                            the multi-hour training cost.
#   --skip-model-check       Skip the up-front HF reachability probe.
#                            Use when --base is a local path that doesn't
#                            need network resolution.
#   --dry-run                Print every command that WOULD run; don't
#                            execute. Useful for CI / first-time setup
#                            without paying for actual compute.
#
# Exit codes:
#   0  success — adapter at --output, hint printed
#   1  arg validation failed
#   2  persona export failed (human binary / persona missing?)
#   3  mlx_lm.lora training failed (or mlx_lm not installed)
#   4  output adapter not produced by training (subprocess silently failed)
#   5  base model not reachable on HuggingFace (gated/typo/network)

set -euo pipefail

# ── argument parsing ──────────────────────────────────────────────────
PERSONA=""
BASE=""
OUTPUT=""
ITERS=200
RANK=8
LR="1e-4"
BS=4
MAX_SEQ=2048
EXPORT_ONLY=0
DRY_RUN=0
SKIP_MODEL_CHECK=0

usage() {
    sed -n '2,42p' "$0" | sed 's/^# \?//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --persona)         PERSONA="${2:?--persona needs a value}"; shift 2 ;;
        --base)            BASE="${2:?--base needs a value}"; shift 2 ;;
        --output)          OUTPUT="${2:?--output needs a value}"; shift 2 ;;
        --iters)           ITERS="${2:?--iters needs N}"; shift 2 ;;
        --rank)            RANK="${2:?--rank needs N}"; shift 2 ;;
        --learning-rate)   LR="${2:?--learning-rate needs F}"; shift 2 ;;
        --batch-size)      BS="${2:?--batch-size needs N}"; shift 2 ;;
        --max-seq-length)  MAX_SEQ="${2:?--max-seq-length needs N}"; shift 2 ;;
        --export-only)     EXPORT_ONLY=1; shift ;;
        --skip-model-check) SKIP_MODEL_CHECK=1; shift ;;
        --dry-run)         DRY_RUN=1; shift ;;
        -h|--help)         usage; exit 0 ;;
        *)
            echo "train-persona-draft: unknown arg: $1" >&2
            usage
            exit 1
            ;;
    esac
done

# Required args.
if [[ -z "${PERSONA}" ]]; then
    echo "train-persona-draft: --persona is required" >&2
    exit 1
fi
if [[ "${EXPORT_ONLY}" -eq 0 && -z "${BASE}" ]]; then
    echo "train-persona-draft: --base is required (unless --export-only)" >&2
    exit 1
fi
if [[ "${EXPORT_ONLY}" -eq 0 && -z "${OUTPUT}" ]]; then
    echo "train-persona-draft: --output is required (unless --export-only)" >&2
    exit 1
fi

# ── locate the human binary ──────────────────────────────────────────
# Prefer the build/human in this repo; fall back to PATH.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
HUMAN_BIN="${REPO_ROOT}/build/human"
if [[ ! -x "${HUMAN_BIN}" ]]; then
    HUMAN_BIN="$(command -v human 2>/dev/null || true)"
fi
if [[ -z "${HUMAN_BIN}" || ! -x "${HUMAN_BIN}" ]]; then
    echo "train-persona-draft: 'human' binary not found at \
${REPO_ROOT}/build/human or on PATH. Build first: cmake --build build --target human" >&2
    exit 2
fi

# ── work directory ───────────────────────────────────────────────────
WORK_DIR="$(mktemp -d -t persona-draft-XXXXXX)"
trap 'rm -rf "${WORK_DIR}"' EXIT
JSONL_PATH="${WORK_DIR}/train.jsonl"

# ── step 1: export persona bank to Alpaca JSONL ──────────────────────
echo "train-persona-draft: step 1/2 — exporting persona '${PERSONA}' to JSONL"
EXPORT_CMD=("${HUMAN_BIN}" ml lora-persona --persona "${PERSONA}" --export-jsonl "${JSONL_PATH}")
if [[ "${DRY_RUN}" -eq 1 ]]; then
    echo "  DRY: ${EXPORT_CMD[*]}"
else
    if ! "${EXPORT_CMD[@]}"; then
        echo "train-persona-draft: persona export failed" >&2
        exit 2
    fi
    if [[ ! -s "${JSONL_PATH}" ]]; then
        echo "train-persona-draft: export produced empty JSONL at ${JSONL_PATH}" >&2
        echo "  (persona '${PERSONA}' may have no example bank; populate via" >&2
        echo "   'human ml lora-persona --persona ${PERSONA} --from-history <db>')" >&2
        exit 2
    fi
    n_examples="$(wc -l < "${JSONL_PATH}")"
    echo "  exported ${n_examples} examples → ${JSONL_PATH}"
fi

if [[ "${EXPORT_ONLY}" -eq 1 ]]; then
    # Move the JSONL to a stable path so operators can inspect it.
    out_jsonl="${PWD}/${PERSONA}-bank.jsonl"
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        echo "  DRY: cp ${JSONL_PATH} ${out_jsonl}"
    else
        cp "${JSONL_PATH}" "${out_jsonl}"
    fi
    echo
    echo "train-persona-draft: --export-only — JSONL at ${out_jsonl}"
    echo "  Review it, then re-run WITHOUT --export-only to train."
    exit 0
fi

# ── step 1.5: probe HF model reachability ────────────────────────────
# mlx_lm.lora loads weights immediately on start. If --base names a
# gated, mistyped, or unauthenticated HF repo, it fails after spending
# 30+ seconds resolving the tokenizer and model config. Probe first so
# the operator learns about the gate or typo in under 2 seconds.
#
# Skip for local paths and when --skip-model-check is set.
if [[ "${SKIP_MODEL_CHECK}" -eq 0 && "${DRY_RUN}" -eq 0 && ! -e "${BASE}" ]]; then
    # Downloading config.json (small, always present in HF model repos)
    # exercises BOTH the repo-exists check AND the gate-acceptance check.
    # Model-metadata calls succeed for gated-not-accepted repos; file
    # downloads do not. We want the strong check.
    if ! python3 - "${BASE}" <<'PYEOF' >/dev/null 2>&1
import sys
from huggingface_hub import hf_hub_download
hf_hub_download(repo_id=sys.argv[1], filename="config.json")
PYEOF
    then
        echo "train-persona-draft: base model '${BASE}' not reachable on HuggingFace" >&2
        echo "  Common causes:" >&2
        echo "    1. Typo — verify the repo id at https://huggingface.co/${BASE}" >&2
        echo "    2. Gated repo — visit the model page and click 'Acknowledge license'" >&2
        echo "       (most google/gemma-* repos require this once per HF account)" >&2
        echo "    3. Not authenticated — run 'huggingface-cli login' if --base is private" >&2
        echo "    4. No network / DNS — retry when online" >&2
        echo "  Bypass: --skip-model-check (use only when --base is a local path or" >&2
        echo "  you've separately verified the model is reachable)" >&2
        exit 5
    fi
fi

# ── step 2: train via mlx_lm.lora ────────────────────────────────────
# mlx_lm.lora expects a --data DIRECTORY containing train.jsonl /
# valid.jsonl. Our export produces a single train.jsonl; symlink it
# into a directory mlx_lm can consume. Validation split is optional;
# mlx_lm tolerates its absence.
echo
echo "train-persona-draft: step 2/2 — training draft adapter"
DATA_DIR="${WORK_DIR}/data"
ADAPTER_DIR="${WORK_DIR}/adapter"
mkdir -p "${DATA_DIR}" "${ADAPTER_DIR}"
if [[ "${DRY_RUN}" -eq 0 ]]; then
    ln -s "${JSONL_PATH}" "${DATA_DIR}/train.jsonl"
fi

LORA_CMD=(
    python3 -m mlx_lm.lora
    --model "${BASE}"
    --train
    --data "${DATA_DIR}"
    --iters "${ITERS}"
    --learning-rate "${LR}"
    --batch-size "${BS}"
    --num-layers "${RANK}"
    --max-seq-length "${MAX_SEQ}"
    --adapter-path "${ADAPTER_DIR}"
)

if [[ "${DRY_RUN}" -eq 1 ]]; then
    echo "  DRY: ${LORA_CMD[*]}"
    echo "  DRY: cp ${ADAPTER_DIR}/adapters.safetensors ${OUTPUT}"
    echo
    echo "train-persona-draft: --dry-run complete (nothing actually trained)"
    exit 0
fi

if ! "${LORA_CMD[@]}"; then
    echo "train-persona-draft: mlx_lm.lora training failed" >&2
    echo "  (verify mlx_lm is installed: python3 -c 'import mlx_lm')" >&2
    exit 3
fi

# Locate the produced adapter file. mlx_lm.lora writes
# adapters.safetensors into --adapter-path.
SRC_ADAPTER="${ADAPTER_DIR}/adapters.safetensors"
if [[ ! -s "${SRC_ADAPTER}" ]]; then
    echo "train-persona-draft: training reported success but no adapter at ${SRC_ADAPTER}" >&2
    echo "  (mlx_lm.lora may have silently failed — check its stderr above)" >&2
    exit 4
fi

# Move (not copy — preserves no-double-storage) to the operator's output.
mkdir -p "$(dirname "${OUTPUT}")"
mv "${SRC_ADAPTER}" "${OUTPUT}"
echo "  trained adapter → ${OUTPUT}"

# ── operator hint ────────────────────────────────────────────────────
cat <<EOM

train-persona-draft: ✓ done.

Next step — enable spec decode against this draft:

  HU_LLAMACPP_DRAFT_MODEL=${OUTPUT}
  HU_LLAMACPP_DRAFT_MIN_P=0.05
  HU_LLAMACPP_DRAFT_MAX_TOKENS=5

Or add to ~/.human/config.json (Phase 1c bridge — env still wins):

  "inference": { "draft_model": "${OUTPUT}" }

Then bench the acceptance rate (Phase 5 runbook step 4):

  scripts/bench-gemma-perf.py --tag persona-draft --measure-rss \\
      --out /tmp/bench-persona-draft.json

Compare against your previous --tag q8+fa+draft run. Expect higher
acceptance rate (and thus higher TPS) than the unaligned draft, per
the Phase 6 / A3 target of ≥50% acceptance on Tier-1 channels.
EOM
