# SOTA: Continual Learning, Online Personalization & Catastrophic-Forgetting Mitigation

**Status:** READY (May 2026)
**Author:** research-analyst agent
**Context:** Sprint 9 input. h-uman's W14 nightly cron (US-7.5) re-trains a LoRA from accumulated correction pairs. We need adapter-update patterns that DON'T overwrite prior learning, plus principled "should we re-train tonight?" gates.

---

## 1. Top 5 Papers (May 2026)

1. **EWC-LoRA / Revisiting Weight Regularization for Low-Rank Continual Learning** (ICLR 2026, arXiv:2602.17559) — Regularizes a *shared* low-rank update via EWC; storage stays constant regardless of task count. **+8.92% over vanilla LoRA**, matches or beats other low-rank CL SOTA. Directly relevant: we have ONE adapter that keeps absorbing turns. ([arXiv](https://arxiv.org/abs/2602.17559))

2. **STABLE: Gated Continual Learning for Large Language Models** (arXiv:2510.16089) — Gating framework for sequential LoRA edits. Each candidate edit is scored against a **stability budget** including **KL divergence between base and adapted models**; edits that exceed budget are rejected. This is the "should we accept tonight's training run?" gate. ([arXiv](https://arxiv.org/html/2510.16089))

3. **SuRe: Surprise-Driven Prioritised Replay for Continual LLM Learning** (arXiv:2511.22367) — Replay buffer that ranks rehearsal candidates by *surprise* (loss/gradient magnitude) instead of uniform sampling. Architecture-agnostic, composes with PEFT/LoRA. Replaces our current "shuffle all corrections" approach. ([arXiv](https://arxiv.org/html/2511.22367v1))

4. **OFS-DPO / COFS-DPO: Online Fast-Slow Chasing DPO** (Online DPO with Fast-Slow Chasing, arXiv:2406.05534) — **Dual-LoRA design**: a fast adapter rapidly absorbs new preferences while a slow adapter consolidates via EMA. COFS-DPO extends with cross-domain LoRA combination. Maps onto our DPO nightly retrain (US-8.5/8.6). ([arXiv](https://arxiv.org/html/2406.05534v1))

5. **LoRA Subtraction for Drift-Resistant Space in Exemplar-Free Continual Learning** (arXiv:2503.18985) — Subtracts the LoRA delta from a "drift-resistant subspace" of the base model, so new training doesn't tread on directions the base already used. Pairs naturally with KL-based drift detection in STABLE. ([arXiv](https://ar5iv.labs.arxiv.org/html/2503.18985))

**Honorable mentions:**
- **EWC Done Right** (arXiv:2603.18596) — Logits Reversal fixes EWC's Fisher gradient-vanishing. ([arXiv](https://arxiv.org/abs/2603.18596))
- **LaLoRA** — Laplace approximation for per-LoRA-parameter uncertainty; cheaper than full Fisher.
- **FOREVER: Forgetting-Curve-Inspired Memory Replay** (Jan 2026) — Ebbinghaus-scheduled rehearsal.
- **Many Preferences, Few Policies** (arXiv:2604.04144) — Compact user-vector retrieval at inference, no per-user fine-tune. ([arXiv](https://arxiv.org/html/2604.04144))

---

## 2. Recommended Sprint 9 Architecture

### The retrain-with-guard loop

```
NIGHTLY @ 03:00 local
  │
  ├── 1. Gate: do we have ≥N new pairs since last retrain?              [SuRe-style surprise]
  │       priority = abs(reward_gap) * recency_decay
  │
  ├── 2. Replay buffer: top-K from corrections + reservoir from history  [SuRe + FOREVER]
  │       new : old ratio = 70 : 30 (paper-supported sweet spot)
  │
  ├── 3. Train: COFS-DPO with dual LoRA
  │       - fast_lora: rank-8, lr=1e-4, 1 epoch on tonight's batch
  │       - slow_lora: EMA of fast (alpha=0.95), promoted on PASS
  │
  ├── 4. Stability gate (STABLE): reject if any of
  │       a. KL(base || fast_lora) > tau_kl              (we already have probe set)
  │       b. eval_winrate < last_slow - 1.0 stderr       (regression)
  │       c. forgetting_score > tau_forget               (eval on held-out OLD pairs)
  │
  ├── 5. Promote or rollback:
  │       PASS  -> slow_lora <- EMA(slow, fast); save as v{N+1}; symlink current
  │       FAIL  -> keep v{N}; archive fast as quarantine/{date}.safetensors
  │                emit telemetry "nightly_retrain_rejected" with reason
  │
  └── 6. Inference: retrieve recent preferences (last 7 days) via vector index;
         inject into prompt. Adapter holds STABLE preferences (older than 7d).
         [Many Preferences, Few Policies pattern]
```

### Why this shape

- **Dual LoRA** prevents one-shot overwrite. Fast = today's batch; slow = compounded EMA. Rollback is `cp slow.v{N}.safetensors current`.
- **STABLE gate** answers "should we even ship tonight's run?" — KL drift + held-out forgetting check + winrate regression are all cheap. We already have `ab_eval_30.py`; reuse it as the regression check.
- **Retrieval layer** absorbs *very recent* preferences (last week) without training. Adapter only absorbs preferences that have *persisted* through the replay buffer. This is the right separation: things you said once go to retrieval; things you say repeatedly bake into the adapter.

---

## 3. Sprint 9 Stories (citation-grounded)

| ID | Story | Citation |
|---|---|---|
| **US-9.1** | Replace uniform replay sampler in `lora-persona` with surprise-priority queue. Score = `|reward_gap| * exp(-age/tau)`. | SuRe, arXiv:2511.22367 |
| **US-9.2** | Add `--ewc-lambda` flag wiring shared-LoRA Fisher penalty into the loss. Land regression test that EWC-LoRA beats vanilla LoRA on the smoke-eval-30 set. | EWC-LoRA, arXiv:2602.17559 |
| **US-9.3** | Implement STABLE-style nightly stability gate: KL-on-probe-set + winrate-regression + forgetting-on-old-pairs. Three thresholds in `config.yaml`. Cron must reject and log when ANY trips. | STABLE, arXiv:2510.16089 |
| **US-9.4** | Dual-LoRA training: fast (per-night) + slow (EMA). On PASS, slow := EMA(slow, fast, alpha=0.95). On FAIL, fast quarantined. | OFS-DPO/COFS-DPO, arXiv:2406.05534 |
| **US-9.5** | Adapter versioning: `~/.human/adapters/v{N}.safetensors` + `current` symlink. `human adapter rollback` CLI. Keep last 7 versions; GC older. | Dynatrace agent versioning (2026) + general LLMOps practice |
| **US-9.6** | Retrieval layer for recent preferences (last 7 days). Embedding index over correction pairs; inject top-K into prompt at inference. Adapter only re-trains on pairs older than 7d. | Many Preferences, Few Policies, arXiv:2604.04144; ExpRAG family |
| **US-9.7** | Telemetry: `nightly_retrain_accepted/rejected` counter, `kl_drift_bits`, `forgetting_score`, `winrate_delta`. Add to `/cache-report`-style dashboard. | Production LLMOps (Dynatrace 2026 blog) |
| **US-9.8** *(stretch)* | LoRA-Subtraction drift-resistant subspace: project fast-LoRA gradient onto orthogonal complement of base's high-energy directions before applying. | LoRA Subtraction, arXiv:2503.18985 |

### Concrete acceptance thresholds (from papers, tuneable)

- `tau_kl = 0.5 bits` (STABLE's typical operating range for small adapters)
- `tau_forget = 0.05` regression on held-out OLD pairs (5pp drop in old-pair winrate = reject)
- `tau_winrate = 1.0 * stderr` (one standard error below previous slow adapter = reject)
- Replay ratio: `new:old = 70:30` for the per-night batch
- Slow-EMA `alpha = 0.95` (OFS-DPO default)

---

## 4. Two Things to NOT Do

- **Don't re-train from scratch every night.** That's what the W14 cron currently does. With EMA + replay it's wasteful AND erases prior preferences. Use the dual-LoRA delta-from-slow pattern.
- **Don't trust "loss went down" as a promotion signal.** Loss on tonight's pairs always goes down — that's the failure mode. Promotion needs winrate-on-held-out + forgetting-on-old + KL-drift. STABLE makes this explicit; we should too.

---

## 5. Risks / Open Questions

- **EWC Fisher cost.** Computing a true Fisher diagonal is expensive. LaLoRA (Laplace over adapter only) is the cheap proxy — confirm it's good enough on our model size before committing to full EWC-LoRA.
- **Retrieval staleness vs. adapter freshness.** "Last 7 days" is a guess. Sprint 9 should add a `/eval` scenario that varies the cutoff and measures recall-of-recent-preference.
- **Quarantine signal.** When the stability gate rejects 3 nights in a row, something upstream is wrong (bad correction data, model drift). Need an alert, not just a log line.

---

## Citations

- [Revisiting Weight Regularization for Low-Rank Continual Learning (EWC-LoRA), ICLR 2026 — arXiv:2602.17559](https://arxiv.org/abs/2602.17559)
- [STABLE: Gated Continual Learning for Large Language Models — arXiv:2510.16089](https://arxiv.org/html/2510.16089)
- [SuRe: Surprise-Driven Prioritised Replay for Continual LLM Learning — arXiv:2511.22367](https://arxiv.org/html/2511.22367v1)
- [Online DPO with Fast-Slow Chasing (OFS-DPO / COFS-DPO) — arXiv:2406.05534](https://arxiv.org/html/2406.05534v1)
- [LoRA Subtraction for Drift-Resistant Space — arXiv:2503.18985](https://ar5iv.labs.arxiv.org/html/2503.18985)
- [Elastic Weight Consolidation Done Right — arXiv:2603.18596](https://arxiv.org/abs/2603.18596)
- [Many Preferences, Few Policies — arXiv:2604.04144](https://arxiv.org/html/2604.04144)
- [Understanding LoRA as Knowledge Memory — arXiv:2603.01097](https://arxiv.org/abs/2603.01097)
- [AI Model Versioning and A/B testing for LLM services (Dynatrace, 2026)](https://www.dynatrace.com/news/blog/the-rise-of-agentic-ai-part-6-introducing-ai-model-versioning-and-a-b-testing-for-smarter-llm-services/)
- [Combining replay and LoRA for continual learning in NLU (Computer Speech & Language)](https://www.sciencedirect.com/science/article/abs/pii/S0885230824001207)

---

`RESULT_research=READY`
