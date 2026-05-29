# Sprint 60 — SOTA + Local E2E Proof (evidence)

The TikTok-style "feels-alive" learning loop, **built this sprint and proven to
work locally end-to-end against real artifacts** — plus the SOTA persona-fidelity
lift it feeds, **reproduced live on-device**.

## The end-to-end loop (what was built + how each leg is proven)

```
implicit signal (reply / tapback / edit / reply-latency)
  → [US-102] mining → DPO preference pairs
  → [US-101] Bradley-Terry reward model (analytical gradients, gradient-checked)
  → DPO/LoRA training on gemma-4-31b-it-4bit
  → +27pp persona fidelity (hot-swapped adapter)
  → next turn sounds more like the user
[US-103/104] proactive contextual bandit learns WHEN to reach out
```

## Proof 1 — loop learns locally on the REAL binary

`scripts/prove-loop-local-e2e.sh` (reproducible; no cloud, no live server):

| Leg | Real-binary command | Observed | Verdict |
|---|---|---|---|
| Reward model learns | `human ml rm-train` (Bradley-Terry, HUML) | loss **0.694 → 0.461** over 1000 iters; checkpoint persisted | PASS |
| Persona fidelity baseline | `human ml lora-baseline --persona seth` (CPU) | mean **0.691** / 145 examples | PASS |
| Loop closure | real test binary suites | huml 10/10 · dpo_collector 4/4 · contextual_bandit 9/9 · e2e_learning_loop 4/4 | PASS |

## Proof 2 — SOTA fidelity lift, reproduced LIVE on this machine

`scripts/eval_fidelity_nightly.py` on Apple M4 Max, model + adapter local
(no download). Result: `sprints/sprint-60/results/fidelity-live-verdict-20260529.json`.

| Run | Model | Pre (base) | Post (LoRA) | Δ mean | 95% CI | Gate |
|---|---|---|---|---|---|---|
| **Live 2026-05-29** | gemma-4-31b-it-4bit + seth-lora-v4-repair | **0.581** | **0.850** | **+0.269** | [0.166, 0.357] | **PASS** (stat+practical) |
| Prior 2026-05-25 | same | 0.586 | 0.856 | +0.270 | [0.156, 0.372] | PASS |

29 held-out Seth-style prompts. Pre/post means independently reproduce within
noise → the on-device LoRA personalization is **real, significant, and stable**.

## Honest scope

- **Proven locally e2e:** reward-model learning, persona-fidelity scoring +27pp
  lift (live), bandit convergence, and the full data-path loop closure.
- **Built this sprint (the spine):** US-101 reward model, US-102 mining, US-103
  bandit, US-104 signal→bandit, US-105 nightly mining-wired retrain, US-106
  hot-swap, US-107 e2e proof. Audit: `RESULT_sprint-auditor=PASS`.
- **Needs the live MLX server (not in this proof):** the in-session hot-swap of
  a freshly-trained adapter into a running daemon. The swap CALL + graceful
  fallback are tested; a live MLX server is required to observe the swapped
  weights serve a turn. The fidelity eval above loads the adapter directly,
  proving the trained weights produce the lift.
- **NOT built (groomed backlog #4–#9):** reflection/consolidation, late-interaction
  RAG reranking, predictive world-model, catastrophic-forgetting defenses,
  mixture-of-LoRAs, process reward model.

## Reproduce

```sh
# loop learns locally (seconds, CPU):
scripts/prove-loop-local-e2e.sh

# SOTA fidelity lift live (~9 min, Apple Silicon, 31B model + adapter must be local):
python3 scripts/eval_fidelity_nightly.py \
  --adapter-path ~/.human/training-data/adapters/seth-lora-v4-repair-20260525-071921 \
  --model-id mlx-community/gemma-4-31b-it-4bit \
  --held-out-fixture docs/plans/2026-05-26-sprint-56-gemma-as-seth/data/heldout-prompts.jsonl \
  --output-json /tmp/fidelity-verdict.json
```
