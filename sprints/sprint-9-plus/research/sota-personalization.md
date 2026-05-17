# SOTA: Personalized LLM Fine-Tuning / Digital Twin Training

**Compiled:** 2026-05-16
**For:** h-uman Sprint 9+ planning (DPO digital-twin work)
**Method:** WebSearch + WebFetch over arXiv, lab blogs (Anthropic, Apple/MLX), HF papers.

---

## Top 5 most relevant papers/posts

### 1. FSPO — Few-Shot Preference Optimization of Synthetic Preferences (Singh et al., Stanford, Feb 2025)
- **arXiv:** [2502.19312](https://arxiv.org/abs/2502.19312)
- **TL;DR:** Reframes reward modeling as a **meta-learning** problem: the model conditions on N preference pairs from a user and learns to infer a personalized reward at inference time. Trained on >1M *synthetic* personalized preferences across 1,500 synthetic users. Adds **user-description rationalization (RAT)** as an auxiliary objective.
- **Headline numbers:** 87% AlpacaEval winrate on synthetic users, **70% winrate on real human users** in open-ended QA.
- **Why it matters:** This is the closest published work to what h-uman is trying to do. Their key insight — that the bottleneck isn't optimizer choice (DPO vs IPO) but **synthetic-preference dataset structure** ("high diversity + coherent, self-consistent structure") — directly explains why our 300-pair mined corrections may have insufficient diversity. They also show meta-learning over many *fake* users transfers to real users with a small few-shot prompt — a path that does not require us to fine-tune per user at all.

### 2. POPI — Personalizing LLMs via Optimized Natural Language Preference Inference (Oct 2025, WWW '26 under review)
- **arXiv:** [2510.17881](https://arxiv.org/abs/2510.17881)
- **TL;DR:** Two-stage architecture: (a) an inference LLM trained with RL to compress heterogeneous user signals (chat history, few preference pairs, persona text) into a short natural-language preference summary; (b) a generator LLM fine-tuned to condition on that summary.
- **Headline numbers:** Shrinks user context from **thousands of tokens to tens** while matching or beating full-context personalization across 4 benchmarks. Summaries transfer zero-shot to frozen black-box LLMs (GPT-4o, Claude).
- **Why it matters:** Direct alternative to our DPO approach: instead of fine-tuning weights on 300 pairs, train an *inference* model that compresses 300 corrections into a system-prompt fragment. Side-steps overfit/break-rate problems entirely; works on cloud frontier models when on-device LoRA is degraded. Aligns with our M3 "frontier-model bridge" plan.

### 3. Persona Vectors (Anthropic, Chen et al., July 2025)
- **arXiv:** [2507.21509](https://arxiv.org/abs/2507.21509) | [Anthropic post](https://www.anthropic.com/research/persona-vectors)
- **TL;DR:** Activation-space directions that encode personality traits, extracted automatically from a natural-language trait description. Three uses: (1) **monitor** training-induced persona drift; (2) **steer at inference** by activation-injection; (3) **preventative steering during training** ("vaccinate" the model against unwanted shift while training on data that would otherwise induce it).
- **Headline numbers:** Preventative steering preserves capability (no MMLU drop) while neutralizing trait shifts that vanilla fine-tuning would induce. Strong correlation between training-data-induced activation shift and post-training behavior — predictable *before* training completes.
- **Why it matters:** Gives us an early-warning system for our DPO break-rate problem: project activations onto a "coherence" or "user-voice" vector during training and stop when it deviates. Also offers a runtime alternative to weight updates — steer activations toward a "user-voice" direction instead.

### 4. Smaug / DPO-Positive (Pal et al., Abacus AI, Feb 2024 — still the canonical fix)
- **arXiv:** [2402.13228](https://arxiv.org/abs/2402.13228)
- **TL;DR:** Identifies the DPO failure mode we observed: when preference pairs have **low edit distance**, the DPO loss can be minimized by *reducing the likelihood of the preferred completion* (as long as the relative log-ratio still improves). Fix is **DPOP**: add a penalty term that explicitly keeps `log π(y_chosen|x)` above `log π_ref(y_chosen|x)`.
- **Headline numbers:** First open-source 80%+ score on HF Open LLM Leaderboard; outperforms vanilla DPO on every low-edit-distance dataset tested.
- **Why it matters:** This is the published diagnosis of our **chosen-reward-goes-negative + pad-token-collapse + 40% break rate** symptom. Our mined corrections are exactly the low-edit-distance regime (often single-clause rewrites). DPOP is a 3-line loss change; an immediate Sprint 9 fix.

### 5. SPRInG — Selective Parametric adaptation + Retrieval-Interpolated Generation (Jan 2026)
- **arXiv:** [2601.09974](https://arxiv.org/abs/2601.09974)
- **TL;DR:** Continual personalization framework. At train time, a likelihood-based **drift detector** decides whether a new interaction triggers a LoRA update or goes to a replay buffer. At inference, output logits interpolate between the LoRA-adapted model and retrieval-augmented context.
- **Why it matters:** Directly addresses our research-question #3 (continual personalization without catastrophic forgetting). The drift-gated update mechanism prevents the death-spiral our 200-iter run hit; the replay buffer addresses the "weeks of new corrections" use case for M2/M3.

---

## Key SOTA numbers worth pinning

| Result | Source | Relevance to us |
|---|---|---|
| **70%** real-user winrate from few-shot meta-learned preferences (vs ~50% baseline) | FSPO | Upper bound for few-shot personalization on a frontier-scale eval |
| **Order-of-magnitude** context reduction (≥1000 tokens → ≤100) with no quality loss | POPI | Validates "compress user into a system prompt" as competitive with weight updates |
| **>80%** HF Leaderboard with DPOP vs DPO breakage on low-edit-distance pairs | Smaug | Concrete delta from one loss fix |
| **0% MMLU regression** with persona-vector preventative steering | Anthropic | Activation-steering avoids the capability tax we paid on iter-60 |
| MLX LoRA on **31B Gemma** fits in M3 Max unified memory, **0.053% trainable params** | MLX community / build guides | Headroom on our current hardware; we're not memory-bound |
| **~3K** training pairs sufficient for ID-LoRA cross-modal personalization | arXiv 2603.10256 | 300 is genuinely small even by 2026 standards — diversity > raw count |

Persona evaluation now favors: **judge-based winrate** (AlpacaEval-style), **HEXACO distributional distance**, **held-out perplexity on user history**, **BERTScore-F (~0.84)** for semantic match. PersonaLens ([2506.09902](https://arxiv.org/abs/2506.09902)) and LongLaMP ([2407.11016](https://arxiv.org/abs/2407.11016)) are the closest published benchmarks for what we'd want a `human eval` to compute.

---

## Concrete techniques for Sprint 9

1. **Swap DPO → DPOP loss** (Smaug, [2402.13228](https://arxiv.org/abs/2402.13228)). One-line change to the mlx-lm-lora fork: add `λ · max(0, log π_ref(y_chosen) - log π(y_chosen))` to the loss. Directly targets the chosen-r-going-negative failure mode. Estimated effort: 1 day. Estimated impact: eliminates pad-token collapse class of failure.

2. **Add a `human eval persona-judge` rubric** modeled on FSPO / PersonaLens. Replace gameable lexical fidelity with a held-out AlpacaEval-style judge winrate on 50 in-character prompts. Cite: [2502.19312](https://arxiv.org/abs/2502.19312), [2506.09902](https://arxiv.org/abs/2506.09902). Removes the "iter-60 was best by lexical, worst by behavior" measurement gap.

3. **Try the POPI inference path before fine-tuning more** ([2510.17881](https://arxiv.org/abs/2510.17881)). Train a small summarizer that compresses our 300 corrections into a ≤100-token user-style block injected into the system prompt. If it matches LoRA-iter-60 fidelity, it removes the entire weight-update burden and works against cloud providers (our M3 daemon fallback case). Pairs naturally with the personal-model summary we already inject (`hu_personal_model_build_prompt`).

4. **Add LoRA-dropout + drift-gated continual update** (LoRA-Dropout + SPRInG, [2601.09974](https://arxiv.org/abs/2601.09974)). For the M3 "weeks of corrections" loop, only update LoRA on interactions whose `log π_base - log π_adapted` exceeds a threshold; everything else goes to a replay buffer. Combined with LoRA dropout this should keep break-rate flat as data accumulates.

5. **Compute a "user-voice" persona vector from our example bank** (Anthropic, [2507.21509](https://arxiv.org/abs/2507.21509)). Use it as a *monitoring* signal during DPO training (early-stop when projection drifts) and optionally as an *inference-time* steering vector. Doesn't require Anthropic's weights — works on any open-weight model we can read activations from (Gemma 4, Llama, Qwen).

---

## What's NOT yet solved (open problems where we can contribute)

- **<300 preference pairs for measurable persona transfer.** FSPO uses meta-learning over 1M synthetic; SPRInG uses streaming; no one has published clean numbers for "fine-tune on exactly N=300 real corrections from one person" with a rigorous behavioral eval. This is our actual operating regime — our smoke-test data on real users is a contribution if we publish it (even as a negative result).
- **Reward-hacking pad-token leakage in DPO at small N.** DPOP fixes the low-edit-distance case but the literature does **not** clearly enumerate the pad-token-collapse mode; our 80% pad leakage at iter 200 is documentable behavior worth a short writeup.
- **On-device personalization that targets the model the user actually chats with.** Every method we found either trains a reference small GPT (us today) or assumes the chat model is local and writable (open-weight, no API). The M3 frontier-bridge — *use* a cloud model but *learn* on-device — is genuinely an open research direction. POPI is the closest published precedent.
- **Behavioral consistency under context drift over weeks.** LongLaMP/PersonaLens use static cold-start splits; no public benchmark evaluates "did the model still sound like the user after 4 weeks of new corrections?" We are well-positioned to publish this longitudinal eval once M2 is real.
- **Compose persona vectors with LoRA adapters.** Anthropic uses steering OR fine-tuning, not both. Layering a user-voice steering vector on top of a small DPO LoRA is unexplored and likely cheap.

---

## Sources

- [FSPO arXiv 2502.19312](https://arxiv.org/abs/2502.19312)
- [POPI arXiv 2510.17881](https://arxiv.org/abs/2510.17881)
- [Anthropic Persona Vectors arXiv 2507.21509](https://arxiv.org/abs/2507.21509) | [Anthropic blog](https://www.anthropic.com/research/persona-vectors)
- [Smaug / DPO-Positive arXiv 2402.13228](https://arxiv.org/abs/2402.13228)
- [SPRInG arXiv 2601.09974](https://arxiv.org/abs/2601.09974)
- [PersonaLens arXiv 2506.09902](https://arxiv.org/abs/2506.09902)
- [LongLaMP arXiv 2407.11016](https://arxiv.org/abs/2407.11016)
- [PersonalLLM ICLR 2025](https://openreview.net/forum?id=2R7498e2Tx)
- [TGDPO arXiv 2506.14574](https://arxiv.org/abs/2506.14574) — token-level reward guidance for DPO
- [C-LoRA arXiv 2502.17920](https://arxiv.org/abs/2502.17920) — continual LoRA with orthogonality constraint
- [JumpLoRA arXiv 2604.16171](https://arxiv.org/abs/2604.16171) — sparse adapters for continual LLM personalization
- [ID-LoRA arXiv 2603.10256](https://arxiv.org/abs/2603.10256) — ~3K-pair identity LoRA precedent
- [Alignment Data Map arXiv 2505.23114](https://arxiv.org/abs/2505.23114) — preference-data selection / diagnosis
