# M3 Auto-Promote Enabled + GCE Training Backend (2026-05-19)

User instruction: "1 flip 3 use the Google Cloud account and CLI we have
come training VMs up there, we don't have to stay local"

Interpreted as:
  1. Flip my Slice-1 remaining-work item ("`M3_AUTO_PROMOTE=1`")
  3. Solve my Slice-3 remaining-work item (base-model gap) by moving
     training to GCE

Both shipped in this commit.

## Slice 1 — M3_AUTO_PROMOTE flipped

`scripts/ai.human.m3-loop.plist`: `M3_AUTO_PROMOTE` 0 → 1.
Plist re-installed via `launchctl unload && load`. Verified:

  $ launchctl list ai.human.m3-loop
    "Label" = "ai.human.m3-loop";
    "LastExitStatus" = 0;
  $ plistlib EnvironmentVariables.M3_AUTO_PROMOTE → '1'

Safety stack remains intact — auto-promote only fires when ALL of:
  - metadata verdict = pass (real safetensors LoRA, ≥ baseline tensors)
  - behavioral verdict ∈ {pass, skipped} (closer-to-Seth on ≥3/5
    dimensions AND not mode-collapsed)
  - M3_AUTO_PROMOTE = 1 (env opt-in)

The 50-iter overtrained adapter from earlier today would still be
BLOCKED — its diversity_check.is_collapsed=True forces verdict=regress,
which fails the behavioral gate. Calibrated adapters with 8/8 distinct
outputs pass both gates.

## Slice 3 — GCE training backend

### New files

  scripts/m3_gce_train.sh           Provisions GPU VM, uploads data,
                                     runs training, downloads adapter,
                                     deletes VM. Auto-teardown trap.
  scripts/m3_gce_train_remote.py    HuggingFace transformers + PEFT
                                     LoRA trainer that runs INSIDE
                                     the VM. Same output shape as
                                     mlx_lm.lora — interoperable with
                                     existing eval + promote scripts.
  scripts/test_m3_gce_train.sh      15-assertion verifier (dry-run only;
                                     no provisioning during tests).

### Safety stack (mandatory)

  - DRY-RUN by default. `bash m3_gce_train.sh --pairs ...` prints the
    plan + cost ceiling but does NOT provision anything.
  - `--confirm-spend` REQUIRED to actually call gcloud compute create.
  - `--max-hours N` hard cap on VM lifetime (default 2 hours).
  - Auto-teardown TRAP on EXIT/ERR — VM is deleted even if the
    training subprocess crashes, the shell loses its tty, or an
    SCP hangs.
  - Cost ceiling printed BEFORE any provisioning call.

### GPU options + pricing (us-central1-a, May 2026)

  --gpu l4         L4 24GB  / $0.71/hr   (default — fits 4B base @ 4-bit)
  --gpu a100       A100 40GB / $3.67/hr   (handles 12B/27B base)
  --gpu a100-80gb  A100 80GB / $5.07/hr   (handles 31B base; needs quota)

Current project quota (johnb-2025):
  L4:            1 GPU available
  A100/A100-80GB: 0 GPUs (would need quota request)
  GPUS_ALL_REGIONS: 3

### How the autonomous loop uses it

`scripts/m3_loop_cycle.sh` step 3 (training) checks two env vars:

  M3_TRAINING_BACKEND=local|gce  (default local)
  M3_GCE_CONFIRM_SPEND=0|1       (default 0; required for actual GCE run)

If backend=gce AND confirm_spend=1 → invokes m3_gce_train.sh, which:
  1. Provisions an L4 (or A100, via M3_GPU) VM
  2. Uploads the merged Alpaca-DPO pairs file
  3. Runs HuggingFace transformers + PEFT LoRA training
  4. SCPs the adapter back to ~/.human/training-data/adapters/gce-<ts>/
  5. Deletes the VM (via EXIT trap)

If backend=local OR confirm_spend=0 → falls through to the existing
local mlx_lm.lora path (no GCE spend).

The plist's M3_TRAINING_BACKEND is still `local` so the weekly
autonomous cycle stays free until the operator explicitly flips it
AND sets M3_GCE_CONFIRM_SPEND=1.

### Status dashboard extension

`scripts/m3_status_dashboard.py` gained a "Training backend (GCE)"
section showing:
  - current backend
  - gcloud availability + active project
  - any LIVE `m3-train-*` VMs that might be billing

If the auto-teardown trap fails for any reason (network partition
during cleanup, gcloud quota delay, etc.), the dashboard will surface
the orphaned VM so the operator can manually delete it.

## Reproduction

  # Dry-run plan (free):
  make m3-gce-dryrun

  # Live training (costs $0.71/hr × max-hours):
  make m3-gce-train     # uses M3_GPU, M3_BASE_MODEL, M3_ITERS env vars

  # Flip the autonomous loop to GCE backend
  # (requires editing the plist OR setting before manual run):
  M3_TRAINING_BACKEND=gce M3_GCE_CONFIRM_SPEND=1 bash scripts/m3_loop_cycle.sh

  # Watch for orphaned VMs:
  python3 scripts/m3_status_dashboard.py | grep -A 3 "Training backend"

## Verifier totals

  H1   test_m3_extract_corpus              48
  H2   test_m3_generate_counterfactuals    27
  H3   test_m3_active_probe                60
  H3b  test_m3_probe_collector             69
  C5   test_m3_eval_adapter                21
  hold test_m3_holdout_split               12
  beh  test_m3_behavioral_eval             40
  gce  test_m3_gce_train.sh                15  (NEW)
                                          ───
                                          292 total

## What this commit does NOT do (deliberately)

  - Does NOT actually provision a VM. The infrastructure is shipped
    and dry-run-verified; the first real spend requires explicit
    operator approval per the safety rules ("Making purchases or
    completing financial transactions" is on the explicit-permission list).
  - Does NOT flip M3_TRAINING_BACKEND from `local` to `gce` in the
    plist. That's a separate operator decision — the plumbing is
    safe (auto-teardown, cost ceiling, dual confirm) but the policy
    of "run a paid VM weekly" should be flipped intentionally.

## Status as of this commit

  Auto-promote:    ENABLED (M3_AUTO_PROMOTE=1; dual-gate enforced)
  GCE backend:     SHIPPED but not engaged (M3_TRAINING_BACKEND=local)
  Live VMs:        0 (zero billing)
  Next cycle:      Sunday 04:00 local (autonomous, local training)
  Operator path to actually train on GCE:
                   1. Confirm cost tolerance
                   2. Run `make m3-gce-train` (one-shot, manual)
                   3. OR edit plist to M3_TRAINING_BACKEND=gce +
                      M3_GCE_CONFIRM_SPEND=1 for weekly auto-GCE
