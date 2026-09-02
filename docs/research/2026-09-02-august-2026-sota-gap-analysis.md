---
title: August-2026 SOTA gap analysis — what "better than human" is measured by, and where h-uman stands
date: 2026-09-02
status: draft-for-execution
---

# August-2026 SOTA gap analysis

Written the morning after the 2026-09-01 e2e pass, on Seth's instruction to hold the
allowlist until the platform is SOTA on grounded evidence. Every row below pairs a
frontier claim (paper or release, August 2026 unless noted) with h-uman's **measured**
state and the gate that would prove the gap closed. Nothing here is "done" until the
gate number exists.

## The frontier, per axis

| Axis | What August 2026 establishes | Source |
|---|---|---|
| **Authorship (the real "sounds like me" metric)** | PersonalBench: every inference-time personalization method (few-shot, profile, contrastive+stylometrics) lands at LUAR similarity **0.48–0.51**, below the *cross-author human floor* **0.626** (same-author ceiling 0.756). Generated text is farther from any human than random humans are from each other; the LLM's own fingerprint dominates. "Closing the authorship gap likely requires training-time adaptation." LUAR is open (`rrivera1849/LUAR-MUD`). | [PersonalBench 2608.19746](https://arxiv.org/abs/2608.19746), [2604.26460](https://arxiv.org/abs/2604.26460) |
| **Detectability** | Computational-Turing-test framing: human judgment is blunt; calibrated classifiers still separate. Fine-tuned generators are what break detectors ("detectability drops dramatically" for fine-tuned models the detector never saw). LLM judges (Opus 4.6 / GPT-5.5) now beat human judges at human-vs-AI dialogue. | [2511.04195](https://arxiv.org/abs/2511.04195), [2506.09975](https://arxiv.org/abs/2506.09975), [Inverse Turing Bench 2606.21844](https://arxiv.org/pdf/2606.21844), [CT2 2605.20761](https://arxiv.org/abs/2605.20761) |
| **Memory: real-dialogue personalization** | AlpsBench (real WildChat, 2,500 sequences): extraction F1 tops at **0.67**; memory *update* plateaus ≈**0.78** even for frontier models; classical retrieval collapses at 1,000 distractors (BM25 0.21, embeddings 0.49 vs LLM-retrieval >0.94); **adding memory degrades virtual-reality awareness and emotional intelligence** — over-reliance on retrieved memories. Implicit facts are the failure mode (direct 0.55 vs indirect 0.003). | [AlpsBench 2603.26680](https://arxiv.org/html/2603.26680v2) |
| **Memory: architecture** | Mem0's 2026 algorithm: single-pass extraction that stores *agent-generated* facts as first-class + **multi-signal fusion (semantic + keyword + entity)** → LongMemEval 94.4, temporal +29.6. EverMemOS: MemCells (episodes + atomic facts + **time-bounded "foresight"**) → MemScenes → reconstructive recollection with a sufficiency check; ablating scenes −3.9 pts, cells −11.2. MemPalace: verbatim storage, temporal SQLite graph, 96.6% LongMemEval R@5 with no LLM in the loop, MIT, local. CloneMem: hybrid vector+graph wins for clones. Temporal queries remain the hardest category everywhere. | [Mem0 report](https://mem0.ai/blog/state-of-ai-agent-memory-2026), [EverMind](https://evermind.ai/blogs/top-ai-memory-systems-benchmarked-in-2026), [MemPalace comparison](https://rohitraj.tech/en/notes/open-source-ai-agent-memory-mem0-vs-zep-letta-2026), [CloneMem 2601.07023](https://arxiv.org/pdf/2601.07023), [PersonaTree 2606.04780](https://arxiv.org/pdf/2606.04780) |
| **Preference drift over time** | HorizonBench: failure modes are reverting to earlier preferences, holding contradictory tastes, and losing cumulative modifications. | [HorizonBench 2604.17283](https://arxiv.org/pdf/2604.17283) |
| **Persona evolution** | "Do AI Personas Grow?": agents shift traits after life events at the same rate whether or not humans do; magnitudes below human effect sizes; persona dispersion compressed 3–4×. BFI-Adapt released. | [2608.06485](https://arxiv.org/abs/2608.06485) |
| **When to speak** | When2Speak (216k examples): SFT is over-conservative (misses half of warranted turns, MIR 0.52); RL with asymmetric reward fixes it (MIR 0.19) at the cost of more false interruptions (FIR 0.17). TimelyChat/TIMER: 79.1% turn-level F1 predicting *how long* to wait. | [When2Speak 2605.05626](https://arxiv.org/html/2605.05626v1), [TIMER 2506.14285](https://arxiv.org/abs/2506.14285), [DiscussLLM 2508.18167](https://arxiv.org/pdf/2508.18167) |
| **On-device training** | mlx-tune (v0.6, June 2026): SFT/DPO/ORPO/GRPO/KTO/**SimPO** natively on MLX, per-expert LoRA for 39+ MoE architectures, QLoRA on 4-bit bases; Unsloth-compatible API. SimPO/KTO are reference-free. | [mlx-tune](https://github.com/ARahim3/mlx-tune) |

## Where h-uman stands (measured, 2026-09-01/02)

| Axis | h-uman today | Gap |
|---|---|---|
| Authorship | v6 is **trained** (ORPO, margin engaged) — the one lever PersonalBench says works — but we have **no authorship number**. Human blind A/B: detection 0.225 at n=40 (PASS) — a blunt judge by the frontier's own account. | No LUAR/stylometric measurement of Seth-vs-adapter; no per-register breakdown. |
| Detectability | Binoculars classifier gate exists, never run (needs the retrain window; all rated cycles carry 12 keyed trials, the n=37 run is usable). | No classifier-tier number; no LLM-judge (Inverse-Turing style) tier. |
| Memory (real dialogue) | FTS5 + graph + (shadow) semantic recall; Phase-2 proved **+25 recall@5** on 20 paraphrase probes. Fact extraction: 0.44 facts/msg on own messages, closed predicates 84%. | Never run on LoCoMo/LongMemEval/AlpsBench; no update-F1 number; no measurement of the AlpsBench over-reliance effect (memory hurting EI/virtual-reality awareness) — the exact risk of flipping semantic recall LIVE. |
| Memory architecture | Multi-signal fusion exists (`hybrid.c`: keyword + semantic + graph) — the Mem0 shape. Bi-temporal edges exist (`event_start/event_end/supersedes_id`) — EverMemOS "foresight" shape. Reconstructive retrieval (scene selection + sufficiency check): **absent**; agent-generated facts as first-class: **absent** (daemon output isn't even separable from Seth's in chat.db until `message_ref` accrues). | Reconstructive recollection layer; agent-fact provenance; MemScene-style consolidation (we have `insight:*` rows but no scene structure). |
| Preference drift | Supersession now closes old edges; grounding read is time-windowed. | No HorizonBench-style eval of reverting/contradiction; opinions table has `superseded_by` but no validity window. |
| Persona evolution | Persona is static JSON + measured style card; life-chapter repo exists. | No event-induced trait modelling; no BFI-Adapt-style check that the persona's emotional register moves after real events (move, job change). |
| When to speak | Proactive timing: `llm_decides` + 120 s you-answer-first window + Pare-Bench-style low-rate; reply delay humanised by heuristics. | No MIR/FIR measurement; no learned delay model (TIMER-style); proposals mostly declined for unmeasured reasons. |
| On-device training | Patched `mlx-lm-lora` ORPO (our patch), lora_b≠0 guard, scale pinned. | Not on mlx-tune; no SimPO/KTO (reference-free, cheaper); per-expert LoRA for GLM-4.5-Air MoE unverified. |

## Gaps ranked by leverage, each with its gate

1. **Authorship gap measurement (LUAR) → the new north star.** Run `LUAR-MUD` on real-Seth vs adapter replies (5v5 profile embeddings per PersonalBench), report Seth-self / AI-Seth / cross-author floor on *our* corpus, per register. Gate: number exists; promotion of any adapter requires AI-Seth similarity to move toward the same-author ceiling. (Probe started 2026-09-02; result appended below when it lands.)
2. **Standard memory benchmarks on our retrieval.** Run LongMemEval-S and LoCoMo through `human memory search` (keyword / `--semantic` / hybrid). Gate: numbers; target ≥ MemPalace's 96.6% R@5 on LongMemEval with no LLM in the loop, since our store is the same shape (verbatim + temporal graph + vectors).
3. **Over-reliance check before semantic recall goes LIVE.** AlpsBench's finding is the exact risk: memory makes EI and hypothetical-vs-real worse. Gate: a blind A/B on the corrected harness with SHADOW vs LIVE, scored on the humanness composite *and* an EI axis; LIVE only if EI does not drop.
4. **Reconstructive recollection.** Replace one-shot top-k with scene-select → retrieve+rerank → time-bounded filter → sufficiency check (EverMemOS). We already have the pieces (`insight:*` rows, bi-temporal edges, reranker). Gate: LongMemEval temporal category and the AlpsBench "update" task improve; no regression on #3.
5. **Agent-generated facts as first-class, with provenance.** Now that reactive `message_ref` lands, store what *h-uman* said as facts with `source=agent` and a confidence discount, so the graph stops treating its own output as Seth's and can also recall its own commitments. Gate: `promised_to`/commitment recall on a held-out set of daemon promises.
6. **Classifier + LLM-judge tiers beside the human gate.** Binoculars AUC (script exists, needs the window) plus an Inverse-Turing-style judge (Opus 4.6-class) over the same trials. Gate: three numbers per cycle; disagreement between tiers is the signal to investigate.
7. **When-to-speak as a measured policy.** Log every proactive *proposal* with the decision and outcome; compute MIR/FIR against Seth's own reply-back behaviour; train the timing/delay predictor (TIMER-style) on chat.db inter-message gaps. Gate: MIR and FIR both reported; delay model beats the heuristic on held-out gaps.
8. **Trainer upgrade: mlx-tune with SimPO/KTO and per-expert LoRA on GLM-4.5-Air.** Gate: an adapter trained by mlx-tune passes the lora_b≠0 guard, the capability smoke, and improves LUAR (#1) over v6.
9. **Persona evolution.** Model event-induced register shifts (move, job change) with BFI-Adapt as the check. Gate: directional fidelity on the events that actually happened to Seth this summer.

## Where we are already at the frontier (don't spend here)

Reference-free preference training on-device with a working objective (ORPO patched, margin engaged); multi-signal fusion and bi-temporal edges already in the store; measurement discipline (refuse-don't-fallback, artifact-not-log, lora_b≠0) stricter than what the memory vendors publish.

## Sequence

#1 and #2 are measurements and run first (days, no product change). #3 gates the only pending activation (semantic recall LIVE). #4/#5 are the architecture work (weeks). #6/#7 are instruments. #8 is the training upgrade that #1 will score. #9 last. The allowlist stays closed until #1–#3 have numbers.
