---
title: RL SOTA Closed-Loop Proof
description: Index of Phase 0–6 deliverables, Ship Contract DoD verification, CI evidence, and local reproduction steps.
status: current
date: 2026-05-16
---

# RL SOTA closed-loop proof

This document indexes the **full SOTA RL improvement loop** (Phases 0–6) and how to reproduce it locally.

**Ship Contract status: 14/14 DoD items PASS at `rl-sota-phase-6-complete`.** See [`rl-loop-shipcontract.md`](rl-loop-shipcontract.md) for the per-item PASS/FAIL/PARTIAL verification with file:line evidence.

**Adversarial audit status: every gate fired at every applicable phase boundary, every finding remediated.** See [`adversarial-audit-report.md`](adversarial-audit-report.md) for the full chronological index of `critic` + `aspect-panel` + `sprint-auditor` findings with remediation commits.

## Phase completion tags

| Phase | Tag | Commit | Date | What shipped |
|-------|-----|--------|------|--------------|
| 0 | `rl-sota-phase-0-complete` | — | 2026-05-11 | Honesty fixes, atomic personal-model save |
| 1 | `rl-sota-phase-1-complete` | — | 2026-05-11 | llama.cpp Metal, `load_adapter`, Gemma fetch |
| 2 | `rl-sota-phase-2-complete` | `75a3687a` | 2026-05-12 | DPO trainer (HUML in-process + MLX subprocess), reaction events (iMessage tapback poll + Slack `reactions.added/removed`), preference DB |
| 3 | `rl-sota-phase-3-complete` | `dfab9937` | 2026-05-12 | KTO (HUML gradient-checked + MLX subprocess), reward model = backbone + linear value head + Bradley-Terry SGD, `hu_reward_model_load` round-trip |
| 4 | `rl-sota-phase-4-complete` | `10236977` | 2026-05-12 | GRPO + multi-rollout vtable, KL divergence module (Schulman k3), reward source vtable, `human ml grpo-train` CLI |
| 5 | `rl-sota-phase-5-complete` | `a16cb489` | 2026-05-16 | 4-axis fidelity scorer v2, bootstrap-CI helper, `hu_eval_gate_t`, external judge vtable (Apple FM + Gemini Nano), competitive harness, production wiring of 3 Phase-2 deferrals |
| 6 | `rl-sota-phase-6-complete` | `3a17a528` | 2026-05-16 | Deterministic E2E (`test_e2e_closed_loop_*` 4 tests), `human demo rl-closed-loop`, proof-directory contract |

Branch integrating all phases: `rl-sota-phase-6` (HEAD at tag `rl-sota-phase-6-complete`). Close-out branch: `rl-sota-program-close`.

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

**Full suite at `rl-sota-phase-6-complete`:** **10330/10332 PASS, 2 SKIP, 0 FAIL, 0 ASan, 0 UBSan, 0 leaks.**

### Test skips (documented, expected)

| Skip | Why | What it would prove |
|------|-----|--------------------|
| `wasm WASI syscall tests` | Build with `wasm32-wasi` target to run; not part of `rl_sota` preset | wasm runtime parity |
| `HU_HAVE_MLX_LM not defined — MLX RM test skipped` | `mlx-lm-lora` Python venv not installed in this preset run; install with `pip install mlx-lm-lora` and re-run for MLX RM subprocess coverage | MLX reward-model subprocess training end-to-end |

Both skips are intentional gating, not failures. The `rl_sota` preset on Apple Silicon with `mlx-lm-lora` installed exercises the MLX paths; CI runs the HUML paths which are gradient-checked and cross-platform.

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

- Umbrella: [`docs/plans/2026-05-11-full-sota-rl-improvement-loop.md`](../plans/2026-05-11-full-sota-rl-improvement-loop.md)
- Per-phase plans:
  - [Phase 0 — honesty pass](../plans/2026-05-11-rl-loop-phase-0-honesty.md)
  - [Phase 1 — llama.cpp Metal](../plans/2026-05-11-rl-loop-phase-1-llamacpp.md)
  - [Phase 2 — real DPO + reactions](../plans/2026-05-11-rl-loop-phase-2-dpo-reactions.md)
  - [Phase 3 — KTO + reward model](../plans/2026-05-11-rl-loop-phase-3-kto-rm.md)
  - [Phase 4 — GRPO + multi-rollout](../plans/2026-05-11-rl-loop-phase-4-grpo.md)
  - [Phase 5 — eval gate + competitive harness](../plans/2026-05-11-rl-loop-phase-5-eval-competitive.md)
  - [Phase 6 — E2E proof + demo](../plans/2026-05-11-rl-loop-phase-6-proof.md)
- Ship Contract DoD verification: [`docs/proof/rl-loop-shipcontract.md`](rl-loop-shipcontract.md)
- Adversarial audit report: [`docs/proof/adversarial-audit-report.md`](adversarial-audit-report.md)
- Demo runbook: [`docs/demos/rl-loop-demo.md`](../demos/rl-loop-demo.md)
- Master Track D row: [`docs/plans/2026-05-10-master-follow-through-program.md#phase-d3--rl-sota-closed-loop-track-d-phase-2-`](../plans/2026-05-10-master-follow-through-program.md)

## Final close-out verdict

| Item | Status |
|------|--------|
| 7 phase tags exist + all green | ✅ `rl-sota-phase-0-complete` … `rl-sota-phase-6-complete` |
| Full rl_sota suite | ✅ 10330/10332 PASS, 2 SKIP, 0 ASan, 0 UBSan, 0 leaks |
| Ship Contract DoD (14 items) | ✅ 14/14 PASS — see [`rl-loop-shipcontract.md`](rl-loop-shipcontract.md) |
| Adversarial audit gates | ✅ Every gate fired, every finding remediated — see [`adversarial-audit-report.md`](adversarial-audit-report.md) |
| Umbrella status table | ✅ Up to date (per-phase rows with auditor verdicts + remediations) |
| Master Track D row | ✅ Phase D3 subsection added with sub-phase table |

**The full SOTA RL improvement loop is shipped and proven locally end-to-end at `rl-sota-phase-6-complete`.**

## Out of scope on this branch

KV compression (`kv_compressor`, DeltaKV, SWAN) and MoLoRA prototypes live on `feat/init-13-kv-compression-foundation` — not part of the RL SOTA phase plan.
