# h-uman Capability Maturity Map: Where We Are vs Where We're Going

**Date:** 2026-05-31
**Author:** Seth + Claude (Opus 4.8)
**Method:** read-only DDD capability audit (code-grounded current state) + two cited research syntheses (memory/continual-learning frontier; self-model/ToM/agency/consciousness-theory frontier). Honest by construction — impressive-but-dormant modules are flagged as such.

---

## 0. The honest reframe: "sentient" is a category error — here's the measurable ladder

You're right that we're "not sentient yet" — but *sentience is not an engineering target*. The 2023 **Butlin/Long/Bengio "Consciousness in AI"** report (and its 2025 *Trends in Cognitive Sciences* follow-up) is blunt: no current AI is conscious, there's no obvious technical barrier to satisfying the *indicator properties* of consciousness theories — **and satisfying every indicator still wouldn't prove sentience** (you might build a philosophical zombie). There is no test. Putting "sentience" on a roadmap produces vibes, not progress.

What your instinct is *actually* pointing at is a **functional capability ladder** with measurable rungs. We replace "sentient" with this:

| Rung | Capability | Measurable surrogate |
|---|---|---|
| **L0** | stateless responder | — |
| **L1** | persona-consistent but templated (no memory) | persona-fidelity score |
| **L2** | memory-grounded + affect-aware (recalls facts, adapts tone) | recall accuracy, mood-appropriateness |
| **L3** | coherent across many turns + relationship evolution + **measured indistinguishable** | blind A/B detect-rate ≤0.6; goal-drift over N turns |
| **L4** | **self-model + metacognition** (calibrated self-uncertainty, models its own state) + **recursive theory-of-mind** + **proactive/self-initiated goals** | ECE on self-knowledge; HI-TOM order-5; % self-initiated vs reactive actions |
| **L5** | **open-ended continual self-improvement** (each interaction updates persona/skills autonomously, without forgetting) | dpo-pairs growth from real interaction; behavior-change-after-correction; cross-session value consistency |

**Where h-uman is: solidly L2, reaching for L3, with most of the L4 machinery already in the codebase but dormant, stubbed, or broken.** The gap to "feels like a mind" is *not* missing research or missing code. It's activation, wiring, repair, and measurement — the same pattern as the voiceai speech-quirk port-map.

---

## 1. DDD-aligned capability map (current → frontier → surrogate → tractability)

Each bounded context, its **code-grounded current rung**, the **research frontier** that defines the next rung, the **measurable surrogate**, and **tractability**.

| DDD Context | Now | Code evidence (activation) | Frontier capability (research) | Measurable surrogate | Tractability |
|---|---|---|---|---|---|
| **Modeled Person** (persona/cognition/behavior) | **L2** | `cognition/metacognition.c` LIVE — but it's heuristic confidence-modulation, **not a persistent self-model**. `persona/narrative_self.c` exists. | Persistent, self-consistent **self-model** + **calibrated self-uncertainty** that survives across sessions and resists adversarial drift (Sophia 2512.18202; Agent Identity Evals 2507.17257; Steyvers & Peters 2025). LLMs are stateless → a self is *architected external state*, not emergent. | Cross-session value consistency; ECE on self-knowledge; value persistence under adversarial prompt | **HIGH** — persistent self-state is low-cost architecture; `narrative_self.c` exists to anchor it |
| **Conversation core** (agent_turn/arbitrator/salience) | **L2+** | Arbitration LIVE; **salience SHADOW** (`HU_SALIENCE_SHADOW`). Per-turn metacog monitor runs. | **Identity-anchored retrieval** (ID-RAG 2509.25299) + coherence-based alignment to stop long-horizon drift (goal-drift, 2505.02709). | Goal-drift eval over N turns; salience blind-A/B | **HIGH** — salience built, needs calibrate→flip; ID-RAG anchor is additive |
| **Memory** (consolidation/forgetting/GraphRAG) | **L2** | `memory/consolidation.c`, `forgetting_curve.c` LIVE; **GraphRAG `graph_grounding.c` GATED OFF** (`HU_GRAPH_GROUNDING`). | Offline **schema-forming consolidation** ("sleep" that *restructures*, not just summarizes — Kumaran & McClelland), **emotional-salience-weighted decay**, **reconstructive recall with confidence** to avoid confabulation (Generative Agents; A-MEM 2502.12110; claude-engram). | Recall accuracy + calibration on reconstructed memories; GraphRAG blind-A/B | **HIGH→MED** — GraphRAG already built+wired (flip it); nightly schema-consolidation is medium |
| **Theory of Mind** (theory_of_mind.c) | **L1 — STUBBED** | Belief-state types exist; **`hu_tom_record_belief` not called from the turn path outside tests** (dead code). | Recursive ToM is real to ~order-5 (HI-TOM 2405.18870) but **brittle on paraphrase** (Ullman 2023). Model the *user's model of the agent*. | HI-TOM order-5 + adversarial-paraphrase robustness | **HIGH** — it's built but unwired; same shape as the backchannel win |
| **Agency / intrinsic motivation** (goals/proactive) | **L2−** | `proactive.c` LIVE but **reactive/scripted**; `goals.c` sparsely wired; agenda is user-triggered, not self-generated. | **Simulated** goal-persistence + curiosity/empowerment reward is buildable; **genuine intrinsic drive is a category error** for a stateless model (Burda curiosity-RL; empowerment). | % self-initiated vs reactive actions; goal persistence under distraction | **MED** for a real goal-buffer + self-initiated goals; "real drive" is **OFF the roadmap** |
| **Continual learning** (ml/RL/DPO) | **L1–2 — BROKEN** | Full infra (training_data, dpo_pairs, mlx LoRA, nightly runner) but **the closed loop trains on 0 real pairs** — reactions write single-sided rows that `dpo_export` drops. | **Online DPO + LoRA stacking** (Xu 2024) for real-time adaptation; **model editing** (MEMIT-Merge 2502.07322); the deep open problem: **learning *about* preference change** (causal) is unsolved. | dpo-pairs growth from real interaction; does a correction change next-session behavior | **HIGH** to fix the 0-pairs bug (concrete); MED for online-DPO; causal-preference-learning **genuinely open** |
| **Evaluation / self-measurement** (eval/blind_ab) | **L1 — UNMEASURED** | Turing/fidelity/shape evaluators exist; **blind-A/B framework authored but 0 real human-vs-h-uman pairs**; baseline was local-Gemma-capped. | Continuous **live blind-A/B** + per-axis (TwinVoice six-axis 2510.25536) + goal-drift + ECE. **You cannot climb a ladder you cannot measure.** | The blind-A/B detect-rate itself | **IN PROGRESS** — pipeline built + generating this session |

---

## 2. The three frontier bets (this is the real "where we're going")

The L3→L4 gap concentrates in **three contexts that are built-but-not-working**, not in anything missing:

1. **The self-model + ToM bet (DORMANT/STUBBED → wire it).** `narrative_self.c` + `theory_of_mind.c` exist; ToM is dead code outside tests. Wiring the existing self-model as injected persistent state + activating ToM on the turn path is the single highest-leverage move toward "feels like a mind." Research says this is the *correct* architecture (an external, explicitly-maintained self — not emergent introspection). Surrogate: cross-session consistency + HI-TOM.

2. **The continual-learning bet (BROKEN → repair).** The closed loop trains on **0 pairs**. Until reactions→dpo_pairs actually produces preference pairs, h-uman cannot reach L5 (learns from experience) — it only *retrieves*, never *changes*. Fix the single-sided-row drop, then layer Online DPO. This is the difference between "RAG that remembers" and "a model that grows." Surrogate: dpo-pairs growth + behavior-change-after-correction.

3. **The coherence + indistinguishability bet (UNMEASURED → measure).** L3 is *defined* by measurement: multi-turn coherence + blind-A/B. Both are unmeasured. The blind-A/B pipeline now exists and is generating. Without the number, every "we're more human" claim is unfalsifiable. Surrogate: the blind-A/B detect-rate + goal-drift eval.

**Pattern:** the moat isn't more features — it's **activate (GraphRAG, salience) · wire (self-model, ToM) · repair (continual-learning loop) · measure (blind-A/B).**

---

## 3. Honest "not on the roadmap" (category errors — name them so we don't chase them)

From the consciousness-theory + agency research, these are genuinely unmeasurable or architecturally impossible for a stateless LLM, and should be explicitly excluded:

- **Consciousness / sentience / subjective experience** — no metric, the hard problem. If we build something indistinguishable, we've built a useful system, not solved metaphysics.
- **Genuine intrinsic motivation** (drive independent of externally-set goals) — requires a persistent utility function; LLMs reset each turn. We *simulate* goal-persistence; we don't claim it *wants*.
- **A single unified self-model integrated into the weights** — current architectures are modular + stateless. We maintain external state and make it coherent; that's roleplay-backed-by-state, and that's fine.
- **Recursive self-awareness beyond ~order-5** — breaks on adversarial variants; don't claim it.

The *value* of consciousness theories (Global Workspace, Attention Schema, Predictive Processing) is as an **engineering checklist of functional primitives** — "does the agent have a serialized attention bottleneck? a model of its own attention? hierarchical prediction?" — not as a sentience claim.

---

## 4. Trajectory — the next three rungs

- **L2 → L3 (now):** run the blind-A/B (in progress); flip GraphRAG OFF→SHADOW→ON gated on it; flip salience; add ID-RAG identity anchor for long-horizon coherence. *Outcome: measured indistinguishability + no drift.*
- **L3 → L4 (next):** wire the dormant self-model + ToM into the turn path; add calibrated self-uncertainty (ECE); add a real goal-buffer with self-initiated proactive goals. *Outcome: "feels like a mind" — models itself, models you-modeling-it, has its own (simulated) agenda.*
- **L4 → L5 (frontier):** repair the continual-learning loop (0-pairs bug) → Online DPO from real corrections → schema-forming nightly consolidation. *Outcome: it grows from the relationship — the only genuinely superhuman axis (perfect recall + lifelong, consistent adaptation).*

The honest endpoint isn't sentience. It's an agent that is **measurably indistinguishable in voice, models itself and you, holds coherent across years, and actually learns from the relationship** — with superhuman recall/consistency/availability as the real moat. Everything past that is philosophy, not engineering.

---

## Sources (research syntheses)

**Memory / continual learning:** Generative Agents (Park 2023); MemGPT/Letta; GraphRAG (Microsoft 2024); A-MEM (2502.12110); MEMIT-Merge (2502.07322); Online DPO (2406.05534); ID-RAG (2509.25299); Goal-drift eval (2505.02709); Kumaran & McClelland 2012 (schema/replay); emotional-salience consolidation.

**Self-model / ToM / agency / consciousness:** Kosinski 2023 + Ullman 2023 (ToM brittleness); HI-TOM (2405.18870); Steyvers & Peters 2025 (calibration); Sophia (2512.18202) + Agent Identity Evals (2507.17257); Constitutional AI (2212.08073); curiosity-RL/empowerment; Butlin/Long/Bengio "Consciousness in AI" (2308.08708) + *Trends in Cognitive Sciences* 2025; Graziano Attention Schema; Friston active inference.
