# ORPO deliberation-suppression retrain — runbook

**Goal:** retrain the seth persona LoRA with **ORPO** (reference-free preference
optimization) so it stops emitting ~120–340 tokens of markdown-bullet
deliberation before the terse reply. ORPO penalizes the `rejected` (deliberation)
text directly — unlike SFT-on-chosen, which only rewards brevity.

Built & verified 2026-06-05 (#2 of the deliberation-tax fixes). The GPU run is
gated on infra you provide (GCE has **0 GPU quota** on johnb-2025 in all regions).

## Artifacts (all in `scripts/`)

| File | What |
|---|---|
| `build_orpo_deliberation_set.py` | Consolidates the preference data → `~/.human/training-data/orpo_deliberation/{train,valid}.jsonl`. **Already run: 594 pairs (535/59).** |
| `m3_gce_orpo_remote.py` | Portable HF+PEFT+**TRL ORPOTrainer** (QLoRA, ref-free). Runs on any CUDA box. |
| `run_orpo_ssh.sh` | **Provider-agnostic** launcher — ships trainer+data+HF-token to any rented GPU host over SSH, runs, downloads the adapter. |
| `m3_gce_train.sh --objective orpo` | GCE launcher (blocked: no A100 quota — needs a Google quota-increase). |

## Confirmed facts

- **Base model:** `google/gemma-4-31B-it` (capital **B** — the served
  `mlx-community/gemma-4-31b-it-8bit` declares this as its `base_model`).
- It's **multimodal** (`Gemma4ForConditionalGeneration`, image-text-to-text);
  the trainer loads via `AutoModelForImageTextToText` and LoRA-targets the
  language tower's `q/k/v/o_proj`. Text-only ORPO batches route through the text
  path. **The first remote run validates this load path** (the one thing not
  testable without the GPU + gated weights).

## To launch (non-GCE GPU path — chosen)

Prereqs you provide:
1. A rented CUDA host (≥40GB; **80GB recommended** for 31B QLoRA-ORPO) with your
   SSH key authorized — Lambda Cloud, RunPod (SSH), a bare CUDA VM, etc.
2. An **HF token with gemma-4 access** (the base is gated).

```bash
HF_TOKEN=hf_xxx scripts/run_orpo_ssh.sh \
  --host <user@host> [--ssh-key ~/.ssh/key] \
  --pairs ~/.human/training-data/orpo_deliberation/train.jsonl \
  --base-model google/gemma-4-31B-it \
  --iters 300 --rank 16 --learning-rate 5e-6 --beta 0.1 \
  --adapter-out ~/.human/training-data/adapters/orpo-$(date +%Y%m%d-%H%M%S)
```
(Add `--dry-run` to preview without uploading.)

## After training — evaluate before shipping

1. Re-run the casual + multi-turn sweep (the throwaway :8742 instance method,
   headroom=0) on the new adapter vs `seth-lora-v4-repair`: confirm deliberation
   tokens drop AND voice holds (terse, lowercase, in-voice).
2. If it wins, load it live via the daemon's `/v1/adapters/swap`.
3. If deliberation isn't suppressed enough, the dataset is only **15%
   deliberation-style** rejected — re-run `build_orpo_deliberation_set.py` with
   the deliberation pairs upweighted, or raise `--beta`.

## Notes
- Server-side guard (#1) and eval pipelining (#3) already shipped & verified;
  they don't depend on this retrain.
- ORPO is the **root-cause** fix; #1 is the safety net that keeps any residual
  deliberation-garbage from reaching the user in the meantime.
