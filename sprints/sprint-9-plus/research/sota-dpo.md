# SOTA Preference Optimization for Small-Data Persona Transfer (May 2026)

**Context:** h-uman Sprint 8 attempted DPO on Gemma-4-E2B with 331 preference pairs (LoRA adapter). After ~200 iterations: `chosen_reward` flipped sign (+20 → −8.9), `rejected_reward` dropped further, and ~80% of generated outputs leaked `<pad>` tokens. This is the textbook **Degraded Chosen Responses (DCR)** failure mode of vanilla DPO, compounded by length-bias exploitation. This document surveys what to use instead.

---

## 1. The failure mode, named

The pattern we observed has a name in the literature: **DCR — Degraded Chosen Responses**. DPO's pairwise ranking loss only constrains the *margin* between chosen and rejected; both can collapse together as long as rejected falls faster. Pushing down a rejected sequence is much easier than pushing up a specific chosen one — the model only needs to redistribute mass to *any* alternative token. The `<pad>` token leakage is a classic symptom: the model finds that emitting EOS/pad early is the cheapest way to suppress the rejected continuation, and length normalization is absent so short pad-spam wins on margin.

Independently named in the 2025 literature as the **"3D-Properties"**: Drastic drop in rejected likelihood, Degradation into response suppression, Dispersion onto unseen outputs (Yan et al., ICLR 2025).

---

## 2. Comparison table

| Method | Paper / Year | Main innovation | Reward-hacking susceptibility | Small-data fit |
|---|---|---|---|---|
| **DPO** | Rafailov et al., NeurIPS 2023 (arXiv 2305.18290) | Implicit reward via log-ratio against frozen reference | **High** — DCR, length bias, ref-model drift | Poor — overfits fast |
| **IPO** | Azar et al., AISTATS 2024 (arXiv 2310.12036) | Bounded identity-mapped loss (regression target, no sigmoid) | **Low–Medium** — bounded objective prevents unbounded reward growth; sensitive to KL temp | **Good** — designed for the deterministic-preference regime small data exhibits |
| **KTO** | Ethayarajh et al., ICML 2024 (arXiv 2402.01306) | Prospect-theoretic loss on unpaired binary signal | **Medium** — single-sample, no margin gaming | Good when you have thumbs-up/down rather than pairs |
| **ORPO** | Hong et al., EMNLP 2024 (arXiv 2403.07691) | Odds-ratio penalty fused with SFT NLL; no reference model | **Medium-Low** — SFT anchor on chosen prevents DCR by construction | **Excellent** — single-stage, no ref model, NLL anchor keeps chosen up |
| **SimPO** | Meng et al., NeurIPS 2024 (arXiv 2405.14734) | Length-normalized avg log-prob reward, no ref model, target margin γ | **Medium** — beats DPO on AlpacaEval2 by +6.4, but no ref regularization can cause drastic drift from base | OK at scale; **risky** at 300 pairs without ref anchor |
| **DPOP / Smaug** | Pal et al., 2024 (arXiv 2402.13228) | Add clipping term that *positively* incentivizes chosen log-prob | **Low** — explicit DCR fix | **Excellent** — designed for low-edit-distance pairs (persona transfer is low-edit-distance) |
| **Cal-DPO** | Xiao et al., 2024 (arXiv 2412.14516) | Calibrate absolute reward magnitudes, not just margin | **Low** — rewards on chosen stay positive empirically | Good |
| **R-DPO / RPO** | Liu et al., ICLR 2025 (arXiv 2405.16436 "SFT loss is implicitly adversarial regularizer") | Fuse DPO loss with SFT NLL on chosen | **Low** — NLL anchor prevents the drop | **Excellent**, simple to implement |
| **LN-DPO** | Practical-PO analysis, Saeidi et al. 2024 (arXiv 2407.15229) | Length normalization on top of DPO (keep ref model) | **Low-Medium** — kills length exploitation, keeps ref regularization | **Good** — more stable than SimPO across hparams |
| **BPO (Balanced)** | Sun et al., 2025 (arXiv 2506.03557) | "Balanced reward margin + gap adaptor" — one-line patch that resolves DCR | **Low** — explicitly designed against DCR | **Promising**, drop-in for DPO frameworks |
| **GRPO** | DeepSeek (DeepSeekMath, R1) 2024–25 | Group-relative advantage; PPO-style on-policy | **Medium** — vulnerable in multi-objective (MO-GRPO arXiv 2509.22047) | Not a fit — needs online rollouts + verifiable rewards |
| **XPO (online DPO)** | Xie et al., ICLR 2025 (arXiv 2405.21046) | DPO + exploration bonus, on-policy | **Low** — provably sample-efficient | Not a fit — needs online preference oracle |

---

## 3. Top 5 papers (full citations)

1. **Rafailov, R., Sharma, A., Mitchell, E., Ermon, S., Manning, C. D., & Finn, C. (2023).** *Direct Preference Optimization: Your Language Model is Secretly a Reward Model.* NeurIPS 2023. https://arxiv.org/abs/2305.18290
2. **Pal, A., Karkhanis, D., Roberts, M., Dooley, S., Sundararajan, A., & Naidu, S. (2024).** *Smaug: Fixing Failure Modes of Preference Optimisation with DPO-Positive.* https://arxiv.org/abs/2402.13228 — **most directly relevant to our DCR failure**.
3. **Meng, Y., Xia, M., & Chen, D. (2024).** *SimPO: Simple Preference Optimization with a Reference-Free Reward.* NeurIPS 2024. https://arxiv.org/abs/2405.14734 — length normalization rationale.
4. **Hong, J., Lee, N., & Thorne, J. (2024).** *ORPO: Monolithic Preference Optimization without Reference Model.* EMNLP 2024. https://arxiv.org/abs/2403.07691 — single-stage SFT+pref, ideal for small data with LoRA.
5. **Sun, H., et al. (2025).** *BPO: Revisiting Preference Modeling in Direct Preference Optimization.* https://arxiv.org/abs/2506.03557 — one-line fix that resolves DCR directly.

Supplementary: Azar et al. *A General Theoretical Paradigm to Understand Learning from Human Preferences* (IPO, arXiv 2310.12036), and Yan et al.'s 3D-Properties analysis (ICLR 2025) for the diagnostic framework.

---

## 4. Recommendation for h-uman (~300 pairs, persona transfer, LoRA on Gemma-4-E2B)

**Use ORPO as the primary, with DPOP and length normalization as the fallback.**

Rationale:
- **ORPO** combines SFT NLL on chosen + odds-ratio penalty on rejected in a single objective. The NLL term *directly prevents* the DCR collapse we hit — chosen log-prob is anchored, not just relatively constrained. No reference model means lower memory (matters for E2B + LoRA on Apple Silicon). Empirically validated at 125M–7B on UltraFeedback; 300 pairs is small but the SFT anchor makes it forgiving.
- **DPOP fallback**: if we keep a DPO codepath, add the DPOP positive-clipping term. Persona pairs are **low-edit-distance** (same intent, different tone) — exactly the regime DPOP was designed for.
- **Length normalization** (LN-DPO style) is mandatory either way. Our `<pad>` leakage is a length-exploitation symptom; un-normalized DPO assigns higher reward to whatever sequence has fewer tokens, and pad-spam is the local minimum.
- **Avoid SimPO at this scale.** It beats DPO at 60k+ pairs, but the absent reference regularization is dangerous at 300 pairs — the model can wander far from base with no anchor. SimPO's authors themselves note this risk.

**Hyperparameters** (synthesized from Anyscale, BramVanroy HF sweeps, ORPO paper, and the "3D-Properties" paper):
- `beta`: ORPO uses `lambda ∈ [0.05, 0.2]` for the OR term; start at **0.1**. For DPO/DPOP, **0.1–0.2** for ~300 pairs (lower beta = more aggressive = more DCR risk; 0.01 is too aggressive at this scale).
- `lr`: **5e-6 to 1e-5** for LoRA adapter (lower than SFT typical 2e-4 — preference learning is finicky).
- `epochs`: **1–3** (200 iters at ~331 pairs ≈ 1 epoch with bs=2; **we were already past the sweet spot**).
- `max_length`: cap completion length at p95 of training data; pad to right; **mask pad tokens from loss** (this likely fixes the leakage by itself).
- `target margin γ` (SimPO/BPO): **0.5–1.5** if used.

**Early-stopping signals** (the question we should have answered in Sprint 7):
1. **Chosen reward must stay ≥0 and trend up.** If `chosen_r` goes negative or strictly decreases for 3 consecutive eval steps → stop. This single rule would have caught our Sprint-8 failure at iter ~50.
2. **Margin (chosen − rejected) alone is misleading** — it can grow while both collapse.
3. **Validation generation samples every N steps.** Eyeball 5 generations; if pad/EOS leakage appears, stop. Cheap and decisive.
4. **KL(policy ‖ ref) on a held-out prompt set.** If KL > 10 nats, you've drifted out of the base distribution.
5. **Hold out 10% of pairs as a preference-accuracy val set.** Stop when val accuracy plateaus (not train accuracy).

---

## 5. Recommended Sprint 9 stories (citation-grounded)

- **US-9.1 — Switch DPO trainer to ORPO** (single-stage, no ref model). Cite Hong et al. 2024 (arXiv 2403.07691). AC: trainer produces a LoRA adapter on 331 pairs without `chosen_r` going negative.
- **US-9.2 — Add DPOP positive-clipping term as an alternative path.** Cite Pal et al. 2024 (arXiv 2402.13228). AC: with `--variant dpop`, chosen log-prob is non-decreasing over training.
- **US-9.3 — Mandatory length normalization + pad masking.** Cite Meng et al. 2024 (SimPO §3, arXiv 2405.14734) and the SimPO GitHub issue on LN-DPO. AC: pad-token rate in generated outputs < 1% on held-out prompts.
- **US-9.4 — Early-stopping callback on `chosen_r ≥ 0` and val-pref-accuracy plateau.** Cite Yan et al. 3D-Properties (ICLR 2025) for the chosen-reward-as-stop-signal rationale. AC: training auto-halts on the canary; no human babysitting required.
- **US-9.5 — Eval harness reporting absolute chosen/rejected rewards over time, KL(π‖ref), and 5 sampled generations per eval step.** AC: existing `--summary-json` extended with these fields; A/B harness in `scripts/ab_eval_30.py` consumes them.
- **US-9.6 — Hyperparameter sweep: beta ∈ {0.05, 0.1, 0.2}, lr ∈ {5e-6, 1e-5}, epochs ∈ {1, 2, 3}.** Cite β-DPO (arXiv 2407.08639) for adaptive-beta as a stretch goal.
- **US-9.7 (stretch) — BPO one-line patch as a third variant.** Cite Sun et al. 2025 (arXiv 2506.03557). AC: trainer supports `--variant bpo`.

---

## 6. What we are NOT doing and why

- **Not GRPO / online DPO / XPO** — these require an online preference oracle (verifiable reward function or fresh human/judge labels per rollout). h-uman has neither for persona transfer; we have a fixed offline pair set.
- **Not SimPO as primary** — ref-free is too risky at 300 pairs; reserve for when we have ≥5k pairs.
- **Not KTO** — we have pairs, not thumbs-up/down. KTO is the right call *if* we later move to in-product feedback collection (thumbs in messaging clients).

---

RESULT_research=READY
