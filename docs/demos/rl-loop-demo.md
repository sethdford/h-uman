---
title: "RL Closed-Loop Demo Runbook"
created: 2026-05-12
status: living
audience: developers + reviewers
phase: 6
linked_spec: docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md
linked_plan: docs/plans/2026-05-11-rl-loop-phase-6-proof.md
reproducibility: local Apple Silicon only (no CI)
---

# RL Closed-Loop Demo Runbook

This runbook walks through running the Phase 6 demo on Apple Silicon and reading the resulting `~/.human/proofs/` evidence directory.

For the deterministic CI test, see `tests/test_e2e_rl_loop.c`.

## 1. Prerequisites

- macOS on Apple Silicon (`arm64`)
- `cmake --preset rl_sota && cmake --build --preset rl_sota -j`
- Optional GGUF models for MLX backend (`scripts/fetch-gemma.sh`, `scripts/fetch-qwen-rm.sh`)
- `pip install mlx-lm-lora` when using `--backend mlx`

## 2. One-command setup

```bash
cmake --preset rl_sota && cmake --build --preset rl_sota -j
chmod +x scripts/demo-rl-loop.sh
```

## 3. Running the demo

```bash
DRY_RUN=1 bash scripts/demo-rl-loop.sh   # prereq check only
bash scripts/demo-rl-loop.sh
```

Or directly:

```bash
./build-rl-sota/human demo rl-closed-loop \
  --backend huml \
  --persona demo_persona_e2e \
  --reaction-count 50 \
  --out tests/_tmp/proofs/demo-smoke \
  --require-positive-delta
```

## 4. Reading the evidence dir

Nine files per spec section 8:

- `manifest.json`
- `training_curves.json`
- `eval_before.json`, `eval_after.json`, `eval_delta.json`
- `delta_responses.md`
- `gate_decision.json`
- `adversarial_review.md`
- `reproduce.sh`

## 5. Troubleshooting

| Symptom | Fix |
|---------|-----|
| `build-rl-sota/human missing` | Run the `rl_sota` preset build |
| Exit 3 | Check SQLite + `HU_ENABLE_RL_FULL` in the build |
| Exit 2 | Inspect `eval_delta.json`; lower `--reaction-count` or retry with `--backend huml` for wiring-only smoke |

## 6. Reproducibility recipe

Deterministic wiring proof (CI): `./build-rl-sota/human_tests --suite=E2E-closed-loop`

Live demo (local only): `bash scripts/demo-rl-loop.sh` with real Gemma + Qwen RM when MLX backend is configured.

## 7. FAQ

**Why is the demo not in CI?** Real-model runs are slow, non-deterministic, and Apple-Silicon-only per spec section 6.5.

**What does exit 0 mean?** Persona-fidelity delta met the configured threshold with `--require-positive-delta`.

## 8. Citations

- `docs/plans/2026-05-11-rl-loop-phase-6-proof.md`
- `docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md` section 8
