# SOTA Reward Modeling & Evaluation — May 2026 Survey

**For:** h-uman persona fidelity gate redesign (Sprint 9+)
**Driving incident:** Sprint 8 smoke-run shipped a +0.046 "win" for an 80% broken adapter. Cause: synthetic lexical-surface fingerprint (lowercase / abbrev / length ratios) could not distinguish coherent English from `<pad>` token gibberish.
**Conclusion up front:** the lexical fingerprint is a textbook Goodhart-collapsed proxy. Replace it with a layered gate: held-out PPL sanity floor → small coherence judge → persona-specific PRM-style verifier → ensembled aggregation with position/length de-biasing.

---

## Top 5 Papers

1. **Lightman et al., "Let's Verify Step by Step" (OpenAI, 2023)** — the founding PRM paper. PRM800K dataset, step-level supervision beats outcome-only verifiers on MATH. Key lesson: granularity of supervision matters more than model scale. [arxiv 2305.20050]

2. **Khalifa et al., "Process Reward Models That Think" (ThinkPRM, 2025, arxiv 2504.16828)** — verbalized CoT verifier fine-tuned on ~8K step labels (1% of PRM800K) outperforms discriminative PRMs and LLM-as-a-Judge. **Directly applicable to h-uman:** we cannot afford 800K labeled persona-turn samples; we *can* afford 8K. Verifier explains its reasoning, making the gate auditable.

3. **Gao, Schulman, Hilton, "Scaling Laws for Reward Model Overoptimization" (ICML 2023)** + **Laidlaw et al., "Catastrophic Goodhart: KL regularization does not mitigate heavy-tailed reward misspecification" (NeurIPS 2024, openreview UXuBzWoZGK)** — the proxy-gold gap is the universal failure mode. Hump-shaped curve: proxy reward rises monotonically, gold reward peaks and falls. **This is exactly what we saw in Sprint 8.** KL regularization alone won't save us; the proxy itself has to be hardened.

4. **Coste et al., "Reward Model Ensembles Help Mitigate Overoptimization" (2023, arxiv 2310.02743)** + 2025 follow-ups: conservative aggregation (min, uncertainty-weighted) reliably reduces overoptimization. LoRA-headed ensembles (Reward LoRA Ensembles, ICLR 2025) make this cheap. **But:** ensembles that share a bias still fail — we need *orthogonal* judges, not N copies of the same critic.

5. **"Judging the Judges: Systematic Study of Position Bias in LLM-as-a-Judge" (IJCNLP 2025, aclanthology 2025.ijcnlp-long.18)** + **"Evaluating Scoring Bias in LLM-as-a-Judge" (arxiv 2506.22316)** — position bias is real, judge-specific, and worsens with small quality gaps (our exact regime: adapter A vs base, delta ~0.05). Mitigations: randomized order, balanced position calibration, regression-based bias correction on small human-labeled set. **The Sprint 8 result almost certainly contained uncontrolled judge bias.**

Supplementary: **RewardHackWatch** (github aerosta/rewardhackwatch, 2025) — 89.7% F1 on METR MALT dataset for *detecting* reward hacking trajectories. METR's June 2025 frontier-model audit confirmed reward hacking is now the dominant failure mode at frontier scale. **Han et al., "Quantifying Global Faithfulness in Persona-Driven Role-Playing" (NeurIPS 2024)** — APC scores correlate with human persona-fidelity judgments; usable as DPO reward.

---

## Recommended Architecture for h-uman's Gate

Replace the single lexical fingerprint with a **four-stage cascade**, each cheaper than the next and gating it:

```
candidate generation
        │
        ▼
[1] PPL FLOOR  ─── reject if held-out PPL > 3× base model PPL on persona dev set
        │           (catches <pad> gibberish, broken sampling, mode collapse)
        ▼
[2] COHERENCE JUDGE  ─── small local LLM (3-8B, e.g. Qwen / Mistral) with G-Eval
        │              prompt: "is this fluent English / coherent?"
        │              REJECT below 0.7; cost ~$0 (local)
        ▼
[3] PERSONA PRM  ─── ThinkPRM-style verbalized verifier on ~5-10K persona examples
        │           outputs (a) step-by-step rationale (b) scalar persona-fidelity score
        │           trained from APC (Han 2024) labels on h-uman persona example bank
        ▼
[4] ENSEMBLE AGGREGATION  ─── min/uncertainty-weighted across 3-5 orthogonal heads
        │                     (lexical, coherence, persona-content, persona-style, length-debiased)
        ▼
    Pareto report  ──── per-objective scores, not a single delta
                        win-rate only declared if Pareto-dominant on ≥3 of 5 axes
```

Why the cascade beats a single rich judge:
- Stage 1 is free and catches the Sprint 8 class of failure deterministically.
- Stage 2 catches "coherent but off-persona" failures *before* paying for stage 3.
- Stage 3 is the only stage that can be gamed; stage 4 ensembling + bias correction caps the damage.
- Pareto reporting kills single-number Goodhart — Coste 2023 + Laidlaw 2024 say KL alone cannot, so we *structure the report* to surface trade-offs.

---

## Sprint 9 Stories (citation-grounded)

**US-9.1 — PPL sanity floor.** Compute held-out PPL on a 1K-sample persona dev set; reject adapters with PPL > 3× base. Cites Liu et al. "Can Perplexity Predict Finetuning Performance?" (arxiv 2404.18071) — lower PPL strongly predicts SFT success when scale is fixed. **AC:** Sprint 8's broken adapter must fail this gate.

**US-9.2 — Local coherence judge.** Integrate G-Eval (DeepEval framework) with a 7B local model scoring fluency/coherence on every candidate. Cites Liu et al. G-Eval (Spearman 0.514 with human on coherence). **AC:** rejection rate on `<pad>`-poisoned outputs = 100%.

**US-9.3 — Persona PRM v0 (ThinkPRM-style).** Collect 5-10K step-level persona-fidelity labels from existing persona example banks. Fine-tune a 7-8B model as a verbalized verifier (cites arxiv 2504.16828). Output: rationale + scalar. **AC:** correlation with human persona-fidelity rating ≥ 0.55 on held-out test.

**US-9.4 — Orthogonal ensemble + bias correction.** 3-5 judges, each on a different feature family. Aggregate via min and uncertainty-weighted (cites Coste 2023, RRM ICLR 2025). Add regression-based position/length bias correction calibrated on 200 human-labeled pairs (cites IJCNLP 2025 position-bias study). **AC:** randomized A/B order produces same verdict ≥ 95% of trials.

**US-9.5 — Pareto adapter report.** Replace single-delta output with per-objective scorecard + Pareto-dominance check. Cites Zhong et al. "Pareto Multi-Objective Alignment" (arxiv 2508.07768). **AC:** a Sprint-8-class broken adapter cannot be reported as a win.

**US-9.6 — Reward-hacking watchdog.** Port RewardHackWatch trajectory detector (github aerosta/rewardhackwatch) as a CI gate that flags adapter outputs which spike on stage 1+2 but collapse on stage 3 — the signature of judge-gaming. **AC:** trips deterministically on a synthetic adapter that copies high-PPL tokens verbatim from the reward prompt.

---

`RESULT_research=READY`

Sources:
- [Process Reward Models That Think (ThinkPRM)](https://arxiv.org/abs/2504.16828)
- [Let's Verify Step by Step (Lightman et al.)](https://cdn.openai.com/improving-mathematical-reasoning-with-process-supervision/Lets_Verify_Step_by_Step.pdf)
- [Scaling Laws for Reward Model Overoptimization](https://arxiv.org/abs/2210.10760)
- [Catastrophic Goodhart (NeurIPS 2024)](https://proceedings.neurips.cc/paper_files/paper/2024/file/1a8189929f3d7bd6183718f42c3f4309-Paper-Conference.pdf)
- [Reward Model Ensembles Help Mitigate Overoptimization](https://arxiv.org/abs/2310.02743)
- [Judging the Judges: Position Bias in LLM-as-a-Judge](https://aclanthology.org/2025.ijcnlp-long.18/)
- [Evaluating Scoring Bias in LLM-as-a-Judge](https://arxiv.org/html/2506.22316v1)
- [RewardHackWatch](https://github.com/aerosta/rewardhackwatch)
- [METR: Recent Frontier Models Are Reward Hacking](https://metr.org/blog/2025-06-05-recent-reward-hacking/)
- [Pareto Multi-Objective Alignment for Language Models](https://arxiv.org/abs/2508.07768)
- [Can Perplexity Predict Finetuning Performance?](https://arxiv.org/pdf/2404.18071)
- [G-Eval / DeepEval](https://deepeval.com/docs/metrics-llm-evals)
- [Quantifying Global Faithfulness in Persona-Driven Role-Playing (APC, NeurIPS 2024)](https://dl.acm.org/doi/10.5555/3737916.3738782)
- [Reward LoRA Ensembles (ICLR 2025)](https://proceedings.iclr.cc/paper_files/paper/2025/file/fd7259e22add6de6df8ff0ccc902a34d-Paper-Conference.pdf)
- [RRM: Robust Reward Model Training (ICLR 2025)](https://proceedings.iclr.cc/paper_files/paper/2025/file/9d46574e5baae5121180228223a11836-Paper-Conference.pdf)
