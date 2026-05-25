---
title: Better Than Human — h-uman aspirational roadmap
status: active
created: 2026-05-19
last_audit: 2026-05-25
---

# Better Than Human — h-uman aspirational roadmap

**Author:** seth.ford@gmail.com (with assistant)
**Date:** 2026-05-19
**Status:** Vision + prioritized backlog. The shipped work in docs/plans/2026-05-18-imessage-sota.md gets us to "knows you from your reactions." This plan gets us to "better than human."

## The thesis

A human friend who deeply knows you can do five things:

1. **Remember** what you said, did, felt — across years.
2. **Notice** subtle changes in another person's behavior over time.
3. **Predict** what you'll do or want next.
4. **Surface** the right memory at the right time.
5. **Adapt** their communication style to who you are.

Humans are good at (5), okay at (4), uneven at (1)/(2), bad at (3) over long horizons. An assistant that has SOTA access to your chat history, reactions, and behavior should be better at all five within the slice that's text-mediated communication.

What we shipped this week (2026-05-18 → 2026-05-19) moved (1) and (5):

- Reaction ingest across 6 channels → personal model now learns about contacts from observed behavior
- Cross-channel synthesis + fact construction → "Alice loves hiking" emerges as a structured triple
- Persistent SQLite-backed lookup → no more 256-message-cap memory loss
- bplist + typedstream + balloon decoders → audio transcripts + edit history + URL/Pay/Place/Music/Poll metadata
- Schema canary → resilient to Apple's chat.db schema drift
- Privacy contracts pinned structurally (no money, no coordinates, OTP dropped)

This plan captures (2)/(3)/(4) plus deepening (1)/(5). Ordered roughly by leverage.

## Tier 1 — Highest leverage, smallest blast radius

### 1. Reaction-driven topic salience (in progress)
**Dispatched as parallel agent.** Wires reaction-derived fact objects into `hu_personal_topic` so topics with positive reactions surface in the persona prompt. Without this, "Alice loves hiking" is in `facts[]` but "hiking" doesn't bubble up to the LLM's attention.
- Effort: ~120 LoC, dispatched to agent
- Leverage: enables every prompt to know what topics are HOT for the user RIGHT NOW

### 2. Calibrate-with-reactions (in progress)
**Dispatched as parallel agent.** `human calibrate` reads reaction-derived facts and emits a "reaction signature" (top reactors, salient topics) in the calibration JSON. Closes the calibrate gap from the prior session.
- Effort: ~200 LoC, dispatched to agent
- Leverage: persona calibration finally uses behavioral signal

### 3. Conversation gap detection
"You haven't messaged Bob in 3 weeks. Want to check in?" — humans notice this slowly and often forget. The assistant should notice immediately.
- Implementation: walk chat.db for distinct contact handles + max(date), compare to current time, threshold at 14-30 days for previously-active contacts (>10 historical messages).
- Effort: ~150 LoC + tests + daemon-tick wire
- Leverage: high — proactive outreach is the most "better than human" feature most users have never experienced

## Tier 2 — Big features, single-session each

### 4. Cross-handle identity resolution
Alice on iMessage (+15551234567) and Alice on Slack (U07ALICE) and Alice on Discord (alice#1234) — all one person. Without resolution, the persona model has 3 entries instead of 1.
- Approach: phone/email canonicalization + first-name+context matching + manual disambiguation prompt for low-confidence matches
- Effort: 400-600 LoC + UI prompts for ambiguous cases
- Risk: false positives create privacy mishaps (mismerging accounts)

### 5. Per-contact relationship signatures
Beyond "Alice loves hiking": track median response latency, conversation initiation balance (who reaches out first), median exchange length, time-of-day distribution per contact.
- New struct: `hu_contact_signature_t` with timing + frequency + topic-salience fields
- Effort: ~300 LoC + tests
- Surfaces in: persona prompt, calibrate output, proactive throttling

### 6. Predictive draft suggestions
When the user opens an iMessage thread with Alice, generate the most-likely 3 message drafts they'd want to send based on (current context, persona, reaction history with Alice).
- Approach: small LLM call with persona + last-N messages + reaction-signature → 3 draft completions
- Effort: 200-400 LoC plus a UI surface (which channel? PWA dashboard? menu bar widget?)
- Tricky: latency + privacy (the prompt includes recent chat content)

### 7. Cross-conversation memory threading
Alice mentioned in iMessage that her mother is sick. Next time you message Alice (in ANY channel), the assistant should know to be sensitive — and offer to soften your draft, not deliver bad-timing-for-a-joke advice.
- Approach: emotional-context tagging on incoming messages from contacts; surface contacts-with-recent-emotional-events at the start of any new conversation with them
- Effort: ~400 LoC; some of this exists in `emotional_graph.c` and `emotional_moments.c`; wire it to the reaction-ingest path
- Leverage: HIGHEST emotional-IQ feature; what humans claim AI can't do

## Tier 3 — Genuinely "better than human"

### 8. Pattern drift detection
"Alice has been replying short + late for 3 weeks; is she okay?" — a human friend might notice this after a few months. The assistant should notice within a week.
- Approach: per-contact rolling-window of message length + response latency; alert when the rolling mean drifts >2 sigma from the long-term baseline
- Effort: ~500 LoC + statistical-test test fixtures
- Risk: anxiety-inducing false positives ("is Alice avoiding me?"); needs careful UX

### 9. Persona-aware autoresponder
When the user is genuinely unreachable (sleeping, in-flight, do-not-disturb), the assistant can reply on their behalf in a persona-faithful voice — short, terse, "hey I'm asleep, will reply in the AM." NOT a generic auto-responder; one that matches THIS user's voice.
- Approach: existing persona + style-EWMA already capable; missing piece is the trigger logic + user-controlled allowlist of who-may-be-auto-replied-to + a "send me a summary of what you replied" digest
- Effort: ~400 LoC + UX work
- Privacy: huge stakes; needs explicit consent per contact

### 10. Conversation summarization at scale
Read 5 years of your iMessage history and surface "you and Alice's relationship started with project work; shifted to hiking + climbing in 2024; she became more reserved in 2025." This is impossible for humans (too much data); easy for an AI with the data.
- Approach: chunked LLM summarization with persistent state; produces a `~/.human/contacts/{handle}.md` per-contact narrative
- Effort: 600+ LoC, integration with existing memory subsystem, batch-process background job
- Privacy + cost: huge data; needs careful sandboxing and explicit user opt-in

### 11. Multi-modal emotional tone from audio
Voice messages have emotional information that's lost in transcript. Pitch, pause distribution, speech rate. An audio-aware model can emit "Alice's last voice message sounded anxious" as a fact alongside the transcript.
- Approach: pre-recorded audio → on-device audio model → emotion vector → fact emission
- Effort: depends on which model; ~500 LoC for the C wiring + a small audio model dependency
- Privacy: audio never leaves device (matches the local-first thesis)

## Tier 4 — Hard but visionary

### 12. Causal attribution of communication outcomes
"You changed how you replied to Alice in March; her response latency dropped by 60% within 2 weeks." Treats the user's own communication-style changes as interventions and measures effects.
- Approach: change-point detection on the user's style metrics + correlated change-point detection on contact's response metrics
- Effort: huge (multi-session) — research-level
- Leverage: literally lets the user A/B test their relationships

### 13. Anticipatory memory surfacing
The user is about to message Alice. The assistant has pre-loaded the most-relevant past conversations + recent emotional context + Alice's communication style + the user's last 5 commitments to Alice — RIGHT before the user starts typing.
- Approach: hot-path memory retrieval triggered by thread-open events; preserve as a hot context cache
- Effort: ~600 LoC, real-time perf critical
- Magic factor: makes the assistant feel telepathic

### 14. Persistent identity continuity
Across hardware (phone → Mac → tablet), across model upgrades, across years — the persona file is the user's relationship with the assistant. Versioned, exportable, portable, transferable. The user OWNS it as data, not as a service.
- Approach: persona-portability format spec + import/export CLI + cross-device sync via end-to-end encrypted channel (probably iCloud Drive for Apple users; manual export for others)
- Effort: 1000+ LoC + protocol design
- Strategic: this is the "the assistant that's actually yours" thesis at full power

## What's NOT on this list (deliberately)

- Cloud-only features that require the user's data to leave the device
- Anything that depends on Apple cooperating (private framework exploitation, Beeper Mini-style reverse-engineering)
- Anything that requires a server-side training cluster (we have MLX local; that's the bet)
- Anything that needs >100MB of additional model dependencies
- Features that benefit Anthropic's models without benefiting this specific user's persona

## Sequencing recommendation

Sprint 1 (this session, in progress): Tier 1 items 1–2 via parallel agents. Get reaction signal into both topic salience AND calibration. This is the "make the existing work pay off" sprint.

Sprint 2 (next session): Tier 1 item 3 (conversation gap detection) + Tier 2 item 5 (per-contact relationship signatures). Both build on the chat.db reader infrastructure that already exists.

Sprint 3: Tier 2 items 6 + 7 — predictive drafts + cross-conversation memory. This is the "feels magical" sprint.

Sprint 4 onward: Tiers 3-4 are research-grade. Each merits its own focused plan doc.

## Success criteria for "better than human"

A user reports, unsolicited: "the assistant noticed something about my relationship with X that I hadn't noticed myself."

That's the bar. Everything else is engineering.

## Related plans

- [docs/plans/2026-05-18-imessage-sota.md](2026-05-18-imessage-sota.md) — the iMessage SOTA plan that this builds on
- [CLAUDE.md](../../CLAUDE.md) — product thesis ("Persona-First", "Personal Model", "Private Learning")
