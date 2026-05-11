---
title: "ADR — Persona eval judge: hybrid frontier + local"
created: 2026-05-11
status: accepted
deciders: engineering, product
parent: ../2026-05-10-sota-roadmap-6mo.md
related:
  - ../2026-05-10-sota-roadmap-6mo.md
  - ../../standards/ai/evaluation.md
---

# ADR: Persona eval judge — hybrid frontier + local

## Context

The SOTA roadmap (Phase A6.1) calls for a "feels like Seth" eval suite with model-judged scoring. Judge selection is a high-leverage decision: it determines both **cost** (frontier judges are expensive at scale) and **credibility** (a self-judge can be discounted as biased).

Three patterns were considered:

| Pattern | Cost / 1k scenarios | Credibility | Local? |
|---|---|---|---|
| **A** — Frontier judge only (Gemini 3.1 Pro) | ~$20–40 | High; matches industry practice (MT-Bench, AlpacaEval) | No |
| **B** — Local judge only (Gemma 3 27B or Qwen 3) | ~$0 (compute time) | Mixed; "self-judge" concern when local judge is similar family | Yes |
| **C** — Hybrid: frontier for public/anchor runs, local for nightly | ~$5/mo + compute | High where it matters; cheap iteration | Partial |
| **D** — Human-only | $$$ + slow | Highest | n/a |

A4 (the personal-model retrieval eval) is a separate eval and uses deterministic relevance scoring — no model judge needed. This ADR covers only persona-fidelity / preference-style evals.

## Decision

**Accept Option C — hybrid.**

- **Frontier judge** (Gemini 3.1 Pro via Vertex AI with ADC, per CLAUDE.md AI Model Version Policy) judges:
  - The anchor / locked baseline at every program phase boundary (one run, locked into `docs/evaluation/baseline.json`).
  - The Month-6 public benchmark drop (Phase A6.2, E5).
  - Any release candidate before tagging.
- **Local judge** (Gemma 3 27B running through the same mlx-server, separate process) judges:
  - Nightly continuous-eval runs (Phase E4).
  - PR-time eval gates (Phase E3).
  - A/B harness comparisons in the continuous-learning loop (Phase A5.2).
- **Three-judge agreement** required for any auto-promotion (Phase A5):
  - Local judge primary,
  - Frontier judge confirmation if local says "promote",
  - Held-out perplexity check (deterministic) as third leg.

Operational rules:

- Judge model IDs and prompts are version-locked. Changing either invalidates baselines (per CLAUDE.md AI Model Version Policy: re-verify available Vertex AI model IDs at every program phase).
- Judge prompts live under `eval_suites/persona/judge-prompts/` with semantic-version filenames.
- Frontier judge calls go through the existing Gemini provider (`src/providers/gemini.c`) using ADC. No raw API keys.
- Inter-rater agreement κ ≥ 0.6 between local and frontier judge required at the lock-in run; below that, the eval suite needs revision before publication.

## Consequences

- **Positive:** the public benchmark uses a frontier judge (credible), nightly cost stays bounded (cheap), and the privacy-by-architecture story holds for inner-loop development (local-only).
- **Negative:** maintenance cost — two judge prompts to keep version-aligned. Possible divergence between local and frontier opinion on edge cases; documented as the κ floor.
- **Cost projection:** ~$5/mo for anchor runs at 50 scenarios × 4 conditions × monthly = 200 frontier calls. Phase boundaries add ~$10 each. Public benchmark drop (Month 6) is a one-shot ~$40.
- **Privacy:** persona eval scenarios MUST be synthetic or explicitly user-consented. No real personal conversation history is ever shipped to the frontier judge. Phase A6.1 prep includes a privacy audit of every scenario before frontier judging.
- **Documented in:** `eval_suites/persona/README.md` (to be created in Phase A6.1) and `docs/standards/ai/evaluation.md`.

## Status

Accepted. Revisit at the Phase A6.2 lock-in run; if inter-rater κ falls below 0.6, escalate to a three-judge frontier panel (Gemini 3.1 Pro + Claude Opus + GPT) and re-evaluate cost.
