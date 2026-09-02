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
2. **Standard memory benchmarks on our retrieval.** Run LongMemEval-S and LoCoMo through `human memory search` (keyword / `--semantic` / hybrid). Gate: numbers; target ≥ MemPalace's 96.6% R@5 on LongMemEval with no LLM in the loop, since our store is the same shape (verbatim + temporal graph + vectors). **Measured 2026-09-02 (Appendix D): LongMemEval-S R@5 keyword 0.883 → semantic 0.983; LoCoMo-10 R@10 keyword 0.650 → semantic 0.767 → hybrid 0.783.** The LongMemEval target is met by the semantic arm alone; the gate now moves to #3.
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

## Appendix — the first authorship measurement (2026-09-02, LUAR-MUD, n=37 blind-A/B trials)

| Pair | LUAR cosine |
|---|---|
| Seth ↔ Seth (different texts, same author) | **0.396** |
| adapter reply ↔ real Seth | **0.344** |
| adapter ↔ adapter | **0.462** |

AUC of "is this real Seth?" by LUAR similarity: **0.782**. In 13/37 trials the adapter reply
sits closer to its real counterpart than Seth's own self-similarity.

Reading: the same shape PersonalBench reports on blogs, reproduced on texting-length inputs.
The adapter's replies resemble *each other* more than Seth's replies resemble each other —
the model's fingerprint and compressed dispersion dominate — and they sit below Seth's
self-similarity. Absolute values are not comparable to the Blog-corpus floor/ceiling (64-token
windows vs 200-word posts); the ordering and the AUC are. This is the north-star baseline:
an adapter is better when AI↔Seth rises toward Seth↔Seth **and** AI↔AI falls toward it.
Artifact: `~/.human/logs/luar-authorship-2026-09-02.json`; probe: `/tmp/luar_probe.py` (to be
promoted into `scripts/blind_ab/authorship_gap.py` with 5v5 profile embeddings per PersonalBench).

### Appendix B — 5v5 PersonalBench protocol on our corpus (2026-09-02)

| Adapter (replies) | ceiling Seth↔Seth | floor Seth↔other humans | twin Seth↔adapter | gap closed |
|---|---|---|---|---|
| June `seth-voice-l4` (n=37 trials) | 0.707 | 0.615 | **0.576** (casual 0.569 / substantive 0.627) | −0.42 |
| **v6 ORPO, production head (n=36, regenerated live)** | 0.715 | 0.633 | **0.626** (casual 0.629 / substantive 0.627) | −0.09 |

v6 moved from well below the stranger floor to *at* it; it is not yet inside the human band.
Floor built from 60 other senders in chat.db (5 texts each), 200 bootstrap splits, 64-token
episodes. Script: `scripts/blind_ab/authorship_gap.py`; artifacts
`~/.human/logs/authorship-gap-2026-09-02.json`, `authorship-gap-v6-2026-09-02.json`.

### Appendix C — nightly LoRA has not trained since at least 08-08; why (2026-09-02)

The 03:07 run "succeeded" (rc=0) with a **349-byte adapter**. Chain: the retrain script
stopped serving with `launchctl kill SIGTERM`; the plist's `KeepAlive={Crashed,SuccessfulExit}`
relaunched the server within seconds; `training_loop.py`'s preflight saw a live server and
refused; its failure path then wrote an *empty-tensors safetensors and returned 0*; the
script staged it. The steering extractor then loaded the base (rc=137, killed) and the
machine rebooted four minutes later. Fixed: bootout/bootstrap instead of kill/kickstart with
a wait-until-port-free, refusal exits 3 with no placeholder, and `scripts/adapter_is_real.py`
(size > 1 MB, lora_b non-zero, scale ≤ 4) gates staging. Proof pending: the next window.

### Appendix D — standard memory benchmarks through `human memory search` (2026-09-02)

Harness `scripts/eval_memory_benchmarks.py`, binary at `aa2a1a79b`, embedder `nomic-embed-text` (8-bit) on :8749, datasets under `~/.human/eval-data/`. Each conversation is loaded into a fresh SQLite store and queried three ways; no LLM answers anything, so this is retrieval only. Results file: `docs/plans/2026-08-02-semantic-retrieval/memory-benchmarks-2026-09-02.json`.

| Benchmark | n | keyword (FTS5) | semantic (`--semantic`) | hybrid (harness-side RRF k=60) |
|---|---|---|---|---|
| LongMemEval-S, session-level R@5 | 60 (10 per type) | 0.883 | **0.983** | 0.983 |
| LoCoMo-10, turn-level R@10 | 60 | 0.650 | 0.767 | **0.783** |

LongMemEval by type: keyword loses only on `single-session-preference` (0.60 → 1.00) and `temporal-reasoning` (0.80 → 1.00); `multi-session` sits at 0.90 on every arm. LoCoMo by category: semantic is 1.00 on category 1 (n=8) where keyword is 0.38; category 3 (n=3) is the one place keyword beats semantic. The hybrid here is fused in the harness; the in-binary `hybrid.c` fusion is what C2 is rebuilding.

**Caveat that cost a run:** the first pass reported LoCoMo 0.000 on every arm. That was the harness, not the store — the key parser stopped at the first colon, so `D1:3` became `D1` and no evidence ever joined (fixed in the same commit as this appendix). A zero on every arm of a benchmark is a join failure until proven otherwise, exactly the shape `.claude/rules/reports-success-does-nothing.md` warns about.

Same night, C4's nightly tiers produced their first numbers on freshly regenerated trials (n=35): LUAR ceiling 0.709, twin 0.633, floor 0.621 (gap closed 0.136); Gemini inverse-Turing judge n=36 accuracy 0.667, AUC 0.651. Both are now `nightly-watchdog.sh` jobs.

