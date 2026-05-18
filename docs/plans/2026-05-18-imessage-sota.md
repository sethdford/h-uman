# iMessage SOTA — close the persona/ingest gap

**Author:** seth.ford@gmail.com (with assistant)
**Status:** DRAFT — awaiting one design decision before Phase 1 (see §5)
**Worktree:** `strange-lamport-3d7b29`
**Branch:** `claude/strange-lamport-3d7b29`

## Goal

Make h-uman's iMessage integration **state-of-the-art for personal-model
learning**, not state-of-the-art for transport (we don't want to win
the AppleScript war). The win condition: every tapback, edit, unsend,
balloon-plugin event, and rich-text mention from chat.db produces a
typed fact in `hu_personal_model_t` within one daemon tick, with
half-life decay and trust-tier defaults applied automatically.

## Background — what the comparison showed

Verified 2026-05-18 against four sources (imessage-exporter, Mautrix,
Barcelona, BlueBubbles + Apple docs). See chat transcript for the
full table. Headlines:

- We already parse text, attributedBody (text-only), 7 reaction kinds,
  unsends, replies, read receipts, expressive sends, balloon labels.
  Equivalent to or better than Mautrix's standard `mac` mode.
- We have a tiered send fallback (imsg CLI → AppleScript → AX). Better
  than pure-AppleScript bridges, doesn't require SIP-disabled IMCore.
- **What's missing is downstream of the parser, not the parser itself.**
  Zero call sites for `hu_personal_model_*` / `hu_fact_extract_*` in
  `src/channels/imessage.c` (5,055 LoC), `src/channels/imessage_reactions.c`
  (154 LoC), `src/feeds/imessage.c` (176 LoC). The richest behavioral
  signal source we have is read and then discarded.

## Verified gaps (Phase 0, audit-verify-before-allege.md compliant)

| Gap | Evidence | Impact |
|---|---|---|
| No personal-model ingestion from iMessage events | Empty grep for `personal_model`/`fact_extract` across all 4 source files | M2 thesis blocker |
| Custom-emoji tapbacks not captured | Empty grep for `associated_message_emoji`; only kind code (2006) is read, not the actual emoji string | High |
| Typedstream parser is text-only | [imessage.c:49-81](../../src/channels/imessage.c) decodes `0x01 0x2B` length-prefixed text only; no attribute runs | Med |
| Audio message transcripts dropped | We emit `[Voice Message]` label only; Apple stores transcript in `payload_data` plist | Med |
| Edit history truncated to final version | We read `date_edited` but not `message_summary_info` plist (Apple keeps 5 versions, 15min window) | Med |
| App-message (balloon) payloads flattened | `balloon_bundle_id` → generic label; payload_data plist (Apple Pay amounts, polls, placemarks) dropped | High |
| Group chat events (rename / member add) not read | `group_action_type` + `group_title` columns never queried | Low |

## Phased plan

### Phase 1 — Ingest wiring (HIGHEST ROI; ~1-2 sessions)

Convert the iMessage events we **already parse** into facts in
`hu_personal_model_t`. Reuses existing infrastructure (decay,
half-life, dedup, trust tier).

**Scope:**

1. New file `src/channels/imessage_ingest.c` with one entry per event
   shape (`hu_imessage_ingest_reaction`, `hu_imessage_ingest_edit`,
   `hu_imessage_ingest_unsend`, `hu_imessage_ingest_reply`,
   `hu_imessage_ingest_balloon`, `hu_imessage_ingest_read_receipt`).
2. Each entry synthesizes a canonical English sentence
   ("Alice reacted ❤️ to message: `<truncated text>`") and routes through
   the existing `hu_personal_model_ingest()` pipeline. **Decision in §5.**
3. Wire into the channel poll path (find where `hu_imessage_poll_reactions`
   results are consumed; add ingest calls).
4. Trust-tier defaults: reactions=tier-1 (directly observed), edits=tier-2
   (intent inferred), unsends=tier-3 (intent uncertain), balloons=tier-1.
5. Tests: `tests/test_imessage_ingest.c` with 6 fixture events,
   asserting one `hu_heuristic_fact_t` is produced per event with
   correct subject/predicate/confidence.

**Acceptance:**
- After a tapback row lands in chat.db, the next daemon tick produces
  a fact in `hu_personal_model_t.facts` with subject=contact_handle.
- Half-life decay applies (90-day) without new code.
- Zero new dependencies; zero new HTTP calls.

### Phase 2 — Cross-channel custom-emoji tapbacks (~half session)

Add `char *emoji` to `hu_reaction_event_t`, populate from chat.db
`associated_message_emoji` column, and from the equivalent payloads in
Discord, Slack, Telegram, WhatsApp (where they exist). This is a
header-schema change with channel-wide ripple — small per-channel
but touches 4-5 channels.

**Scope:**
1. `reaction_event.h` add `const char *emoji` field (NULL = standard 7-kind reaction).
2. `imessage_reactions.c` SELECT `m.associated_message_emoji`, strdup into event.
3. Free path in `tests/test_imessage_reactions.c` canonical loop.
4. Update Discord/Slack/Telegram/WhatsApp poll paths to populate when present.
5. Phase 1's `imessage_ingest_reaction` learns to render "Alice reacted 🔥"
   when `emoji` set, else "Alice loved it".

**Acceptance:**
- iOS 17+ custom emoji tapback ingested with the actual emoji glyph.
- No regression in standard 7-kind path (existing tests still green).

### Phase 3 — Audio transcripts + edit history + group events (~1 session)

Three small additive readers, no schema changes downstream:

- **Audio transcripts**: parse `payload_data` plist when
  `balloon_bundle_id = com.apple.MobileSMS.MoVoMessageBalloon` (or
  similar). Add `hu_imessage_extract_audio_transcript(blob, ...)`.
  Plist key: `transcribed_text` (verify from real fixture).
- **Edit history**: parse `message_summary_info` plist for `ec` key
  (edit chain). Phase 1's `imessage_ingest_edit` learns to ingest the
  *delta* ("Alice softened 'I hate this' → 'I dislike this'") not just
  the final text — this is where edit signal becomes valuable for the
  personal model.
- **Group events**: query `group_action_type`+`group_title` from
  `message` rows; ingest as conversation-shaped events
  ("Alice renamed group to 'Vacation 2026'").

**Acceptance:** fixture-driven tests for each; no LLM calls.

### Phase 4 — Typedstream attribute-run parser (~1-2 sessions)

Clean-room reimplementation of imessage-exporter's typedstream attribute
extraction. Outputs structured `hu_imessage_attribute_run_t` array.
Extracts: mentions (`@name` with handle), link spans, 2FA/OTP detection,
iOS 18 text animations (Big/Echo/Jello/etc.).

**License posture (decision in §5).** Algorithm is documented in
imessage-exporter; LICENSE is GPLv3. Two paths:

- **Clean-room port**: read their algorithm, reimplement in C from
  scratch, no code copying. h-uman license unaffected.
- **Direct port with GPLv3 attribution**: faster but forces h-uman or
  this subsystem to be GPLv3. Probably non-starter given h-uman's
  permissive-license thesis.

Default: clean-room. Test fixtures derived from real chat.db blobs we
generate ourselves.

### Phase 5 — Balloon-plugin payload parsers (~1-2 sessions)

NSKeyedArchiver plist parser + per-balloon decoders for the 5
highest-value app-message types:

1. URL previews (`com.apple.messages.URLBalloonProvider`) →
   `(user, shared_url, "<og:title>")` facts
2. Apple Pay (`com.apple.PassbookUIService.PeerPayment`) →
   `(user, sent_money_to, <handle>)` (no amounts in personal model —
   privacy)
3. Placemark (`com.apple.mobileslideshow.PhotosBalloonProvider`,
   maps variant) → `(user, shared_location, <place>)`
4. Polls (iOS 18+, bundle TBD) → `(user, asked_group_to_vote_on, <topic>)`
5. Music share → `(user, shared_music, <track/artist>)`

**Out of scope** for Phase 5: Apple Cash exact amounts (privacy default
on); Find My continuous location (too high-volume); GamePigeon
(low-signal).

### Phase 6 — Schema-version awareness + golden fixtures (~half session)

- Probe `PRAGMA table_info(message)` at startup; cache feature
  availability flags (`has_date_retracted`, `has_thread_originator_guid`,
  `has_associated_message_emoji`, `has_group_action_type`).
- Generate a `tests/fixtures/chatdb/` golden DB with one row per feature.
- Add `tests/test_imessage_schema_versions.c` that exercises queries
  against schemas matching macOS Catalina / Big Sur / Monterey /
  Ventura / Sonoma / Sequoia / Tahoe.

## Out of scope (explicit)

- **Barcelona / IMCore private framework path.** No longer maintained;
  requires SIP-disabled on user systems; Apple is closing this surface
  release-over-release. Don't go there.
- **RCS-specific parsing.** iOS 18 added RCS; Apple's chat.db schema
  changes are not yet documented publicly. Revisit after iOS 18.5
  releases and the community has reverse-engineered the deltas.
- **Sending message effects / stickers / inline replies / edits.**
  IMCore-only. We use the `*correction` workaround and AX-driven
  tapbacks; that's the ceiling for non-IMCore.
- **Digital Touch / handwriting parsers.** Low volume in 2026; the
  protobuf format is upstream-stable in imessage-exporter if we ever
  need it.
- **GenAI fact extraction from typedstream content.** Phase 1 uses
  text synthesis + `hu_personal_model_ingest` (which internally uses
  fact_extract). Don't add an additional LLM hop.

## Decision points (need user input before Phase 1)

### Decision 1 — Synthesis vs structured API for ingest

For Phase 1, we have two paths to get an iMessage event into
`hu_personal_model_t`:

**Path A (proposed default): text synthesis.** Render the event as a
canonical English sentence, route through existing
`hu_personal_model_ingest(model, text, ...)`. Reuses dedup/decay/half-life
automatically. One downside: each ingest may invoke `hu_fact_extract_llm`
which is an LLM call.

**Path B: structured fact API.** Add `hu_personal_model_ingest_fact(model,
hu_heuristic_fact_t *fact, ...)` that bypasses text extraction. Faster,
deterministic, no LLM cost. Downside: bypasses the contradiction/conflict
resolver that text ingestion runs.

**Recommendation:** Path A for Phase 1 (reversible, smaller, fewer new
APIs). Add Path B later if LLM cost becomes a problem.

### Decision 2 — Phase 4 license posture

Clean-room port of imessage-exporter typedstream parser (GPLv3 ⟶ MIT-clean),
or accept a single GPLv3 subsystem under `src/channels/imessage_typedstream/`
behind a build flag.

**Recommendation:** clean-room. Worth the extra session.

### Decision 3 — Ingest gating

Should iMessage events ingest into the personal model for **all**
contacts, or only contacts the user has marked as "high trust"
(allowlist) or excluded via a "low signal" blocklist (deny-list)?

**Recommendation:** ingest from all, but tag each fact with the
contact handle as part of provenance, and add a future
`hu_personal_model_purge_contact()` operation for retroactive cleanup.
This matches the local-first privacy thesis — data never leaves the
device — without burdening setup.

## Success criteria

After all 6 phases:
- ≥ 95% of iMessage events that match a fact template produce a fact
  in `hu_personal_model_t` within one daemon tick (measured via
  `tests/test_imessage_personal_model_integration.c`).
- The personal-model prompt summary includes at least one
  iMessage-derived line ("Alice [contact] tends to ❤️ messages about
  hiking") after 50 fixture events.
- Binary size growth ≤ 30 KB stripped.
- Zero new external dependencies.
- Test suite: existing 10,600+ stays green, +60-80 new tests.

## Risk register

| Risk | Mitigation |
|---|---|
| Schema differences across macOS 12-26 | Phase 6 explicitly tests fixtures per version |
| `payload_data` plist parser pulls in CoreFoundation | Use minimal plist parser; reference [src/io/plist.c](../../src/io/plist.c) if exists, else write ~200 LoC bplist00 parser |
| Privacy concern: ingesting financial events (Apple Pay amounts) | Phase 5 explicitly excludes amounts from facts; only fact-of-payment |
| LLM cost spike from Phase 1 Path A | Phase 1 already gates ingest at daemon-tick cadence (not per-message); Path B exists as escape hatch |
| Cross-channel custom-emoji ripple breaks Discord/Slack/Telegram | Phase 2 explicitly tests each channel; new field defaults to NULL |

## Related

- [docs/plans/2026-05-17-m3-mlx-bridge-execution-plan.md](2026-05-17-m3-mlx-bridge-execution-plan.md) — separate M3 stream; this plan touches M1 (persona-first) and M2 (personal model), not M3.
- [src/channels/CLAUDE.md](../../src/channels/CLAUDE.md) — existing iMessage capability documentation
- [include/human/memory/personal_model.h](../../include/human/memory/personal_model.h) — ingestion API
- [include/human/memory/fact_extract.h](../../include/human/memory/fact_extract.h) — fact-extraction primitives
- [~/.claude/rules/audit-verify-before-allege.md] — discipline followed in Phase 0
