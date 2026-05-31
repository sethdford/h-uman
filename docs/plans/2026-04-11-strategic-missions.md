---
title: "Strategic Missions — The Assistant That's Actually Yours (Red-Teamed)"
created: 2026-04-11
status: active
scope: product strategy, competitive positioning, mission priorities
missions: 6
parent: CLAUDE.md (Product Thesis)
red-teamed: 2026-04-11
---

# Strategic Missions — The Assistant That's Actually Yours

> Every other AI assistant in 2026 is someone else's product renting you access.
> human is a private, personal AI that runs on your hardware, learns who you are locally,
> and never sends your identity to a cloud.

## Red Team Summary

This strategy was stress-tested across five attack vectors on 2026-04-11. Key findings that shaped revisions:

| Attack Vector | Finding | Impact on Strategy |
|--------------|---------|-------------------|
| **Market** | Privacy paradox: 81% care, 8-12% configure settings. AI apps churn 30% faster (21.1% annual retention). BUT companion AI retains 3x better than utility AI (41% vs 14% DAU/MAU). | Privacy must be architectural default, not a feature. Persona/companion angle validated for retention. |
| **Gemini threat** | Google launched "Personal Intelligence" — Gmail/Photos/YouTube personalization. "Import Memory" poaches users from other AIs. | "We personalize" is NOT a differentiator. "Where your data lives" IS. |
| **OpenClaw threat** | Persona plugins exist: SOUL.md, personality-dynamics (auto-evolution), open-persona. 6.2K stars. | Persona existence is not unique. Compiled depth (27 C modules vs markdown templates) is the real moat. |
| **Technical M1-M3** | `HU_HAS_PERSONA` is repo-wide (daemon, gateway, tools, headers). LoRA trains reference GPT, not chat model. `--checkpoint` is `(void)`. Fact extraction is brittle pattern matching. | Timelines doubled. M3 narrative rewritten to match code reality. |
| **Retention data** | 4 of 5 AI app subscribers cancel within a year. Novelty exhaustion is #1 killer. High-retention companion apps lose money on compute. | Must solve unit economics early. Avoid unlimited-chat cost trap. |

## Context (April 2026)

The AI assistant landscape has converged. Gemini Agent browses the web and manages your Gmail for $250/mo. Claude Cowork controls your Mac and coordinates agent teams. OpenClaw has 100K+ GitHub stars and a thriving ecosystem. Apple paid Google $1B to make Siri not embarrassing.

**What they all share:** task execution, multi-step planning, tool use, chat interfaces, and now *basic personalization*. Gemini connects to your Google apps. OpenClaw has SOUL.md persona plugins. These are converging toward commodity.

**What none of them offer:** an AI where personalization is a *structural property of the architecture* — not a cloud feature you opt into, not a markdown template, but compiled behavior that runs locally and never leaves your device. The gap human fills is *trust through architecture*, not trust through promises.

## The Six Missions

### M1: Persona-First ✅ PHASE 1 COMPLETE

**Goal:** Make persona the default experience, not an optional compile flag.

**Status (2026-04-11):** Phases 1.1–1.4 DONE. Persona fields are unconditional in `hu_agent_t`. 100+ `#ifdef HU_HAS_PERSONA` guards removed across agent.c, agent_turn.c, agent_stream.c, daemon.c, conversation.c, humanness.c, daemon_proactive.c, tools/factory.c, gateway, doctor.c, voice_clone.c, and 9 test files. Only 2 guards remain (minimal-build fallback in main.c and fuzz harness). `human init` now creates a starter persona with Tier 1 channel overlays. 9,063/9,063 tests passing.

**What changed:**
- `include/human/agent.h`: persona, relationship, voice_profile fields always present (no `#ifdef`)
- `include/human/context/conversation.h`: always includes persona.h
- `include/human/daemon.h`: proactive check-ins always declared
- `src/agent/agent.c`, `agent_turn.c`, `agent_stream.c`: all persona code unconditional
- `src/daemon.c`: 100+ guards removed; all persona behavior runtime NULL-safe
- `src/context/conversation.c`: disfluency, context modifiers always compiled
- `src/tools/factory.c`: persona tool always registered
- `src/gateway/cp_admin.c`: persona.set always available
- `src/app/cli_commands.c`: `human init` creates config + starter persona with channel overlays
- Tests: guards removed from test_main, test_e2e, test_conversation, test_bth_e2e, etc.

**Remaining phases:**

| Phase | What | Deliverable | Timeline |
|-------|------|-------------|----------|
| ~~1.1~~ | ~~Audit all `HU_HAS_PERSONA` sites~~ | ~~Catalog~~ | ✅ Done |
| ~~1.2~~ | ~~Runtime persona, not compile-time~~ | ~~ABI refactor~~ | ✅ Done |
| ~~1.3~~ | ~~Starter persona generation~~ | ~~`human init` creates default.json~~ | ✅ Done |
| ~~1.4~~ | ~~Prompt cache compatibility~~ | ~~Verified: persona prompt is per-channel by design~~ | ✅ Done |
| 1.5 | **Zero-config onboarding** | First-run CLI flow: "Tell me about yourself" → generate persona. 5 questions, 2 minutes. Optional — starter persona works without it. | Next sprint |
| 1.6 | **A/B validation** | Blind A/B between persona-on and persona-off across 50 test conversations. Target: 80% preference for persona-on. | After 1.5 |

**Key files:** `src/persona/`, `include/human/persona.h`, `src/agent/agent_turn.c`, `src/config/config.c`, `src/config/config_merge.c`, `src/daemon.c`, `CMakeLists.txt`

**Risk (updated):** Reduced from High to Medium. ABI change is done. Remaining risk: minimal/embedded builds with `HU_ENABLE_PERSONA=OFF` need testing (persona sources not linked, but header types still available).

**Success metric:** ~~Persona context in every agent turn~~ ACHIEVED. Starter persona auto-created on first run ✅. Prompt cache behavior verified ✅.

---

### M2: Personal Model

**Goal:** Evolve memory from retrieval (RAG) into a persistent model-of-the-person.

**Current state:** Memory stack includes episodic, temporal, semantic, knowledge graph, inside jokes, commitments, forgetting curves, life chapters, deep memory — all with SQLite persistence. This is strong retrieval infrastructure. But it retrieves *facts about conversations*, not a *model of the person*.

**Why it matters (red-teamed):** Gemini's "Personal Intelligence" already connects Gmail/Photos/YouTube for personalization. Google even lets users *import memory from other AIs*. We cannot compete on data breadth. We compete on data *depth* and *trust*: a model-of-the-person that lives locally, learns from every interaction, and belongs to the user — not to a cloud provider that trains on it.

**Red team findings:**
- **No unified personal model exists.** There are parallel mechanisms (graph, episodic, emotional residue, consolidation, QMD, etc.) but no single `hu_personal_model_t` with defined training/inference contracts.
- **Fact extraction is brittle.** `src/memory/fact_extract.c` uses pattern matching: `{"i like ", HU_KNOWLEDGE_PROPOSITIONAL}`, `{"i never ", HU_KNOWLEDGE_PRESCRIPTIVE}`. Cannot handle negation, implicit preferences, multi-turn inference, or non-English input.
- **`user_preferences` is a 1024-char bounded string field** in the tier core snapshot (`src/memory/tiers.c`). More telemetry than model.
- **No end-to-end evaluation metric** for "did the personal model improve the right thing?"
- **Risk of creepy behavior** if implicit inference overshoots user expectations.

**Plan (revised):**

| Phase | What | Deliverable | Timeline |
|-------|------|-------------|----------|
| ~~2.1~~ | ~~**Define `hu_personal_model_t`**~~ | ~~New struct~~ | ✅ Done — `include/human/memory/personal_model.h` + `src/memory/personal_model.c`. Topics, style, goals, temporal patterns, facts. Wired into agent turn (ingests every user message). 6 tests passing. |
| 2.2 | **LLM-based preference extraction** | Replace brittle pattern matching with LLM extraction (small model, local if possible). After each conversation, extract preference signals via structured output. Fallback to pattern matching if LLM unavailable. | Week 3-6 |
| 2.3 | **Behavioral pattern aggregation** | Aggregate signals into patterns with confidence scores and decay. "Prefers concise before 9am" (confidence: 0.7, decay: 0.95/week). Require minimum observation count before activating. | Week 6-9 |
| 2.4 | **Adaptive prompting** | Persona prompt builder incorporates personal model signals. Conservative threshold: only adapt when confidence > 0.6 and observations > 10. User can see and correct inferences. | Week 9-12 |
| 2.5 | **Evaluation framework** | End-to-end metric: compare adapted vs static persona responses in blind eval. Measure: persona fidelity, user satisfaction proxy (engagement length, conversation continuation rate). | Week 10-14 |
| 2.6 | **Correction UX** | User can view, edit, and delete any inferred preference. "human preferences show" / "human preferences edit". Transparency is the trust mechanism. | Week 12-16 |

**Key files:** `src/memory/`, `src/memory/fact_extract.c`, `include/human/memory.h`, `src/persona/persona.c`, `src/agent/agent_turn.c`

**Depends on:** M1 (persona must be default for personal model to have context to adapt)

**Risk:** Very High. This is the core privacy promise and the core trust mechanism. Wrong inferences are worse than no inferences. Creepy behavior destroys trust faster than good behavior builds it. Every inference must be inspectable and correctable.

**Success metric:** After 50 conversations, the agent adapts tone/timing without explicit persona config changes. User can view all inferences. Measurable improvement in blind eval. Zero false-certainty hallucinations about user preferences.

---

### M3: Private Learning

**Goal:** On-device ML becomes the personalization engine.

**Current state:** CPU-only training pipeline: BPE tokenizer, GPT forward/backward, MuonAdamW optimizer, DPO, LoRA, experiment loop. All behind `HU_ENABLE_ML`. `src/ml/CLAUDE.md` notes "ggml integration planned for Phase 2+."

**Why it matters (red-teamed):** "Your data never leaves your device" is the anti-Gemini pitch. But this mission has the **largest gap between narrative and code reality** of any mission.

**Red team findings (critical):**
- **`lora-persona` does NOT fine-tune the model users chat with.** It creates a fresh `hu_gpt_create` from `hu_experiment_config_default()` and trains LoRA adapters against that toy reference GPT — not Gemini, not OpenAI, not any frontier model.
- **`--checkpoint` is accepted but explicitly discarded:** `(void)checkpoint_path;` in `src/ml/cli.c` line 532.
- **Training requires example banks, not live conversations.** `src/ml/cli.c` line 521: `if (total_examples == 0) { ... "has no example banks to train on" ... }`. There is no pipeline from conversation history to training data in the LoRA path.
- **CPU-only training** on a reference GPT. No ggml. No MLX. Training speed: 3-5 iterations/sec on M2 (vs 30+ on A100).
- **CLI test coverage for `lora-persona` is shallow** — only help and missing-arg tests. No golden-path training integration test.
- **Tokenizer alignment gap:** BPE trained here has no relationship to the tokenizer of the model users actually chat with.

**Honest assessment:** The ML subsystem is a credible research artifact for training small language models from scratch. It is **not** a personalization engine for frontier model interactions. The narrative "LoRA fine-tunes your assistant" is false as coded today.

**Revised plan (two tracks):**

**Track A: Prompt-based personalization (achievable now, no ML)**

| Phase | What | Deliverable | Timeline |
|-------|------|-------------|----------|
| ~~3A.1~~ | ~~**Personal model → prompt**~~ | ~~Inject into system prompt~~ | ✅ Done — `hu_personal_model_build_prompt()` renders `[Personal Context]` block. Personal model ingested on every user turn via `hu_personal_model_ingest()`. |
| ~~3A.2~~ | ~~**Few-shot persona examples**~~ | ~~Inject as few-shot examples in prompt~~ | ✅ Done — Starter persona includes CLI example bank (3 examples). Inline `example_banks` parsing added to `hu_persona_load_json`. Prompt builder already wired via `hu_persona_select_examples()`. |
| 3A.3 | **Measure improvement** | Blind eval: prompt-personalized responses vs generic. Target: measurable preference. | Week 3-5 |

**Track B: On-device model personalization (hard, long-term)**

| Phase | What | Deliverable | Timeline |
|-------|------|-------------|----------|
| 3B.1 | **MLX/ggml LoRA integration** | Replace reference GPT training with LoRA on a real small model (Llama 3.2 1B/3B via MLX on Apple Silicon, ggml on Linux). Requires new inference path. | Quarter 1-2 |
| 3B.2 | **Conversation → training pipeline** | Automatic preparation of conversation history into LoRA training data. Connect `prepare-conversations` output to actual LoRA training input. Fix `--checkpoint` to actually load base model. | Quarter 1-2 |
| 3B.3 | **Background training loop** | Periodic background job: prepare data, run LoRA fine-tune (time-budgeted), save adapter. Guard against CPU thrashing on low-power devices. | Quarter 2 |
| 3B.4 | **Adapter-augmented inference** | Small local model handles persona-sensitive tasks (tone, timing, proactive suggestions). Frontier model handles complex reasoning. Router decides which model answers which query. | Quarter 2-3 |
| 3B.5 | **DPO from implicit feedback** | Collect preference pairs from engagement signals. Train with DPO. Requires CUDA or MLX DPO support (currently CUDA-only in production quality). | Quarter 3+ |

**Key files:** `src/ml/`, `src/ml/cli.c`, `src/providers/huml.c`, `src/providers/embedded.c`

**Depends on:** M1 (persona default), M2 (preference extraction provides training signal)

**Risk:** Very High for Track B. Track A is medium risk and delivers immediate value. The industry consensus (April 2026): MLX LoRA works for 1B-7B models on Apple Silicon. DPO requires CUDA for production quality. ggml training is not yet mature. On-device personalization is proven in research; production deployment on consumer hardware is still frontier.

**Success metric:**
- Track A (Q2 2026): Prompt-personalized responses measurably preferred over generic in blind eval.
- Track B (Q4 2026+): LoRA-adapted local model measurably improves persona fidelity on held-out conversation test set. User can always roll back to base.

---

### M4: Ship to Users

**Goal:** Get real humans using human daily.

**Current state:** 290K lines of C, 8500+ tests, 31 channels, 87 tools, 50+ providers — zero daily active users. The quality scorecard claims 70/70 on web surfaces. No one is using the product.

**Why it matters (red-teamed):** AI apps churn 30% faster than non-AI apps (RevenueCat 2026). 4 of 5 subscribers cancel within a year. Novelty exhaustion is the #1 killer. BUT: companion AI retains 3x better than utility AI. The persona thesis predicts better retention — but only real users will prove or disprove it. Every week without users is a week we're guessing.

**Red team findings:**
- **First run drops to defaults with a log line.** `config_merge.c` line 498-507: missing config → `hu_log_info("config", NULL, "no config found at %s, using defaults", global_path)`. No guided onboarding.
- **Defaults assume cloud provider credentials.** `config_merge.c` line 83-101: default provider is `gemini`, default model is `gemini-3.1-flash-lite-preview`, memory is `sqlite`. User must configure API credentials manually.
- **Persona defaults are NULL.** `config_merge.c` lines 189-192: `cfg->agent.default_profile = NULL; cfg->agent.persona = NULL;`. No auto-creation.
- **Unit economics warning:** High-retention companion apps lose money on compute (Dot shut down, Inflection pivoted). Avoid the unlimited-chat cost trap. Need usage-aware pricing or cost controls from day 1.

**Plan (revised):**

| Phase | What | Deliverable | Timeline |
|-------|------|-------------|----------|
| ~~4.1~~ | ~~**`human init` command**~~ | ~~Interactive first-run~~ | ✅ Done — `human init` creates config + starter persona with channel overlays + example bank. `human onboard` also creates persona. Provider auto-detected from env vars. |
| 4.2 | **Define the "day 2" hook** | Pick ONE channel (CLI) and ONE use case (daily briefing + proactive task follow-up). The "wow" is not the first response — it's the proactive message the next morning that proves the assistant remembers yesterday. Requires M1 (persona default) and basic memory. | Week 2-4 |
| 4.3 | **Cost controls** | Per-session and per-day token budgets. Background job cost accounting. Prevent runaway API costs for alpha users. Configurable but conservative defaults. | Week 3-5 |
| 4.4 | **Closed alpha (10 users)** | Recruit 10 people. Provide a feedback channel. Collect: setup friction, conversation quality, persona accuracy, crashes, API costs, day-2 return rate. | Week 5-8 |
| 4.5 | **Iterate on feedback** | Fix top 5 friction points. Re-measure. | Week 8-10 |
| 4.6 | **Open beta (100 users)** | Public beta. Track DAU, conversations/day, retention at day 7 and day 30, API cost per user per day. | Week 10-14 |

**Key files:** `src/app/main.c`, `src/config/config.c`, `src/config/config_merge.c`, `website/`, `README.md`

**Depends on:** M1 (persona must be default for the "wow" moment — a generic chatbot won't retain)

**Risk:** High. The biggest risk is that first experience is mediocre because persona/memory isn't mature enough. Second biggest risk is unit economics — if each active user costs $5/day in API calls with no revenue, 100 DAU = $15K/month burn.

**Success metric:** 100 DAU with 30% day-7 retention. API cost per user under $1/day average.

---

### M5: HuLa as Platform

**Goal:** Expose HuLa as a developer-facing tool orchestration SDK.

**Current state:** HuLa has 8 opcodes (call, seq, par, branch, loop, delegate, emit, noop), a JSON IR spec, compiler from LLM chat, emergence/skill promotion, and execution with traces. It's the most sophisticated tool orchestration system in any open-source agent — but only accessible through the internal agent loop.

**Why it matters:** LangGraph has 25K+ GitHub stars because developers need structured tool orchestration. HuLa is architecturally superior — typed, compiled, with emergence and DAG execution — but invisible. Exposing it creates a developer ecosystem and positions human as infrastructure, not just an end-user product.

**Plan:**

| Phase | What | Deliverable | Timeline |
|-------|------|-------------|----------|
| ~~5.1~~ | ~~**HuLa SDK header**~~ | ~~Public SDK API~~ | ✅ Done — `include/human/hula_sdk.h` with version macros, ergonomic helpers (`hu_hula_sdk_call`, `hu_hula_sdk_sequence`, `hu_hula_sdk_run_json`), comprehensive documentation. 79 HuLa tests passing. |
| 5.2 | **HuLa specification doc** | Public-facing spec document (from existing internal spec). Examples, tutorials, comparison with LangGraph/CrewAI. Publish on website. | Week 2-4 |
| 5.3 | **HuLa gateway endpoint** | HTTP API: POST a HuLa program, get execution trace back. Enables non-C clients to use HuLa. | Week 4-6 |
| 5.4 | **HuLa playground** | Web UI for writing, visualizing, and executing HuLa programs. DAG visualization of execution traces. Part of the dashboard. | Week 6-10 |
| 5.5 | **Ecosystem seeding** | Write 10 HuLa programs for common workflows (research, code review, data processing, email triage, etc.). Publish as examples. | Week 4-8 |

**Key files:** `src/agent/hula.c`, `src/agent/hula_compiler.c`, `src/agent/hula_emergence.c`, `src/tools/`

**Risk:** Medium. The value proposition depends on HuLa being demonstrably better than prompt-chaining for developers. Need clear "before/after" examples showing reliability, debugging, and emergence advantages.

**Success metric:** External developers write and run HuLa programs. 500+ GitHub stars on a HuLa examples repo within 3 months of launch.

---

### M6: Channel Focus

**Goal:** Excel on 5 channels instead of spreading thin across 31.

**Current state:** 31 channels implemented. CLI, iMessage, Telegram, Discord have the most test coverage. Many channels share a broad test bucket without dedicated test files. Some are send-only (MaixCam, Voice in catalog).

**Why it matters:** Depth beats breadth. One channel where persona *shines* — where the timing feels right, where the humor lands, where the proactive messages are welcome — is worth more than 31 channels that merely relay text.

**Plan:**

| Phase | What | Deliverable | Timeline |
|-------|------|-------------|----------|
| ~~6.1~~ | ~~**Tier channels**~~ | ~~Tier 1/2/3 classification~~ | ✅ Done — `docs/plans/2026-04-11-channel-tiers.md`. Tier 1: Telegram, Discord, iMessage, Slack. Tier 2: CLI, Web, WhatsApp, Signal, Matrix + 13 more. Tier 3: 17 community/experimental. |
| 6.2 | **Persona-per-channel audit** | For each Tier 1 channel: verify persona overlays work, timing/jitter feels natural, proactive messages are channel-appropriate, rich content (reactions, attachments, typing indicators) is wired. File per-channel gap reports. | Week 1-3 |
| 6.3 | **Channel-specific persona tuning** | iMessage: tapbacks, read receipts, typing bubbles, reaction timing. Discord: thread awareness, emoji reactions, server context. Telegram: inline keyboards, reply threading. CLI: prompt flow, formatting, progress indicators. Web: real-time, rich media. | Week 3-6 |
| 6.4 | **Channel excellence eval** | Create per-channel eval suites: 20 conversations per Tier 1 channel, scored on naturalness, persona consistency, timing appropriateness, feature utilization. Target: 8/10 average across all Tier 1 channels. | Week 4-8 |

**Key files:** `src/channels/`, `src/channels/channel_catalog.c`, `src/persona/persona.c` (overlays)

**Risk:** Low-medium. This is primarily prioritization and polish, not new architecture. Risk is in under-investing in Tier 2/3 channels that specific users depend on.

**Success metric:** Tier 1 channels score 8/10+ on naturalness eval. Tier 2/3 channels continue to pass existing tests.

---

## Mission Dependencies (Revised)

```
M1 (Persona-First) ──────┬──→ M4 (Ship to Users)
      10 weeks            │        14 weeks
                          │
M6 (Channel Focus) ───────┘
      8 weeks (supports M1, M4)

M2 (Personal Model) ──→ M3 Track A (Prompt personalization)
      16 weeks               concurrent with M2

M3 Track B (On-device ML) ← long-term, quarters
M5 (HuLa as Platform) ← independent, quarters
```

**Critical path:** M1 + M6 → M4. Nothing else matters if persona isn't default and users aren't trying it.

**Immediate value:** M3 Track A (prompt-based personalization) delivers persona improvement without ML infrastructure. Start concurrent with M2.

**Long-term bets:** M3 Track B (on-device LoRA) and M5 (HuLa platform) are quarter-scale initiatives. Don't sequence them against the critical path.

## Quarterly Milestones (Red-Teamed)

| Quarter | Milestone | Missions | Honest Risk |
|---------|-----------|----------|-------------|
| **Q2 2026** (now) | ✅ M1 Phase 1 DONE (100+ ifdef removed, ABI unified). ✅ M4.1 DONE (init + onboard create personas, provider auto-detect). ✅ M6.1 DONE (channel tiering). ✅ M2.1 DONE (personal model struct + 52 fact patterns + agent wiring). ✅ M3A.1-2 DONE (prompt personalization + few-shot examples). ✅ M5.1 DONE (HuLa SDK header). **Remaining Q2:** 10-user alpha, M1.5-1.6 onboarding wizard + A/B, M6.2-6.3 channel tuning. | M1-M6 all started | Risk reduced. Alpha should be achievable by end of Q2. |
| **Q3 2026** | Persona always-on (compiled-in default). Zero-config onboarding. Personal model v1 (LLM extraction). Prompt-based personalization (Track 3A). 100 DAU beta. Channel persona tuning. | M1.5-1.6, M2.1-2.4, M3A, M4.5-4.6, M6.3-6.4 | Personal model extraction quality is the big unknown. May need multiple LLM iterations. |
| **Q4 2026** | Personal model with correction UX. Evaluation framework. HuLa CLI + spec (if bandwidth). MLX/ggml LoRA proof of concept. | M2.5-2.6, M5.1-5.2, M3B.1 | ggml/MLX training maturity is uncertain. HuLa platform may deprioritize if user growth demands focus. |
| **Q1 2027** | On-device LoRA training loop (Apple Silicon). Adapter-augmented inference. 500+ DAU. HuLa gateway. | M3B.2-3B.4, M5.3, M4 (scale) | On-device training quality is the hardest unsolved problem. May need to settle for prompt-only personalization. |

## What Happens If the Thesis Is Wrong

The red team must answer: what if "the assistant that's actually yours" isn't what people want?

**Fallback thesis:** If privacy-by-architecture and deep persona don't drive retention, human still has:
- The smallest, fastest, most portable agent runtime (developer/IoT audience)
- HuLa as a typed tool orchestration IR (developer tooling audience)
- 31 channels + 87 tools + 50+ providers in a single binary (infrastructure audience)

These are **infrastructure plays**, not consumer plays. The pivot would be: human as the *engine* other products build on, not a product itself. Similar to how SQLite is everywhere but has no "users" — it has embedders.

**Signal to watch:** If alpha users consistently say "I just want it to do tasks" and don't engage with persona features, the consumer thesis is wrong and the infrastructure thesis is the right play.

## How to Use This Document

Every PR, every feature, every refactor should reference which mission it serves. If a change doesn't serve M1-M6, ask: "Is this moving us toward *the assistant that's actually yours*, or is it engineering for engineering's sake?"

The red team findings are in `CLAUDE.md` (Product Thesis section) and in this document's mission details. **Do not remove the red team notes** — they are the intellectual honesty that prevents us from building the wrong thing.

This document is the strategic layer. `CLAUDE.md` has the thesis and mission table for quick reference.
