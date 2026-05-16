---
title: RL SOTA Closed-Loop Proof
description: Index of Phase 0–6 deliverables, CI evidence, and local reproduction steps.
status: current
date: 2026-05-16
---

# RL SOTA closed-loop proof

This document indexes the **full SOTA RL improvement loop** (Phases 0–6) and how to reproduce it locally.

## Phase completion tags

| Phase | Tag | What shipped |
|-------|-----|--------------|
| 0 | `rl-sota-phase-0-complete` | Honesty fixes, atomic personal-model save |
| 1 | `rl-sota-phase-1-complete` | llama.cpp Metal, `load_adapter`, Gemma fetch |
| 2 | `rl-sota-phase-2-complete` | DPO trainer, reaction events, preference DB |
| 3 | `rl-sota-phase-3-complete` | KTO, reward model, value head |
| 4 | `rl-sota-phase-4-complete` | GRPO, rollouts, KL penalty |
| 5 | `rl-sota-phase-5-complete` | Eval gate, competitive harness, 4-axis fidelity v2 |
| 6 | `rl-sota-phase-6-complete` | Deterministic E2E test + `human demo rl-closed-loop` |

Branch integrating all phases: `rl-sota-phase-6` (HEAD at tag `rl-sota-phase-6-complete`).

## CI proof (deterministic, every PR)

Build and test with the `rl_sota` preset:

```bash
cmake --preset rl_sota && cmake --build --preset rl_sota -j
export HU_E2E_TMP_ROOT="$(pwd)/build-rl-sota/tests/_tmp"
./build-rl-sota/human_tests
```

**E2E closed-loop suite** (toy HUML GPT, ≤30s, proves wiring):

```bash
./build-rl-sota/human_tests --suite=E2E-closed-loop
```

Expected: **4/4 pass** — chat → reactions → DPO pairs → train → adapter swap → re-chat differs; adapter SHA deterministic.

**Full suite** (May 2026): **10321+ pass**, 0 failures, 2 wasm skips.

**v1 fidelity gate** (byte-stable):

```bash
./build-rl-sota/human_tests --suite=PersonalModel
bash scripts/check-lora-baseline.sh
```

## Local demo proof (Apple Silicon, not CI)

### Quick wiring check (no Gemma download)

```bash
./build-rl-sota/human demo rl-closed-loop \
  --backend huml \
  --reaction-count 50 \
  --out /tmp/human-rl-proof \
  --require-positive-delta
ls /tmp/human-rl-proof
```

Writes all nine evidence files under `--out` (manifest, training_curves, eval_*, gate_decision, etc.).

### Full live demo (real Gemma + MLX)

```bash
bash scripts/fetch-gemma.sh    # once
bash scripts/demo-rl-loop.sh   # see docs/demos/rl-loop-demo.md
```

Evidence lands in `~/.human/proofs/<YYYY-MM-DD>-<method>-step-<pid>/`.

## Key entry points

| Command | Purpose |
|---------|---------|
| `human ml dpo-train` | Train DPO adapter |
| `human ml kto-train` | KTO training |
| `human ml grpo-train` | GRPO training |
| `human ml lora-ab --require-positive` | Pre-commit gate via eval gate |
| `human eval gate` | Statistical promotion gate |
| `human eval competitive` | Competitive scorecard |
| `human demo rl-closed-loop` | End-to-end demo |

## Plans

- Umbrella: `docs/plans/2026-05-11-full-sota-rl-improvement-loop.md`
- Phase 6: `docs/plans/2026-05-11-rl-loop-phase-6-proof.md`
- Demo runbook: `docs/demos/rl-loop-demo.md`

## Out of scope on this branch

KV compression (`kv_compressor`, DeltaKV, SWAN) and MoLoRA prototypes live on `feat/init-13-kv-compression-foundation` — not part of the RL SOTA phase plan.
