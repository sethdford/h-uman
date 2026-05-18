# Sprint 9 SOTA Roadmap — Grounded in May 2026 Research

**Synthesis of 7 parallel research agents** that surveyed arXiv, Anthropic / OpenAI / DeepMind / Apple / Meta / Stanford / Microsoft papers May 2026.

**Goal:** Take h-uman's digital-twin loop from Sprint 7-8's "infrastructure works, metric is gameable, +0.019 delta with 40% break rate" → SOTA-defensible "real held-out next-utterance prediction lift on the user's own data."

## Research convergence — findings that surfaced from MULTIPLE independent agents

The strongest signal is **convergent finding** across research lanes:

1. **DPOP / Smaug (arXiv 2402.13228)** surfaced from BOTH the DPO-improvements agent AND the personalization agent. Both independently identified it as the named fix for our exact failure mode (DCR — Degraded Chosen Responses). **Strongest single recommendation.**

2. **Real held-out next-utterance prediction** surfaced from BOTH the benchmarks agent (YNTP-100, TwinVoice, Twin-2K-500) AND the reward-eval agent (PPL floor stage 1). Both said: the synthetic lexical fingerprint cannot be saved; replace it with held-out PPL on real continuations. **Strongest single eval recommendation.**

3. **Pad-token masking + length normalization** surfaced from BOTH the DPO and on-device research. Mandatory regardless of which loss variant we choose. **Cheapest+highest-impact engineering fix.**

4. **Anthropic persona vectors / early-stopping signal** surfaced from BOTH the personalization agent (persona vector projection) AND the DPO agent (chosen_r ≥ 0 canary) — converges with our own prior-iteration finding of `train_chosen_r` plateau-break. **The signal is in the training output; we just need to read it.**

## Top techniques from the literature (by paper, with citations)

| # | Technique | Paper | arXiv ID | Lane(s) | Why it matters for h-uman |
|---|---|---|---|---|---|
| 1 | **DPOP** (positive-clipping DPO) | Smaug — Pal et al. | [2402.13228](https://arxiv.org/abs/2402.13228) | DPO, Personalization | Names our failure mode (DCR); fix is single penalty term that anchors chosen log-prob. Designed for low-edit-distance pairs (our case). |
| 2 | **ORPO** (single-stage SFT NLL + odds-ratio) | Hong et al. ICLR 2025 | – | DPO | No reference model; NLL term directly anchors chosen completion. Sprint 7 US-7.10 already shipped vtable; now wire `train_step`. |
| 3 | **Real held-out next-utterance LL** | TwinVoice (ACL 2026 Findings) | [2510.25536](https://arxiv.org/abs/2510.25536) | Benchmarks | Ungameable by style fingerprints. Real-person digital-twin benchmark. The replacement for our synthetic fingerprint. |
| 4 | **YNTP-100** (real-person Next-Token-Prediction) | – | [2510.14398](https://arxiv.org/abs/2510.14398) | Benchmarks | 100-user real-data benchmark. Gives us a number to beat. |
| 5 | **Twin-2K-500** (forced-choice held-out) | – | [2505.17479](https://arxiv.org/abs/2505.17479) | Benchmarks | Secondary metric — does the model make the same DECISIONS as the user? |
| 6 | **DoRA** | NVIDIA Liu et al. | – | MLX on-device | 93% of LoRA→full-FT gap, zero inference overhead. Drop-in for current LoRA. |
| 7 | **Anthropic Persona Vectors** | Anthropic | [2507.21509](https://arxiv.org/abs/2507.21509) | Personalization, Eval | Project activations onto user-voice direction during training. Early-warning signal for break-rate spiral. |
| 8 | **POPI** (system-prompt summarizer) | – | [2510.17881](https://arxiv.org/abs/2510.17881) | Personalization | Matches LoRA personalization with ≤100 system-prompt tokens. Works on cloud frontier models — solves M3 bridge problem differently. Plan B if LoRA underperforms. |
| 9 | **ThinkPRM** (verbalized verifier PRM) | – | [2504.16828](https://arxiv.org/abs/2504.16828) | Reward eval | Beats discriminative PRMs with 1% of labels. Practical persona verifier for h-uman scale. |
| 10 | **OFS-DPO / COFS-DPO** (dual fast/slow LoRA + EMA) | – | – | Continual | Solves W14 cron forgetting + drift. Nightly `fast` adapter trains the batch; `slow` promoted by EMA only if gates pass. |
| 11 | **EWC-LoRA** | – | ICLR 2026 | Continual | +8.92% over vanilla LoRA; the canonical "don't forget yesterday's preferences" technique. |
| 12 | **STABLE** (KL-divergence drift budget) | – | – | Continual | Per-adapter drift cap; refuses promotion if KL exceeds threshold. |
| 13 | **SuRe** (surprise-prioritized replay 70:30) | – | – | Continual | Replaces uniform sampling for nightly retrain. |
| 14 | **Gemma-4 MTP drafter** | Google | (2026-05-05 release) | MLX on-device | ~3× decode speedup via speculative decode. Production wins. |
| 15 | **RewardHackWatch** (runtime detector) | METR audit | – | Reward eval | 89.7% F1 detecting reward hacking at runtime. Closes the loop on our problem. |

## Sprint 9 backlog — 10 stories, 4 waves, citation-grounded

### Wave 0 (P0; cheap; parallel; unblock the rest)

| # | Story | Source | Effort | Risk |
|---|---|---|---|---|
| **US-9.1** | Pad-token masking + length normalization in DPO loss | DPO + MLX | S | LOW |
| **US-9.2** | DoRA flag in finetune-gemma.py (drop-in for LoRA) | MLX | S | LOW |
| **US-9.3** | Persona-vector projection — early-stopping signal | Personalization (Anthropic 2507.21509) | M | MEDIUM |

### Wave 1 (P0; depends on Wave 0; the real twin work)

| # | Story | Source | Effort | Risk |
|---|---|---|---|---|
| **US-9.4** | **DPOP** loss head (Smaug positive-clipping) | DPO + Personalization (Smaug 2402.13228) | M | MEDIUM |
| **US-9.5** | **ORPO** wire-up — finish US-7.10 train_step using ORPO loss | DPO (US-7.10 partially done) | M | MEDIUM |
| **US-9.6** | **Held-out next-utterance LL evaluator** (YNTP-100-style protocol on Seth's chat.db) | Benchmarks (2510.14398 + 2510.25536) | L | HIGH |

### Wave 2 (P1; depends on Wave 1; production-grade gate + eval)

| # | Story | Source | Effort | Risk |
|---|---|---|---|---|
| **US-9.7** | **4-stage Pareto gate**: PPL floor → coherence judge → persona PRM → ensemble | Reward eval | L | HIGH |
| **US-9.8** | **Dual fast/slow LoRA + EMA promotion** for W14 nightly cron | Continual (OFS-DPO) | L | MEDIUM |

### Wave 3 (P2; stretch; only if Waves 0-2 land clean)

| # | Story | Source | Effort | Risk |
|---|---|---|---|---|
| **US-9.9** | **POPI summarizer baseline** — system-prompt personalization on cloud provider | Personalization (2510.17881) | M | LOW |
| **US-9.10** | **Forced-choice held-out (Twin-2K-500)** as secondary metric | Benchmarks (2505.17479) | M | MEDIUM |

## Out of scope (defer to Sprint 10+)

- MoLoRA Phase 2 (learned MLP router) — only after single-adapter loop works
- ThinkPRM training (5-10K persona labels) — needs Seth's manual labeling effort
- Apple Intelligence parity — wait for first-party DPO in mlx-lm
- M4/M6 (Ship to Users) — premature until SOTA gate passes

## Success criteria

Sprint 9 closes successfully when:

1. **DPOP-trained adapter achieves Pareto-score improvement** over Sprint 8 best (iter-60 DPO @ +0.0114)
2. **Held-out next-utterance log-likelihood** on Seth's chat.db improves measurably vs base+system-prompt baseline (this is the SOTA-defensible claim)
3. **Pad-token leakage rate** drops below 10% (was 40-80% in Sprint 8)
4. **Pareto gate** promotes the best adapter automatically — operator confirmation, not manual judgment
5. **All 10 stories pass verifier + critic + aspect-panel**
6. **Sprint-auditor signs off independently** — same adversarial review as Sprint 7

## Honest scope warning

10 stories with research grounding is realistic; **but** the published baselines (e.g. FSPO's 70% winrate) used 1M synthetic users and frontier-scale compute. Our 137 organic preference pairs + 300-ish synthetic + Gemma-4-E2B is much smaller. **Sprint 9 should produce a defensible delta number with honest methodology**, not match published SOTA numbers in absolute terms.

The publishable claim becomes:

> "On real user data (n=N), DPOP+DoRA+early-stopping on Gemma-4-E2B improves held-out next-utterance log-likelihood by X% vs. base+system-prompt persona baseline, with 0 pad-token leakage in 30 held-out prompts."

That is **falsifiable, not gameable**, and would be a real Sprint 9 deliverable.

## Files

- 7 individual research reports in `sprints/sprint-9-plus/research/sota-*.md`
- This synthesis: `sprints/sprint-9-plus/sota-roadmap.md`

## Next step

Dispatch `/scrum` on this roadmap. The user explicitly asked for "grounded adversarial review and audit and evidence based proof" — `/scrum` provides verifier + critic + aspect-panel per story + sprint-auditor at close, which IS the adversarial-grounding ceremony.
