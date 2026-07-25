---
title: "h-uman SOTA gap analysis — 2026-07-25"
created: 2026-07-25
status: snapshot
audience: maintainers, research
---

# h-uman SOTA Gap Analysis — 2026-07-25

Deep-research pass: 3 parallel web-research agents (persona/digital-twin, memory/learning
loops, local inference) + verified local state (daemon deployed 07:31 today, main@88add74e).
Confidence note: arXiv-cited claims are verifiable; some inference-stack numbers come from
blog benchmarks (presenc.ai, whatllm.org, digitalapplied.com) — treat those as directional.

## Verified internal state (2026-07-25 morning)

- Latest main IS deployed (binary built 07:31, daemon restarted 07:32; contains
  HU_PERSONA_HEAD + the 977cf37f proactive fix — the 07-22 "deploy pending" item is closed).
- Gates: PROMPT_TRIM=live, PERSONA_HEAD=shadow (0 events yet), GRAPH_GROUNDING=shadow,
  WARMTH_TONE_VOCAB=shadow, LLM_FACT_EXTRACT=shadow, PROACTIVE_CONTEXTUAL=on.
- prompt_trim live events still leave prompts at 17.5–24KB after trimming — the 16K
  persona-head problem persists in production until the compact head promotes.
- Rating sheet 0/12 (drip waiting on bab004). Human tier still empty since May.
- **BROKEN: nightly fidelity eval is degenerate (diagnosis corrected same day)** — the
  eval runs (~87 min, n=29) but (A) nightly_eval.sh:47 pins ADAPTER_GLOB to
  `seth-lora-v4-repair*` while production serves **v5** (seth-lora-v5-8bit-20260718, since
  07-18), and (B) PRE (base) and POST (adapter) passes both score mean 1.0 / delta 0.0 with
  near-identical elapsed times — suspect both passes query the live :8741 server whose v5
  adapter is permanently loaded (--adapter-path decorative), and/or scorer saturation.
  delta<0.05 → verdict SKIP nightly, recorded in the registry as score 1.0. (The
  "Adapter not found" SKIP log ended May 29 with the retired standalone plist.) Fix in
  flight in a spawned session with the corrected briefing.
- Nightly blind-A/B proxy: PASS 53.2% fool rate (07-24, n=47, CLI judge).

## What ISN'T SOTA (ranked by leverage)

### 1. Measurement loop — the biggest gap, and it's internal, not research
- Human tier 0/12 blocks every LIVE promotion. 2026 eval methodology (PersonaArena,
  arXiv 2605.17044) treats human-calibrated judging as table stakes: LLM-judge validity
  requires Cohen's kappa ≥0.6 vs human raters, ≥0.75 for release gates. h-uman has the
  infrastructure (drip + harvest + per-axis sheet already matching TwinVoice's 6 axes)
  but zero data. Everything downstream is opinion until n≥10.
- Single Gemini judge vs 2026 practice of ensemble/multi-agent debating judges
  (PersonaArena) to counter the known polished-text bias — the exact bias the simulated
  rater memory already documented ("too polished vs Seth-terse, but warmer").
- The fidelity-nightly SKIP bug (above) means the self-measuring loop shipped 07-11 is
  partially theater: registry looks green while measuring nothing. Fix = point the
  eval at the registry's live adapter (or the symlink), and record SKIP as SKIP, not 1.0.

### 2. Prompt/latency engineering — cheap, large, un-taken wins
- **No prefix caching**: the ~16KB persona head is re-prefilled every turn. mlx-lm
  v0.31.3 (already the installed generation) + vllm-mlx support prefix caching with
  ~84–95% hit rates; expected win 1.5–2.5s/turn TTFT, and it mitigates the head cost
  even before HU_PERSONA_HEAD promotes. Likely the single cheapest latency win.
- **Single-threaded request queue**: vllm-mlx (Feb 2026) does continuous batching +
  paged KV on MLX. The 07-11 incident (50 stacked requests degraded the live server
  ~40min) is exactly the failure mode it removes. Caveat: adapter support in its server
  API needs verification before migrating.
- 16K head itself: lever built (HU_PERSONA_HEAD compact head, shadow). Not-SOTA only
  until soak + promotion. Sequencing note: prefix caching changes the latency math —
  a cached 16K head may cost less than an uncached compact head rebuild.

### 3. Memory retrieval — generic summaries lost; query-time retrieval is the consensus
- GraphRAG shadow data (5 distinct injected sizes, generic community summaries) matches
  the failure the 2026 literature names: static summaries computed ahead of time.
  SOTA is **query-adaptive retrieval** — MemORAI (arXiv 2605.01386): focused subgraphs
  built at query time, query-conditioned edge weighting; hybrid dense+graph. This is
  the concrete redesign for the "make graph context conversation-specific" precondition
  already recorded before re-testing GraphRAG.
- Temporal memory ranking (TiMem, arXiv 2601.02845): distinguishing said-once vs
  said-repeatedly; h-uman's memory layer doesn't rank by reinforcement.
- Evolved-preference tracking: HorizonBench (arXiv 2604.17283) shows all 25 frontier
  models pick OUTDATED preferences ~60% of the time when preferences evolved. h-uman
  is untested here; its fact store has no supersedes/valid-until semantics on the
  generation path.

### 4. Learning loop — batch weekly vs online; drift unmonitored
- Weekly batch DPO is behind the online/streaming-preference frontier (RLUF, P-GRPO —
  Apple ML). Given tapback/reply signals already land in feedback_signals, a
  faster-cadence incremental step is feasible.
- Objective choice: arXiv 2601.12639 — SFT and vanilla DPO INCREASE persona drift at
  scale; ORPO/KL-regularized objectives mitigate it. h-uman's ORPO negative result
  (400-iter over-correction to blank) was a tuning failure, not a verdict on the
  objective; the parked "gentler ORPO / KL-anchored DPO" retrain plan is directionally
  what 2026 research recommends. Worth reviving for v6.
- **No persona drift detection in production** (Nautilus Compass, arXiv 2605.09863:
  behavioral-anchor cosine monitoring, ROC AUC 0.83). Adapter retrains ship with no
  drift monitor; nightly could embed fixed probe prompts and track drift over versions.
- Catastrophic forgetting: no mitigation beyond adapter versioning; A-Mem-style
  persistent persona notes decouple learned facts from weight updates.

### 5. Model/serving choices — mostly fine; one parked task should be killed
- **Kill the parked Q6_K requant task**: 2026 benchmarks put Q6_K in a dead zone
  (+0.3–0.6% ppl for 2.1× vs 8-bit's ~0% at 1.8×; Q4_K_M at 3.7×/1%). If latency ever
  becomes the blocker, jump 8-bit → Q4_K_M and A/B fidelity; otherwise stay 8-bit.
- Speculative decoding: still no off-the-shelf LoRA-adapted-target solution on MLX
  (DFlash-MLX is 2–3× but draft+adapter remains custom). The 05-30 negative result
  stands; keep deferred.
- Base model: Qwen 3.6-35B-A3B (MoE, 3B active) plausibly 1.8–2.2× faster with equal
  or better instruction-following; but persona-overlay quality on it is unknown and a
  swap invalidates the entire adapter lineage. Evaluate-only; not a near-term move.

### 6. Where h-uman is AT or AHEAD of SOTA (don't spend here)
- Real-person digital twin: no shipped competitor (Sesame/Character.AI/Replika are
  authored-character engines). The niche is still open; also means no external benchmark.
- Proactive outreach with bandit reward (send → REPLY/IGNORED): ahead of published
  work; ProEvent/PRISM add event-anticipation and cost-sensitive timing ideas, but the
  measured loop itself is frontier. (n=3 — keep accumulating before any tuning.)
- Activation steering: 2026 papers confirm coherence collapse at effective doses —
  the MoE/steering CLOSED verdict stays closed.
- Anti-AI-tell mitigations (sanitize-before-split, burstiness, timing variance,
  quirk exemplars): aligned with 2026 detection literature.
- Blind A/B protocol design (paired real-vs-generated, per-axis sheet): matches
  TwinVoice methodology. The gap is data (0/12), not design.
- Benchmark context update: TwinVoice discriminative is now led by Claude-Sonnet-4 at
  76.2% vs human ~66% — the "no model beats humans at persona" line from the 05-30
  roadmap is stale for the discriminative axis (generative-ranking still weak: GPT-5
  48.5%). The strategy conclusion (own memory+specificity, not voice) still holds.

## Recommended sequence (leverage order)
1. Fix fidelity-nightly adapter path + SKIP≠1.0 (S, today-sized) — restores real
   self-measurement of the serving v5 adapter.
2. Get rating sheet moving (answer bab004 → n≥10) — unblocks every LIVE gate.
3. Prefix caching on the MLX server (S/M) — biggest latency win, no quality risk.
4. Persona-head soak → promote via human tier (already in flight).
5. GraphRAG redesign to query-adaptive subgraph retrieval (M) before any re-test.
6. Drift monitor probes in nightly (S/M); revive gentle-ORPO for v6 (M, after human
   tier exists to judge it).
7. Kill Q6 task; note Q4_K_M as the future latency escape hatch.
