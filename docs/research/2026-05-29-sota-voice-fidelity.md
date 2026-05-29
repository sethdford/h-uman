# SOTA Voice-Fidelity Analysis — May 2026

> Deep web research pass (2026-05-29) on the state of the art in making an LLM
> faithfully reproduce a specific individual's voice/persona, mapped to what
> h-uman already has vs. what's missing to reach SOTA.
>
> **Confidence flags:** lab products + the data/RAG consensus are HIGH confidence
> (corroborated across multiple sources). Specific arXiv IDs below are LEADS to
> verify, not independently confirmed in this pass — treat as pointers.

## What the frontier labs actually do (2026)

None ship per-user *fine-tuning* at scale. The pattern is **character + memory/RAG**:

| Lab | Approach (2026) | Source |
|---|---|---|
| Anthropic | Character/persona at train time ("Persona Selection Model", Feb 2026) — fixed assistant persona, not per-user adaptation | [alignment.anthropic.com/2026/psm](https://alignment.anthropic.com/2026/psm/) |
| OpenAI | Memory = RAG-style retrieval over conversation history (expanded May 2026) | [openai.com memory](https://openai.com/index/memory-and-new-controls-for-chatgpt/) |
| Google | Gemini "Personal Intelligence" — cloud multimodal retrieval across Google apps (Apr–May 2026); does NOT train on user data | [blog.google personal-intelligence](https://blog.google/innovation-and-ai/products/gemini-app/personal-intelligence/) |
| Apple | On-device context-awareness, Private Cloud Compute; not voice/style FT | [apple.com newsroom 2026-05](https://www.apple.com/newsroom/2026/05/apple-unveils-new-accessibility-features-and-updates-with-apple-intelligence/) |
| Meta | Hyperpersonalization via recommendation, not documented voice FT | — |

**Implication:** h-uman's local-LoRA bet is *more aggressive* than any lab's shipped approach. That's a differentiator IF the data supports it — and a liability if it doesn't (see data finding). The labs' convergence on memory/RAG is a strong signal.

## The data finding (HIGH confidence, corroborated)

- Voice fine-tuning needs **~100–500 quality samples** (LoRA), 500–2000+ for high fidelity; **quality ≫ quantity** ("200 curated beat 2,000 sloppy"). ([particula](https://particula.tech/blog/how-much-data-fine-tune-llm))
- **At low data volume, RAG-over-own-messages beats PEFT/fine-tuning**; hybrid (light FT + RAG) is the 2026 production standard. ([Improving RAG for Personalization, arXiv 2504.08745](https://arxiv.org/pdf/2504.08745))
- Author-feature RAG (+ contrastive examples from *other* authors) gives ~**+15%** over baseline RAG.

→ **The W7 data audit + LoRA-vs-RAG A/B should decide the primary path.** For one person's message history (likely thin), the evidence favors RAG/hybrid.

## Emerging techniques — SOTA vs reach

| Technique | SOTA? | Reachable in h-uman's C agent? |
|---|---|---|
| **Persona vectors / activation steering** (continuous, compositional trait control via residual-stream perturbation) | Yes, hot area | ❌ Runtime-level (inside mlx-server.py / model forward pass). h-uman approximates the *user-facing* benefit via prompt-level persona overlays. |
| **KV-cache personalization** (swap persona state at inference, no LoRA reload) | Yes | ❌ Runtime-level (inference engine KV cache). |
| **RAG + author features + contrastive examples** | Yes, +15% | ✅ In-scope (memory + retrieval + prompt assembly). |
| **Online preference learning** (Amulet/T-POP — adapt from implicit feedback at test time) | Emerging | ⚠️ Partly — h-uman has DPO-pair collection (Wave 1/2); test-time re-steering would be new. |
| **Multi-turn persona-consistency eval + correction** (PICon: retest/external/Q&A consistency) | Yes (eval SOTA) | ✅ In-scope (offline eval today; online correction is the gap). |
| **Authorship-verification judges + atomic/uniqueness eval** (Eval4Sim: consistency/coherence/uniqueness) | Yes (eval SOTA) | ✅ In-scope (W7 does similarity; uniqueness/contrastive + multi-turn consistency are missing). |

## The gap, ranked for h-uman

**Reachable now (this codebase, testable):**
1. **SOTA eval dimensions** — W7 measures similarity-to-reference but lacks Eval4Sim's *uniqueness* (contrastive: closer to target than to other authors) and *multi-turn consistency* (style drift across a conversation). Cheapest, highest-confidence add; sharpens every other decision. **← building now.**
2. **RAG-over-own-messages as a first-class serving path** (with author-features + contrastive grounding). The data/lab evidence says this is likely the SOTA-aligned primary path for one person. Bigger C change; the A/B harness should gate the decision.
3. **Online persona-consistency correction** — make the per-axis/contradiction eval feed a corrective directive into the *next* turn (h-uman has contradiction detection + per-axis eval, but they run post-hoc).

**Frontier, runtime-level (needs mlx-server.py + live validation — spec, don't guess):**
4. **Persona vectors / activation steering** in the local serving layer — continuous compositional trait control. The biggest "feels SOTA" jump, but it requires owning the forward pass.
5. **KV-cache persona swap** — sub-100ms in-conversation style switching.

## Recommendation

Decide the primary path with **data, not assumption**: run the W7 audit + A/B. If thin/RAG-wins (likely), pivot the primary voice path to **RAG/hybrid** (#2) — that's both the lab-convergent and data-supported SOTA move, and it's reachable here. The runtime techniques (#4/#5) are the next frontier but belong in a serving-layer spec, validated live.

## Sources

See inline links above. Primary high-confidence: Anthropic PSM, OpenAI memory, Google Personal Intelligence, Apple newsroom (2026); particula data guide; arXiv 2504.08745 (RAG personalization). Paper-ID leads to verify: PERSONA / activation-steering (ICLR 2026), XKV/Jarvis (KV-cache), PICon/SPASM/Eval4Sim (multi-turn + uniqueness eval), Amulet/T-POP (test-time personalization).
