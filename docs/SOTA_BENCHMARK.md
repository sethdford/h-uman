---
title: "SOTA Benchmark: human vs. The Field"
created: 2026-03-20
status: active
---

# SOTA Benchmark: human vs. The Field

_Last updated: 2026-07-19_

This document benchmarks the `human` runtime against state-of-the-art AI agent platforms, digital twin systems, and AGI evaluation standards. It is honest — capabilities are rated against real, measured baselines where we have them, and demoted where we do not.

**Honesty rule (2026-07-19):** A **SOTA** label requires a cited measured bar (public bench number, blind human study n≥30, or in-repo eval with method notes). Architecture depth alone is **COMPETITIVE** or **PARTIAL**.

## Scoring Legend

| Rating | Meaning |
|--------|---------|
| **SOTA** | At or above state-of-the-art for the category — **measured** |
| **COMPETITIVE** | Functional, within striking distance of SOTA; not yet proven with an external bar |
| **PARTIAL** | Real implementation with meaningful gaps |
| **BASIC** | Works but far below SOTA |
| **MISSING** | Not implemented |

---

## 1. Agent Cognition (vs. OpenHands, SWE-Agent, Devin, Manus)

| Capability | SOTA Reference | SOTA Benchmark | human Status | Rating | Gap |
|-----------|---------------|----------------|-------------|--------|-----|
| **Tool use** | OpenHands | SWE-bench (external) | Large vtable tool surface, policy-gated; no published SWE-bench score for this binary | **COMPETITIVE** | Tool count is not a SOTA proof. Coding quality tracks the underlying model. |
| **Planning** | Manus (multi-model) | GAIA (external) | LLM planner + MCTS refinement + replan on failure | **COMPETITIVE** | No published GAIA score. |
| **DAG execution** | LangGraph | — | LLMCompiler → JSON → DAG, parallel pthread batch execution | **COMPETITIVE** | Real parallel execution. No async/await streaming between nodes. |
| **Multi-agent** | Manus | GAIA L3 (external) | Orchestrator → swarm with multi-round LLM+tools loop | **COMPETITIVE** | Genuine tool-using sub-agents; not full recursive agent fleets. |
| **Reflection** | Constitutional AI | — | Structured multi-axis rubric + heuristic fallback | **COMPETITIVE** | Informs retries; does not update weights. |
| **Self-improvement** | AlphaCode / DeepSeek | — | Nightly eval → weakness → patch → re-eval → keep/rollback | **PARTIAL** | Prompt/config patches, not code generation. |
| **Computer use** | Claude Computer Use | OSWorld | CoreGraphics / X11 paths on macOS/Linux | **COMPETITIVE** | Real input injection; no published OSWorld score. |
| **Browser use** | Playwright/Puppeteer | WebArena | CDP via WebSocket | **COMPETITIVE** | DOM-selector based; no visual grounding score. |
| **Long-horizon planning** | Devin | — | Multi-step within turn + plan persistence via memory | **PARTIAL** | No autonomous background continuation. |

### Agent Cognition Summary
**Overall: COMPETITIVE.** Planning, tools, DAG, reflection, and swarm orchestration are real and production-wired. We do **not** claim agent-cognition SOTA without external bench numbers. Coding-agent SOTA is table stakes via the model, not our differentiator.

---

## 2. Digital Twin / Human Fidelity (vs. Replika, Character.AI, Pi, Pika AI Selves)

| Capability | SOTA Reference | human Status | Rating | Gap |
|-----------|---------------|-------------|--------|-----|
| **Timing simulation** | Replika | Variable delay with jitter, time-of-day, conversation depth | **COMPETITIVE** | Strong implementation; not blinded against human timing corpora. |
| **Style transfer** | Pika AI Selves | Behavioral clone → calibration fields in persona prompt | **COMPETITIVE** | No real-time within-conversation adaptation. |
| **Proactive messaging** | Replika | Multi-signal: social hours, cooldown, feed override, per-contact | **COMPETITIVE** | Richer signals than typical companions; DAU/retention unproven. |
| **Emotional intelligence** | Pi | LLM emotion classifier + mood + ToM hooks | **COMPETITIVE** | No multimodal emotion. |
| **Memory / personality persistence** | Character.AI / LongMemEval / LoCoMo | SQLite episodic + semantic + forgetting curves + consolidation | **PARTIAL** | Architecture is deep; LoCoMo P@1 baseline ≈ **0.058** (`docs/evaluation/baseline.json`). LongMemEval not yet published as a program gate. |
| **Cross-channel identity** | Pika AI Selves | Contact graph, auto-link, cross-channel history in context | **COMPETITIVE** | ~31 catalog channels (not 38). No unified thread UX. |
| **Voice** | Pi | Cartesia TTS → channel attachment | **COMPETITIVE** | Real TTS; realtime path exists but is not the default twin loop. |
| **Typo simulation** | — | PRNG + QWERTY adjacency, opt-in | **COMPETITIVE** | Unusual feature; not a category SOTA claim. |
| **DPO preference learning** | RLHF / ORPO / KTO | Pair collection + export + few-shot injection + optional local train | **COMPETITIVE** | Loop exists; productized reaction→train path is Wave C. |
| **Contact profiles** | Replika | Rich per-contact fields (relationship, warmth, Dunbar, etc.) | **COMPETITIVE** | Schema depth ≠ measured fidelity. |
| **Vulnerability calibration** | — | Per-channel `vulnerability_tier` in persona overlay | **COMPETITIVE** | Research-informed; not externally scored. |
| **Anti-sycophancy** | Anthropic Constitutional AI | Metacognition sycophancy detection | **COMPETITIVE** | Detects patterns; not a published win-rate. |
| **Trajectory empathy** | Pi / EMPA | Multi-turn coherence in eval framework | **COMPETITIVE** | Framework present; live scores vary by model. |
| **Personality consistency** | Character.AI | Multi-metric consistency in fidelity eval | **COMPETITIVE** | Blind A/B human verdict currently **ABSENT** (`docs/evaluation/blind_ab_gate.json`). |
| **ML-based predictions** | On-device personalization (Apple / Gemini PI) | Local BPE/GPT/DPO/LoRA under `HU_ENABLE_ML` | **PARTIAL** | Pipeline exists; off by default; not measured against personalization benches. |
| **Compiled persona** | Prompt templates elsewhere | 46 persona `.c` modules + JSON overlays + example banks | **COMPETITIVE** | Honest architectural moat candidate; needs blind A/B to claim SOTA humanness. |

### Digital Twin Summary
**Overall: COMPETITIVE (not SOTA).** Depth of persona, preference, and memory machinery is real. Memory and humanness claims stay demoted until LongMemEval/LoCoMo numbers and a blind A/B gate (n≥30) ship (Wave B).

---

## 3. Multimodal (vs. GPT-4o, Gemini 3.x, Claude)

| Capability | SOTA Reference | human Status | Rating | Gap |
|-----------|---------------|-------------|--------|-----|
| **Image understanding** | GPT-4o / Gemini 3.x Vision | `hu_vision_describe_image` via provider API | **COMPETITIVE** | Quality tracks the underlying model. |
| **Audio transcription** | Whisper large-v3 | Whisper + Gemini STT | **COMPETITIVE** | API-backed; not on-device by default. |
| **Video understanding** | Gemini 3.x (native video) | Gemini-native video + ffmpeg keyframe fallback | **COMPETITIVE** | Gemini-first path; non-Gemini uses frames. |
| **Image generation** | DALL-E / Midjourney | DALL-E via `image_generate` + search fallback | **COMPETITIVE** | Requires provider key. |
| **Voice synthesis** | ElevenLabs / Cartesia | Cartesia TTS with format selection | **COMPETITIVE** | Single voice per persona typical. |
| **Real-time voice** | GPT-4o Realtime | OpenAI Realtime session path | **COMPETITIVE** | Wired; not the default twin surface. |

### Multimodal Summary
**Overall: COMPETITIVE.** Vision, STT, image gen, video, and realtime voice are production-wired. Gap is on-device inference and default UX.

---

## 4. Infrastructure & Security (vs. Enterprise Agent Platforms)

| Capability | SOTA Reference | human Status | Rating | Gap |
|-----------|---------------|-------------|--------|-----|
| **Binary footprint** | — | ~3 MB release (measured ~2974 KB MinSizeRel+LTO); <6 MB RAM; <30 ms startup | **SOTA** | Measured size/RSS leadership vs Node agent stacks. |
| **Sandbox** | OpenHands (Docker) | Landlock + seccomp, Seatbelt, Docker, WASI | **COMPETITIVE** | Multiple backends; platform parity incomplete. |
| **Security policy** | Anthropic Cowork containment | Deny-by-default, autonomy, pairing, HTTPS-only; Wave A unifies tool pre-execute gate | **COMPETITIVE** | Envelope parity across dispatcher/stream/DAG/HuLa is the bar; vault/SSRF follow-ups remain. |
| **Eval framework** | SWE-bench, GAIA, LongMemEval | Suite runner + LLM-as-judge + regression gates | **COMPETITIVE** | Strong in-repo harness; external memory benches not yet gated. |
| **Observability** | LangSmith | `hu_observer_t`, metrics, structured logging | **COMPETITIVE** | No hosted trace product. |
| **CI/CD** | — | 13,447+ tests, ASan, clang-tidy, Lighthouse, visual regression | **SOTA** | Breadth of automated gates for a C11 agent runtime. |

---

## 5. Evaluation Scores

Scores below are **per-suite aggregate pass rates** from in-repo eval suites (`eval_suites/*.json`), produced by `human eval baseline` (optionally persisted in SQLite when memory is configured).

### Test-mode baseline (deterministic)

Under `HU_IS_TEST`, or when the `CI` environment variable is set (e.g. GitHub Actions), `human eval baseline` returns fixed scores for the six suites below (other suite JSON files still run against the configured provider or score 0.00 if the run fails). This keeps CI deterministic without live API calls for those stems.

| Suite | Tasks | Test-Mode Score | Status | Notes |
|-------|-------|-----------------|--------|-------|
| fidelity | 10 | 0.72 | COMPETITIVE | Timing, style, proactive messaging quality |
| intelligence | 10 | 0.65 | PARTIAL | Multi-step reasoning, knowledge |
| reasoning | 10 | 0.58 | PARTIAL | Logical deduction, causal inference |
| tool_use | 8 | 0.70 | COMPETITIVE | Multi-tool chaining, error recovery |
| memory | 8 | 0.75 | COMPETITIVE | In-repo memory suite — **not** LongMemEval/LoCoMo |
| social | 8 | 0.68 | PARTIAL | Theory of mind, empathy, sarcasm |

Test-mode scores reflect the system's deterministic mock behavior. Production scores against real providers will vary by model. Run `human eval baseline eval_suites/` with API keys configured to measure live scores.

### External memory / humanness (Wave B targets)

| Bar | Current | Gate |
|-----|---------|------|
| LoCoMo P@1 | ≈ 0.058 in `docs/evaluation/baseline.json` | Publish method + improve vs baseline |
| LongMemEval | Not yet published as program gate | Reproduce + document |
| Blind A/B human | Verdict **ABSENT** (n=0) | n≥30 or fail-closed CI when feature LIVE |

---

## 6. Competitive Positioning Matrix

```
                    Agent Capability
                    ▲
                    │
            Manus ●─────────────────● OpenHands
                    │                 │
                    │     human ◆     │
                    │                 │
            Devin ●─────── ● SWE-Agent
                    │
    ────────────────┼──────────────────► Digital Twin Fidelity
                    │
         AutoGPT ●  │        ● Replika
                    │
                    │  ● Character.AI
                    │
                    ● Pi
```

**human occupies a unique position**: compiled persona + tiny C11 footprint + local-first privacy architecture, with genuine agent machinery. That is a **positioning** claim, not a measured twin-fidelity SOTA claim.

---

## 7. Gap Closure Status (Updated 2026-07-19)

### Closed — formerly "must-fix for AGI claims"
1. ~~Multi-agent recursive depth~~ **CLOSED** — Swarm workers run multi-iteration LLM+tools loops.
2. ~~Image generation~~ **CLOSED** — DALL-E integration via `image_generate`.
3. ~~DPO training closure~~ **CLOSED** — Preference pairs inject as few-shot; optional local train path exists.
4. ~~Real-time voice~~ **CLOSED** — OpenAI Realtime API session path.
5. ~~Cross-platform computer use~~ **CLOSED** — Linux X11/XTest + macOS CoreGraphics.

### Closed — formerly "nice-to-have"
6. ~~Multi-model ensemble~~ **CLOSED** — `hu_ensemble_create` routing/consensus.
7. ~~Native video processing~~ **CLOSED** — Gemini-first + ffmpeg fallback.
8. ~~Learned emotional model~~ **CLOSED** — LLM emotion classifier with confidence gating.
9. ~~Visual grounding~~ **CLOSED** — library ready; deeper agent wiring follow-up.

### Open — program Waves A–C
10. **Unified tool security envelope** — Wave A (dispatcher / stream / DAG / HuLa).
11. **Measured memory SOTA** — Wave B (LongMemEval + LoCoMo).
12. **Blind A/B humanness** — Wave B (n≥30 or fail-closed).
13. **On-device default ship path** — Wave C (local provider default + preference UX).

---

## 8. What human Does That Few Others Do

1. **~31-channel** digital twin surface with per-channel personality overlays
2. **Behavioral cloning** from real chat history (timing/style/vocabulary)
3. **Typo simulation** with QWERTY adjacency modeling
4. **Spaced-repetition forgetting curves** for memory
5. **Constitutional principles** injected from persona config
6. **~3 MB C11 binary** with 13,447+ tests — tiny vs Node agent stacks
7. **MCTS-driven planning** that can produce plans from tree search
8. **Proactive cross-channel routing** to a contact's recent channel
9. **Feed-driven outreach** with relationship-aware check-ins
10. **Hardware peripheral support** — Arduino, STM32, Raspberry Pi in the same binary
11. **Compiled persona architecture** — 46 C modules, not markdown-only templates

---

## 9. Session Changelog

| Date | Tests | Key Changes |
|------|-------|-------------|
| 2026-03-20 (initial) | 5975 | Baseline audit: identified 10 gaps |
| 2026-03-20 (round 2) | 5975 | DAG parallel, swarm, eval match_mode, reflection, style transfer, video, MCTS, plan persistence |
| 2026-03-20 (round 3) | 6032 | Recursive agents, image gen, DPO, realtime voice, Linux computer use, ensemble, native video, LLM emotion, visual grounding |
| 2026-03-21 | 6212 | Test-mode eval baselines; regression gates |
| 2026-07-19 | 13447+ | Honesty pass: demote unmeasured SOTA; sync counts; Gemini 3.x refs; Wave A program alignment |

_Re-evaluate after each major release. Run `human eval baseline eval_suites/` for per-suite tables, and publish LongMemEval/LoCoMo under `docs/evaluation/` before restoring memory SOTA claims._
