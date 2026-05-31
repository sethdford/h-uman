---
title: Sprint Backlog — Better-Than-Human Tier 2/3/4
status: active
created: 2026-05-19
last_audit: 2026-05-25
---

# Sprint Backlog — Better-Than-Human Tier 2/3/4

**Date:** 2026-05-19
**Status:** Stories sized + scoped for sequential single-session sprints
**Parent plan:** [2026-05-19-better-than-human.md](2026-05-19-better-than-human.md)

This backlog is what remains after this session's parallel-agent batch on:
- ✅ Topic salience (shipped)
- ✅ Calibrate-with-reactions (shipped)
- ✅ Conversation gap detection (shipped)
- ✅ Better-than-human vision doc (shipped)
- ⏳ Cross-handle identity resolution (agent in progress)
- ⏳ Per-contact relationship signatures (agent in progress)
- ⏳ Pattern drift detection (agent in progress)

Each story is structured for a fresh session: cold-start context, file paths,
acceptance criteria, scope cuts, anti-goals, dependency notes. The order is by
estimated leverage descending; dependencies noted where they matter.

---

## STORY 1 — Predictive draft suggestions

**Why:** When the user opens a thread with Alice, generate the most-likely 3 messages they'd want to send next, given recent context + their persona + Alice's reaction history. The "feels telepathic" feature.

**Acceptance:**
- New CLI subcommand: `human drafts --contact <handle> [--channel <name>]` that prints 3 draft suggestions
- Each draft uses ≤ 60 tokens; takes ≤ 2 seconds with local MLX model
- Uses persona + last-30-message context + reaction signature for that contact
- Tests: 8+ contracts — empty history, single message context, contact-with-no-reactions, multilingual, edge case of 0 outbound messages

**Files to create / modify:**
- `include/human/predictive_drafts.h` (new)
- `src/predictive/drafts.c` (new, ~350 LoC)
- `src/app/cli_commands.c` (add subcommand handler, ~50 LoC)
- `tests/test_predictive_drafts.c` (new, ~250 LoC)

**Dependencies:** per-contact signatures (in progress this session); identity-resolver (in progress) for cross-channel contact unification.

**Anti-goals:** Don't send drafts. Don't store drafts. Don't expose via Slack/Discord/Telegram channels (CLI-only for Sprint 1; PWA surface in a follow-up).

**Scope cuts:** No streaming — return all 3 drafts as a batch. No "explain" mode. No diff-against-existing-draft.

**Estimated:** ~700 LoC, 1 session.

---

## STORY 2 — Cross-conversation emotional memory

**Why:** Alice mentioned her mother is sick in iMessage. Next time you message her — in ANY channel — the assistant should know to be sensitive. Currently `src/memory/emotional_graph.c` exists but isn't wired to the reaction-ingest pipeline.

**Acceptance:**
- New function: `hu_emotional_context_for_contact(model, handle, out_context, cap)` returns a 1-2 sentence summary of any "tender" or "concerning" emotional context tied to the contact within last 30 days
- Wired into persona prompt rendering: when composing a draft / reply to that contact, emotional-context block emits BEFORE the persona body
- Triggers: facts with keywords from a small "tender_emotional" lexicon (`sick`, `lost`, `divorce`, `funeral`, `grief`, `worried`, etc.) AND a recent timestamp
- Tests: 10+ contracts including the lexicon coverage, time-decay (context > 90 days old is dropped), false-positive guards (`sick of work` shouldn't trigger)

**Files:**
- `include/human/memory/emotional_context.h` (new or extend emotional_graph.h)
- `src/memory/emotional_context.c` (new, ~250 LoC)
- `src/agent/prompt.c` (wire the context block into prompt rendering, ~30 LoC)
- `tests/test_emotional_context.c` (new, ~200 LoC)

**Dependencies:** identity resolution (this session) — without unified contacts, cross-channel emotional memory is per-handle, not per-person.

**Anti-goals:** Don't infer emotions from message text directly (that's expensive and unreliable). Trigger only on explicit keywords + temporal proximity. Don't autocomplete sensitivity ("Alice's mother is sick, you should ask about her") — surface the FACT, let the user decide.

**Scope cuts:** English-only lexicon for now. No multi-event context aggregation (only the most recent tender fact).

**Estimated:** ~500 LoC, 1 session.

---

## STORY 3 — Persona-aware autoresponder

**Why:** When the user is unreachable (sleeping, in-flight, DND), reply on their behalf in a persona-faithful voice. NOT a generic "I'm away" — one that sounds like the user.

**Acceptance:**
- New config block: `autoresponder.enabled`, `autoresponder.allowlist[]` (explicit contact handles permitted), `autoresponder.dnd_schedule`
- When user is in DND AND incoming message is from allowlisted contact: the assistant generates a reply in the user's voice using current persona + recent style metrics + minimal context
- Each autoresponse is logged to `~/.human/autoresponder.log` for review
- Daily digest: "you missed 3 messages from Alice, Bob, Carol; here's what I said"
- Tests: 8+ contracts covering allowlist filtering, DND-schedule respect, persona-voice match (assertion via response containing user-name AND not generic phrases like "I'm currently unavailable")

**Files:**
- `include/human/autoresponder.h` (new)
- `src/agent/autoresponder.c` (new, ~400 LoC)
- `src/config/config_parse.c` (allowlist + schedule schema)
- `tests/test_autoresponder.c` (new, ~300 LoC)

**Dependencies:** None hard, but per-contact signatures (in progress) lets the responder match per-contact tone.

**Anti-goals:** Never auto-reply on first contact with someone. Never include the user's location, calendar, or sensitive personal info. Never claim to be the user ("hey it's Seth" — say "hey, this is Seth's assistant").

**Scope cuts:** SMS not supported (only channels that allow programmatic send). Slack/Discord/iMessage. No "smart" timing (always reply immediately when triggered).

**Estimated:** ~700 LoC, 1 session + UX iteration.

---

## STORY 4 — Long-horizon contact narratives

**Why:** Read 5 years of your iMessage history and surface a per-contact narrative: "You and Alice's relationship started with project work in 2022; shifted to hiking + climbing in 2024; she became more reserved in early 2025."

**Acceptance:**
- New batch job: `human research --contact <handle>` walks chat.db history, summarizes via LLM (local model), writes to `~/.human/contacts/<canonical>.md`
- Batch runs in chunks (year-by-year, then synthesized) to avoid context exhaustion
- Persistent state: re-running picks up where it left off
- Tests: 6+ contracts — empty history, single conversation, multi-year cross-section, persistent-state interrupted-and-resumed

**Files:**
- `include/human/research/contact_narrative.h` (new)
- `src/research/contact_narrative.c` (new, ~500 LoC)
- `src/app/cli_commands.c` (CLI subcommand)
- `tests/test_contact_narrative.c` (new, ~300 LoC)

**Dependencies:** identity resolution (this session); local LLM provider must be working.

**Anti-goals:** Don't auto-run on all contacts (user explicitly opts in per-handle). Don't store the narratives anywhere except local disk. Don't include them in the persona prompt automatically (they're a reference, not auto-context).

**Scope cuts:** No interactive correction ("nope, that's wrong"). One-shot summarization per session.

**Estimated:** ~900 LoC + LLM-cost notes. 1-2 sessions.

---

## STORY 5 — Multi-modal emotional tone from audio

**Why:** Voice messages have pitch, pause, speech-rate information that's lost in transcript. An audio-aware model can emit "Alice's last voice message sounded anxious" as a fact alongside the transcript text.

**Acceptance:**
- New module: parses recent voice-message attachments via on-device audio model, extracts an emotion vector (8 categories: neutral/happy/sad/angry/anxious/excited/tender/sarcastic)
- Top-1 emotion above threshold becomes a fact: `(Alice, voice_message_emotion, "anxious")` with confidence
- Tests: 6+ contracts including model-absent fallback (no audio model installed → graceful skip)

**Files:**
- `include/human/audio/emotion.h` (new)
- `src/audio/emotion.c` (new, ~400 LoC + model bindings)
- `tests/test_audio_emotion.c` (new, ~250 LoC)

**Dependencies:** Audio model (Coqui? whisper-emotion? Apple's on-device API on iOS 18+?). Identity resolution.

**Anti-goals:** Don't run cloud-side emotion inference. Don't include audio in any outbound LLM call.

**Scope cuts:** Defer the model choice to Sprint planning. Start with a stub that returns "unknown" for every audio sample so the wiring lands first; real model swaps in later.

**Estimated:** ~700 LoC + model integration. Multi-session.

---

## STORY 6 — Causal attribution of communication outcomes

**Why:** "You changed how you replied to Alice in March; her response latency dropped by 60% within 2 weeks." Lets the user actually A/B test their relationships. Genuinely research-grade.

**Acceptance:**
- Change-point detection on user's style-EWMA over time (find the date where avg_message_length or response_latency took a step)
- Correlated change-point detection on a specific contact's same metrics
- Report: "Around 2026-03-15, your replies to Alice got 40% shorter. Within 14 days, Alice's reply latency dropped 60%. Coincidence or effect?"
- Tests: synthetic style traces with known change-points, statistical test power, false-positive control

**Files:**
- `include/human/causal_attribution.h` (new)
- `src/research/causal_attribution.c` (new, ~600 LoC — statistical heavy)
- `tests/test_causal_attribution.c` (new, ~400 LoC)

**Dependencies:** per-contact signatures (this session); the statistical-test depends on enough history to be meaningful (>= 6 months of activity).

**Anti-goals:** Don't claim causation (we can only detect correlation in temporal change). Don't suggest interventions. The output is observational only.

**Scope cuts:** Single-dimension change-point detection for the first version. Multi-variate version is a follow-up.

**Estimated:** ~1000 LoC + research-grade testing. Multi-session.

---

## STORY 7 — Anticipatory memory surfacing

**Why:** The user is about to message Alice. The assistant has pre-loaded the most-relevant past conversations + recent emotional context + Alice's signature + the user's last 5 commitments to Alice — RIGHT before they start typing. Real-time performance critical.

**Acceptance:**
- Hot-path memory retrieval triggered by thread-open events from each channel
- Preserves a per-contact hot context cache (cleared after 5 minutes of inactivity)
- < 500ms from thread-open to context-ready
- Tests: cache hit/miss, eviction timing, multi-contact concurrent

**Files:**
- `include/human/memory/anticipatory_cache.h` (new)
- `src/memory/anticipatory_cache.c` (new, ~400 LoC)
- Channel poll path mods to fire thread-open events (~30 LoC per channel)
- `tests/test_anticipatory_cache.c` (new, ~300 LoC)

**Dependencies:** All Tier-2 features (signatures + identity + emotional context).

**Anti-goals:** Don't precompute for every contact ever (memory blowup). LRU + recency-weighted eviction.

**Estimated:** ~700 LoC + cross-channel integration. 1-2 sessions.

---

## STORY 8 — Persistent identity continuity

**Why:** The persona file is the user's relationship with the assistant. Versioned, exportable, portable, transferable. THIS is the "the assistant that's actually yours" thesis at full power.

**Acceptance:**
- New CLI subcommands: `human persona export --out <file>`, `human persona import <file>`, `human persona verify <file>`
- Export bundles persona + memory subset + reaction history into a portable archive
- End-to-end encryption for storage (user's chosen passphrase or device key)
- Across-device sync via iCloud Drive (Apple) OR manual file move (all platforms)
- Versioned: each export is a snapshot; import is non-destructive (creates new persona slot)
- Tests: roundtrip integrity, encryption + decryption with wrong passphrase fails, version-skew handling (importing v1 into v2 binary)

**Files:**
- `include/human/persona/portability.h` (new)
- `src/persona/portability.c` (new, ~500 LoC)
- `src/app/cli_commands.c` (3 subcommands)
- `tests/test_persona_portability.c` (new, ~350 LoC)

**Dependencies:** None directly, but the more Tier-1/2 work that's shipped, the more there is to export.

**Anti-goals:** Don't sync to any third-party server. User owns their data, period.

**Estimated:** ~900 LoC + protocol design + UX. Multi-session.

---

## Sequencing recommendation

**Sprint A (this session, in-progress):** Identity + signatures + drift → these 3 are the FOUNDATION for almost every other story below.

**Sprint B:** Predictive drafts + cross-conv emotional memory → both depend on Sprint A. Both are user-facing "feels magical" features.

**Sprint C:** Autoresponder + long-horizon narratives → user-controlled output features that build on signatures + identity.

**Sprint D:** Multi-modal audio emotion → independent track; can run parallel to A/B/C.

**Sprint E:** Causal attribution + anticipatory cache + persistent identity → these are the "research-grade + UX polish" tier. Each is its own focused session.

## What counts as "proven to work locally e2e"

For each story, the e2e bar is:
1. Compiles clean with -Werror, full test suite green
2. Runs against the user's actual chat.db (NOT just synthetic fixtures)
3. Produces a manually observable output (CLI command, log line, or persona-prompt difference)
4. Documented in a docs/guides/ entry showing how to invoke + what to expect

The first two are mechanically verifiable. The third is the "really works" check. The fourth makes it usable to a future operator.

## Anti-pattern guard

Each story above is sized for ONE focused session. If a story balloons past 1500 LoC of implementation, STOP and split it into sub-stories. The "better than human" claim doesn't survive sloppy execution.

## Related

- [2026-05-19-better-than-human.md](2026-05-19-better-than-human.md) — the vision behind these stories
- [2026-05-18-imessage-sota.md](2026-05-18-imessage-sota.md) — the foundation these stories build on
- [CLAUDE.md](../../CLAUDE.md) — operating principles
