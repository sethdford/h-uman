---
title: "Project Digital Twin — AGI-Grade Seth Across Every Channel"
created: 2026-03-20
status: closed
scope: channel parity, daemon generalization, AGI cognition, computer use, multimodal, voice, eval, behavioral calibration, cross-channel identity
phases: 5
parent: 2026-03-10-human-fidelity-design.md, 2026-03-15-sota-agi-convergence.md
last_audit: 2026-05-25
---

# Project Digital Twin — AGI-Grade Seth Across Every Channel

> Not "AI that texts on iMessage" — Seth, everywhere, better than Seth could be.

## Vision

A single persona — Seth's digital twin — that operates across iMessage, Discord, Telegram, Slack, WhatsApp, Signal, and any future channel with:

1. **Indistinguishable fidelity** — timing, reactions, typing, voice, humor, emotional intelligence calibrated from real behavior
2. **AGI-grade cognition** — tree search, multi-agent decomposition, computer/browser use, causal reasoning, self-improvement loops
3. **Superhuman capabilities** — perfect memory across channels, proactive awareness from feeds/news/social, skill acquisition, behavioral cloning from real conversations
4. **Unified identity** — same person whether you text, DM, or email; shared context, shared memory, coherent personality

## Current State (Honest Assessment)

### What's strong

- **Memory stack**: episodic, temporal, semantic, knowledge graph, inside jokes, commitments, forgetting curves, life chapters, deep memory — all implemented with SQLite persistence
- **Persona system**: overlays per channel, example banks, contact profiles, proactive channel routing — architecturally multi-channel
- **Conversation intelligence**: energy detection, escalation, vulnerability, tapback decisions, response classification, message splitting, fillers — channel-agnostic in `conversation.c`
- **Daemon orchestration**: timing, delays, jitter, late-night stretching, leave-on-read, backchannels, proactive outbound — the "human feel" engine
- **Security**: policy engine, sandboxing (landlock/firejail/bwrap/docker/firecracker), input guards, audit, autonomy levels
- **85 tools**, 38 channels, 50+ providers, ToT/beam search, eval framework
- **Planner + cognition wiring**: MCTS path in `planner.c` / `agent_turn.c`, LLMCompiler + DAG executor in the hot path, `browser_use` + macOS `computer_use`, hierarchical compaction, multimodal **audio** + **video** pipelines (POSIX video uses ffmpeg)
- **Channel parity (in progress)**: Discord, Telegram, Slack, WhatsApp, and Signal expose **`react`**; cross-channel context and **`contact_graph`** resolution are wired in the daemon; per-channel daemon config + voice gating generalized beyond iMessage
- **Evaluation + calibration**: JSON suites under `eval_suites/` (fidelity, reasoning, tool_use, memory, social, etc.), calibration module (`human calibrate`), A/B compare, eval dashboard trend API
- **Scale-out**: Swarm parallel execution + aggregation strategies, ensemble provider, fleet limits / spawn governance in the agent pool
- **Realtime transport**: WebSocket client supports **custom headers** (e.g. OpenAI Realtime `Authorization` + beta headers via `hu_ws_connect_with_headers`)

### What's weak

- **Channel parity**: Major chat platforms gained reactions, history hooks, and constraints, but **iMessage still leads** on attachment paths, tapback/read-receipt context, and some daemon affordances. Remaining work is **narrowing capability gaps**, not greenfield vtable work.
- **AGI cognition**: **Computer use** is production-grade on **macOS** only (CoreGraphics); other desktops are limited or unsupported. **WebRTC** remains **signaling-first**, not a full managed media stack.
- **Evaluation**: Suites and harness exist, but **pass rates are not yet a enforced product gate**; continuous benchmarking and “regression as CI” are still maturing.
- **Behavioral calibration**: Pipelines exist; **ground truth linkage** (Seth-specific distributions, automated enforcement of rubrics) still needs operational discipline.
- **Cross-channel identity**: **`contact_graph`** and resolution exist; **coverage and confidence** (manual vs inferred links, drift) remain the hard part.

---

## Phase 1: Channel Generalization (The Foundation)

> Make every channel as capable as iMessage. Without this, "digital twin everywhere" is impossible.

**Timeline**: 2–3 weeks
**Risk**: Medium (refactoring daemon.c, touching vtable interfaces)
**Dependencies**: None — unblocks everything else

### 1.1 Daemon De-iMessageification

**Status — mostly done; polish remains.** Many former iMessage-only paths now use shared helpers, `hu_channel_daemon_config_t`, and vtable hooks; a small number of channel-specific branches may still exist for Apple-only features.

Eliminate all 17 hard-coded `strcmp(chn, "imessage")` forks in `src/daemon.c`. Replace with vtable capabilities and per-channel config.

**Files:**

- Modify: `src/daemon.c` — replace all iMessage forks with generic paths
- Modify: `include/human/channel.h` — add optional vtable hooks
- Modify: `include/human/config.h` — generalize `response_mode` and `user_response_window_sec`
- Modify: `src/config_parse_channels.c` — parse generalized config
- Add tests for each generalized path

**Specific refactors (by daemon.c line):**

| Current fork | Replacement |
|---|---|
| **Vision/attachments** (2507, 5213): `hu_imessage_get_attachment_path` | New vtable: `get_attachment_path(ctx, alloc, message_id)` — iMessage implements via chat.db, others via API |
| **Response mode** (2684): `config->channels.imessage.response_mode` | Move to `hu_channels_config_t` top-level or per-channel generic struct: `config->channels.response_mode` with per-channel override |
| **Human takeover** (3183, 7347): `hu_imessage_user_responded_recently` | New vtable: `human_active_recently(ctx, contact, contact_len, window_sec)` — iMessage checks chat.db; others return false initially |
| **Tapback context** (4726): `hu_imessage_build_tapback_context` (unconditional!) | Guard with channel name or new vtable: `build_reaction_context(ctx, alloc, contact, len, out, out_len)` |
| **Read receipts** (4757): `hu_imessage_build_read_receipt_context` | Same pattern — vtable or channel-capability flag |
| **Inline reply** (7318): text quote fallback | Check `ch->channel->vtable->supports_threading` flag; apply text-quote for channels without native threads |
| **Voice** (7370): Cartesia gate | Check `persona->voice_messages.enabled` + channel supports audio media (new vtable flag or send with media) |
| **Default channel** (7231): fallback `"imessage"` | Use `"unknown"` or require explicit active channel |
| **Prompt text** (7603): `"iMessage"` literal | Interpolate `ch_name` |
| **Log text** (7493+): `"imessage effect"` | Use `ch_name` in log |
| **DPO path** (2352): `data/imessage/` | Use `data/<channel_name>/` or neutral path |
| **skip_send label** (7634): `HU_HAS_IMESSAGE` guard | Make unconditional |

### 1.2 Channel Vtable Enrichment

**Status — done** for Discord, Telegram, Slack, WhatsApp, and Signal (including `react`, history, and constraints where the platform allows).

Fill the gaps in the top 5 non-iMessage channels:

| Vtable member | Discord | Telegram | Slack | WhatsApp | Signal |
|---|---|---|---|---|---|
| `react` | Add (emoji reaction API) | Add (emoji reaction API) | Add (emoji reaction API) | Add (emoji reaction API) | Skip (limited API) |
| `start_typing` | Add (POST /channels/{id}/typing) | Already done | Already done | Skip (no API) | Already done |
| `load_conversation_history` | Add (GET /channels/{id}/messages) | Add (getUpdates history) | Add (conversations.history) | Skip (no server-side API) | Skip (local DB possible) |
| `get_response_constraints` | Add (2000 char limit) | Add (4096 char limit) | Add (40000 char limit) | Add (65536 char limit) | Add (no hard limit) |
| `get_attachment_path` (NEW) | Add (CDN URL from attachment object) | Add (getFile API) | Add (files.info API) | Skip | Skip |

**Files per channel:**

- `src/channels/discord.c` — add `react`, `start_typing`, `load_conversation_history`, `get_response_constraints`, `get_attachment_path`
- `src/channels/telegram.c` — add `react`, `load_conversation_history`, `get_response_constraints`, `get_attachment_path`
- `src/channels/slack.c` — add `react`, `load_conversation_history`, `get_response_constraints`, `get_attachment_path`
- `src/channels/whatsapp.c` — add `react`, `get_response_constraints`
- `src/channels/signal.c` — add `get_response_constraints`

### 1.3 Per-Channel Config Generalization

**Status — done** (`hu_channel_daemon_config_t` embedded per channel; daemon reads active channel config).

**Files:**

- Modify: `include/human/config.h` — add `hu_channel_daemon_config_t` shared struct
- Modify: `src/config_parse_channels.c` — parse shared fields per channel
- Modify: `src/daemon.c` — read from channel-specific daemon config

```c
/* New shared struct embedded in each channel config */
typedef struct hu_channel_daemon_config {
    char *response_mode;          /* "selective", "normal", "eager" */
    int user_response_window_sec; /* 0 = default */
    int poll_interval_sec;        /* 0 = default */
    bool voice_enabled;           /* enable TTS on this channel */
} hu_channel_daemon_config_t;
```

Embed in `hu_imessage_channel_config_t`, `hu_discord_channel_config_t`, etc. Daemon reads from active channel's config, falling back to iMessage config for backward compat.

### 1.4 Message Splitting Respects Constraints

**Status — done** (splitter consumes `get_response_constraints()` / channel limits).

**Files:**

- Modify: `src/context/conversation.c` — `hu_conversation_split_response` accepts `max_chars` parameter
- Modify: `src/daemon.c` — pass `get_response_constraints()` result to splitter

Currently `hu_conversation_split_response` uses a fixed ~300-char algorithm. After this change, Discord splits at 2000, Telegram at 4096, iMessage at ~300, Slack rarely splits.

### 1.5 Per-Channel Outbound Formatting

**Status — partial** (verify coverage for every outbound path and channel-specific markdown/HTML rules).

**Files:**

- Create: `src/channels/format.c` — `hu_channel_format_outbound(alloc, channel_name, text, len, out, out_len)`
- Modify: `src/daemon.c` — call formatter before send

| Channel | Formatting |
|---|---|
| iMessage | Strip markdown, remove AI phrases, 300-char bubble cap |
| Discord | Keep markdown (Discord renders it natively) |
| Telegram | Keep markdown (Telegram supports it) |
| Slack | Convert to mrkdwn (Slack's variant) |
| Email | Convert to HTML |
| CLI | Keep as-is |

---

## Phase 2: AGI Cognition (The Brain)

> Wire the intelligence systems that already exist but aren't connected, then build what's missing.

**Timeline**: 3–4 weeks
**Risk**: High (agent loop changes, new tool implementations)
**Dependencies**: Phase 1 (eval needs channel-general infrastructure to test against)

### 2.1 Wire MCTS into Planner

**Status — done** (`hu_planner_plan_mcts` + routing from `agent_turn.c` when plans merit MCTS).

`src/agent/mcts_planner.c` provides UCB1 / beam search; the planner exposes `hu_planner_plan_mcts` and the agent loop invokes it when appropriate.

**Files:**

- Modify: `src/agent/planner.c` — add `hu_planner_plan_mcts` path when complexity > threshold
- Modify: `src/agent/agent_turn.c` — route to MCTS planner for multi-step tasks
- Modify: `include/human/agent/planner.h` — expose MCTS entry point

**Trigger**: When the LLM's initial plan has 5+ steps with dependencies, or when the first attempt fails and replanning is triggered.

### 2.2 Wire LLMCompiler into Agent Loop

**Status — done** (enabled path when `llm_compiler_enabled` and tool-call volume crosses threshold).

`src/agent/llm_compiler.c` compiles natural language to DAGs; `src/agent/dag_executor.c` executes them from `agent_turn.c` when the compiler path wins.

**Files:**

- Modify: `src/agent/agent_turn.c` — when `config->llm_compiler_enabled` and tool call count >= 3, compile to DAG and execute via `dag_executor` instead of sequential dispatch

### 2.3 Computer Use Tool (Real Implementation)

**Status — partial** — **macOS**: CoreGraphics screenshot + input (see `src/tools/computer_use.c`). **Other OSes**: not production-complete; tests use `HU_IS_TEST` mocks.

**Files:**

- Modify: `src/tools/computer_use.c` — screenshot, click, type, scroll via platform APIs
- macOS: `CGWindowListCreateImage` for screenshots, `CGEventCreateMouseEvent` / `CGEventCreateKeyboardEvent` for input
- Guard with `HU_IS_TEST` for mock paths
- Policy: require SUPERVISED or higher autonomy level

### 2.4 Browser Use Tool (New)

**Status — done** (`src/tools/browser_use.c`, CDP over WebSocket to Chrome; registered in `factory.c`; tests behind `HU_IS_TEST`).

**Files:**

- `src/tools/browser_use.c` — navigate, click, type, extract via CDP (Chrome DevTools Protocol)
- `include/human/tools/browser_use.h`
- `tests/test_browser_use.c`
- `src/tools/factory.c` — registration

CDP over WebSocket to a local Chrome instance. Actions: navigate, screenshot, click (by selector), type, extract text, execute JS. All behind `HU_IS_TEST` guards.

### 2.5 Multimodal Audio & Video

**Status — done** (`src/multimodal/audio.c`, `src/multimodal/video.c`, routing in `multimodal.c` / daemon; video path uses **ffmpeg** on POSIX).

**Files:**

- `src/multimodal/audio.c` — transcription pipeline (provider STT)
- `src/multimodal/video.c` — frame extraction + description pipeline
- `src/multimodal/multimodal.c` — route audio/video to handlers
- `src/daemon.c` — attachment handling aligned with multimodal ingress

### 2.6 Hierarchical Memory Summarization

**Status — done** (`hu_compact_hierarchical` in `src/agent/compaction.c`; life-chapters integration as implemented in-tree).

**Files:**

- `src/agent/compaction.c` — `hu_compact_hierarchical` (session → chapter → summary tiers)
- `src/memory/life_chapters.c` — integration with compaction output where wired
- Chapter-level persistence — follow SQLite memory schema in-tree

### 2.7 Constitutional AI Principles

**Status — done** — `core.principles[]` in persona JSON is parsed and emitted under `## Principles` in composed prompts (`src/persona/persona.c`).

**Files (as implemented):**

- `include/human/persona.h` — `principles` / `principles_count` on `hu_persona_t`
- `src/persona/persona.c` — parse `principles` from persona JSON and append to composed prompt
- (Optional follow-up) `src/persona/prompt.c` — only if prompt assembly is split further later

Principles like: "Be helpful but never harmful," "Admit uncertainty," "Protect privacy," "Defer to the real Seth on irreversible decisions."

---

## Phase 3: Evaluation & Calibration (The Mirror)

> You cannot improve what you cannot measure. Build the measurement infrastructure for both intelligence and fidelity.

**Timeline**: 2 weeks
**Risk**: Low (additive, no behavioral changes)
**Dependencies**: Phase 1 (channel-general tests), Phase 2 (cognition improvements to measure)

### 3.1 Fidelity Benchmark Suite

**Status — done** (suite + rubric files exist under `eval_suites/`).

Measure how "human" the responses feel.

**Files:**

- `eval_suites/fidelity.json` — tasks with real conversation snippets, expected timing distributions, expected response styles
- `docs/eval/fidelity_rubric.json` — LLM judge rubric for naturalness, appropriate length, emotional resonance

**Dimensions:**

- Timing distribution match (vs Seth's real iMessage timing data)
- Response length distribution (vs Seth's real patterns)
- Reaction frequency and appropriateness
- Proactive message quality (not too much, not too little)
- Emotional calibration (detects and responds appropriately to mood)

### 3.2 Intelligence Benchmark Suite

**Status — done** (reasoning, tool_use, memory, social, plus `intelligence.json` / `*_basic.json` variants in `eval_suites/`).

Measure AGI capabilities.

**Files:**

- `eval_suites/reasoning.json` — multi-step reasoning tasks
- `eval_suites/tool_use.json` — complex tool chains
- `eval_suites/memory.json` — recall accuracy over long conversations
- `eval_suites/social.json` — theory of mind, empathy, humor

### 3.3 Behavioral Calibration Pipeline

**Status — done** (`src/calibration/*`, `human calibrate`; **channel-agnostic** entry via `channel_name` / `"auto"` — not iMessage-only).

Capture Seth's real messaging patterns and use them to tune parameters.

**Files:**

- `src/calibration/timing_analyzer.c` — analyze real chat.db for response time distributions per contact, time of day, message type
- `src/calibration/style_analyzer.c` — analyze real messages for length distributions, emoji usage, reaction frequency, vocabulary
- `src/calibration/calibrate.c` — CLI command `human calibrate` that reads chat.db, produces persona parameter recommendations
- `src/cli_commands.c` — `calibrate` subcommand

**Output**: JSON file with measured distributions that the daemon can read to set timing parameters, filler probabilities, reaction frequencies, etc.

### 3.4 Live A/B Comparison

**Status — done** (`src/calibration/ab_compare.c`).

Compare digital twin output to what Seth actually said.

**Files:**

- `src/calibration/ab_compare.c` — given a conversation history, generate what the twin would say, compare to what Seth actually said
- Use LLM judge to score: "Which response is more Seth-like?"

### 3.5 Regression Dashboard

**Status — partial** — `hu_eval_dashboard_render_trend` exists in `src/eval_dashboard.c`; end-to-end “always-on” trend UX + SQLite history may still need product wiring.

Track improvements over time.

**Files:**

- `src/eval_dashboard.c` — fidelity + intelligence trend rendering
- Store eval runs in SQLite for historical comparison
- CLI: `human eval trend` shows score changes over time

---

## Phase 4: Cross-Channel Identity & Superhuman (The Soul)

> One Seth across all platforms, with capabilities no human has.

**Timeline**: 2–3 weeks
**Risk**: Medium
**Dependencies**: Phase 1 (channels generalized), Phase 2 (cognition), Phase 3 (measurement)

### 4.1 Unified Contact Graph

**Status — done** (`src/memory/contact_graph.c`, `hu_contact_identity_t`, SQLite-backed link/resolve APIs; used from agent/daemon paths).

Map identities across platforms.

**Files:**

- `src/memory/contact_graph.c` — cross-platform identity resolution
- `include/human/memory.h` — `hu_contact_identity_t` with platform handles
- `src/memory/superhuman.c` — unified contact ID for inside jokes, commitments, patterns (as wired)

**Schema:**

```sql
CREATE TABLE contact_identities (
    contact_id TEXT PRIMARY KEY,     -- canonical ID
    display_name TEXT,
    platform TEXT NOT NULL,          -- "imessage", "discord", etc.
    platform_handle TEXT NOT NULL,   -- phone number, Discord user ID, etc.
    confidence REAL DEFAULT 1.0,     -- 1.0 = manually confirmed, 0.5 = inferred
    UNIQUE(platform, platform_handle)
);
```

**Resolution**: When a message arrives on Discord from user "seth#1234", look up contact_identities to find the canonical contact_id. Memory, inside jokes, relationship stage — all keyed to the canonical ID.

### 4.2 Cross-Channel Context Awareness

**Status — done** (`cross_channel_ctx` in `src/daemon.c` via other channels' `load_conversation_history` and contact graph handles).

When texting on iMessage, know what was discussed on Discord yesterday.

**Files:**

- `src/daemon.c` — cross-channel history loading pulls from other registered channels for the same contact
- `load_conversation_history` vtable (Phase 1 enrichment) supplies recent exchanges per platform

### 4.3 Proactive Cross-Channel Routing

**Status — partial** — infrastructure exists (multi-channel contact awareness, proactive governor); **verify** all proactive outbound paths rank channels from real activity signals end-to-end.

Know which channel to reach someone on based on their activity patterns.

**Files:**

- `src/daemon.c` — proactive outbound considers all channels the contact is reachable on
- Track last-active-per-channel in contact profiles
- Route proactive messages to the channel where the contact was most recently active

### 4.4 Multi-Agent Task Swarm (Production)

**Status — mostly done** — `hu_swarm_execute` uses **pthread** worker pool; **`hu_swarm_aggregate`** implements merge strategies; ensemble provider covers **multi-model consensus** at the provider layer. Remaining: richer LLM-driven decomposition in `orchestrator_llm.c` vs static task lists.

Wire the orchestrator for real parallel work.

**Files:**

- `src/agent/orchestrator_llm.c` — LLM-driven goal decomposition (not pre-split subtasks)
- `src/agent/swarm.c` — parallel execution with result merging + aggregation
- Ensemble / judge-style flows — see `src/providers/ensemble.c` and spawn policy in `src/agent/spawn.c`

### 4.5 Self-Improvement Loop (Closed)

**Status — partial** — `self_improve.c` consumes eval outputs and persists patch metadata; a **fully automated nightly closed loop** (schedule + safe apply + rollback policy) is not yet a default product path.

Full closed loop: eval → weakness analysis → generate fix → apply → re-eval → keep or rollback.

**Files:**

- `src/intelligence/self_improve.c` — eval framework as quality signal (SQLite-backed patch tracking)
- Add: nightly self-improvement daemon job that runs eval suite, identifies weakest area, generates improvement, measures delta

---

## Phase 5: Voice, Presence & Authentic Existence (The Body)

> A digital twin needs more than text — it needs voice, presence, and a life.

**Timeline**: 3–4 weeks
**Risk**: High (external dependencies: Cartesia, platform APIs)
**Dependencies**: Phase 1 (voice generalized beyond iMessage), Phase 4 (identity)

### 5.1 Voice Across Channels

**Status — mostly done** — daemon gates TTS on **`hu_channel_daemon_config_t.voice_enabled`** for the active channel (not iMessage-only); per-channel audio packaging still varies by platform APIs.

Generalize Cartesia TTS beyond iMessage.

**Files:**

- `src/daemon.c` — voice path for channels with `voice_enabled`
- `src/tts/cartesia.c` — output format per channel (CAF for iMessage, OGG for Telegram, MP3 for Discord)
- `hu_channel_daemon_config_t` — per-channel voice enablement

### 5.2 Full-Duplex Voice Sessions

For channels that support voice calls (Discord voice channels, direct calls).

**Files:**

- Modify: `src/voice/duplex.c` — production-ready interrupt handling
- Modify: `src/voice/realtime.c` — <100ms first-byte latency target
- Integrate with Discord voice gateway, Telegram voice calls

### 5.3 Visual Content Generation

The twin should share photos, screenshots, links, memes — not just text.

**Files:**

- Modify: `src/visual/content.c` — production pipeline for image search, screenshot sharing, link previews
- Add: web image search tool for contextually relevant images
- Integrate with proactive governor (don't over-share)

### 5.4 Active Life Simulation

The twin should have opinions about current events, follow sports scores, track weather, notice things happening in the world.

**Files:**

- Modify: `src/feeds/` — ensure all feed types (news, social, Apple, file ingest) are running and integrated
- Modify: `src/persona/life_sim.c` — enrich with real-world awareness from feeds
- Create: `src/feeds/awareness.c` — synthesize feeds into "things Seth would naturally bring up"

### 5.5 Behavioral Cloning from Real Conversations

The ultimate calibration: learn from Seth's actual messaging history.

**Files:**

- Create: `src/calibration/clone.c` — analyze Seth's real iMessage history to extract:
  - Response timing distributions per contact
  - Vocabulary frequency maps
  - Emoji and reaction patterns
  - Topic initiation patterns
  - Conversation ending patterns
- Feed extracted patterns into persona config and daemon parameters
- Continuous refinement: as new real conversations happen, update the model

---

## Success Criteria

### Turing Test (per-channel)

| Channel | Test | Target |
|---|---|---|
| iMessage | Close contact 24-hour session | Indistinguishable |
| Discord | Server participation for 1 week | Not flagged as bot by other members |
| Telegram | 1:1 conversation with a friend | Cannot tell it's not Seth |
| Slack | Workspace participation for 1 week | Responses feel natural and on-brand |

### Intelligence Tests

| Metric | Current | Target |
|---|---|---|
| Eval suite pass rate | ~60% (est.) | >85% |
| Multi-step reasoning (5+ steps) | Fragile | Reliable (MCTS-backed) |
| Tool chain completion | Sequential only | DAG-parallel (LLMCompiler) |
| Memory recall accuracy (30-day) | Good | >95% F1 |
| Self-improvement cycle gain | Not measured | >10% per cycle |

### Fidelity Tests

| Metric | Current | Target |
|---|---|---|
| Timing distribution divergence | Not measured | KL divergence < 0.1 from Seth's real patterns |
| Response length distribution | Not measured | Within 1 std dev of Seth's real patterns |
| Reaction appropriateness (LLM judge) | Not measured | >8/10 average |
| Proactive message quality | Not measured | >7/10 average, <3 per contact per day |

---

## Implementation Order & Dependencies

```
Phase 1: Channel Generalization ──────────────────────────┐
  1.1 Daemon de-iMessageification                         │
  1.2 Channel vtable enrichment (Discord, Telegram, Slack)│
  1.3 Per-channel config generalization                   │
  1.4 Message splitting respects constraints              │
  1.5 Per-channel outbound formatting                     │
                                                          ▼
Phase 2: AGI Cognition ──────────────── Phase 3: Eval & Calibration
  2.1 MCTS → planner                    3.1 Fidelity benchmark suite
  2.2 LLMCompiler → agent loop          3.2 Intelligence benchmark suite
  2.3 Computer use (real)                3.3 Behavioral calibration pipeline
  2.4 Browser use (new)                  3.4 Live A/B comparison
  2.5 Multimodal audio/video             3.5 Regression dashboard
  2.6 Hierarchical summarization         │
  2.7 Constitutional AI                  │
           │                             │
           ▼                             ▼
Phase 4: Cross-Channel Identity & Superhuman ─────────────┐
  4.1 Unified contact graph                               │
  4.2 Cross-channel context awareness                     │
  4.3 Proactive cross-channel routing                     │
  4.4 Multi-agent task swarm                              │
  4.5 Self-improvement loop (closed)                      │
                                                          ▼
Phase 5: Voice, Presence & Authentic Existence
  5.1 Voice across channels
  5.2 Full-duplex voice sessions
  5.3 Visual content generation
  5.4 Active life simulation
  5.5 Behavioral cloning from real conversations
```

Phases 2 and 3 can run in parallel. Phase 4 depends on both. Phase 5 depends on Phase 4.

---

## Estimated Scope

| Phase | New/Modified files | New tests | Estimated effort |
|---|---|---|---|
| 1. Channel Generalization | ~15 | ~40 | 2–3 weeks |
| 2. AGI Cognition | ~12 | ~35 | 3–4 weeks |
| 3. Eval & Calibration | ~10 | ~25 | 2 weeks |
| 4. Cross-Channel Identity | ~8 | ~20 | 2–3 weeks |
| 5. Voice & Presence | ~10 | ~25 | 3–4 weeks |
| **Total** | **~55** | **~145** | **12–16 weeks** |

---

## Relationship to Existing Plans

This plan **subsumes and extends**:

- **Project Human Fidelity** (2026-03-10): Phases 1–9 focused on iMessage. This plan generalizes that work to all channels and adds measurement.
- **SOTA AGI Convergence** (2026-03-15): Phases 1–6 focused on intelligence infrastructure. This plan wires those systems into the agent loop and adds production-grade implementations where stubs exist.
- **Better Than Human** (2026-03-08): Seven layers of superhuman capability. This plan adds the calibration and cross-channel dimensions that make those capabilities feel like a person, not a system.

The digital twin is what happens when fidelity, intelligence, and measurement converge.
