# Spec: Persona Overlay Wiring (Tier-1 Channels)

**Status:** Spec — not yet implemented
**Author:** 2026-05-16 audit follow-up
**Owner:** TBD
**Risk:** Medium — touches send/receive paths in 4 production channels
**Effort:** 1–2 weeks for Tier-1, +1 week to fan out to remaining 40 channels

## Problem statement

The h-uman product thesis (M1, "Persona-First") claims persona is "always-on"
and **channel-aware** via `hu_persona_overlay_t`. The audit found:

> All 43 channel vtables declare `hu_persona_overlay_t` per-channel, but **0
> channels apply it** in send/receive paths.

The overlay struct (with per-channel `formality`, `length`, `emoji`,
`response_speed` overrides) is structurally wired through every channel
factory, **but the data is discarded** before the message is composed. A
Slack user and an iMessage user get the same persona-rendered text.

This is the single biggest structural gap behind the M1 thesis: the
mechanism exists, the data exists, the integration last-mile is missing.

## Acceptance criteria

| AC | Description | Verification |
|---|---|---|
| AC-1 | Telegram outbound applies `overlay->formality` to text before send. | Unit test: send a "casual" overlay → assert emoji density > "formal" overlay. |
| AC-2 | Discord outbound applies `overlay->length` to truncate / pad. | Unit test: overlay sets `max_chars=200`; assert outbound text <= 200. |
| AC-3 | Slack outbound applies `overlay->emoji`. | Unit test: overlay disables emoji; assert outbound text has none. |
| AC-4 | iMessage outbound applies all overlay fields. | Unit test analogous to AC-1/2/3 combined. |
| AC-5 | A new helper `hu_persona_render_for_channel()` applies the overlay deterministically. | Direct unit test on the helper. |
| AC-6 | When `overlay == NULL`, channels behave exactly as today (no regression). | Replay existing channel tests; all pass unchanged. |
| AC-7 | Overlay lookup is O(1) per send (no string compares in the hot path). | Profile or inspection. |

## Design

### Phase 1 — Helper (no channel changes yet)

Add `src/persona/render.c`:

```c
hu_error_t hu_persona_render_for_channel(
    const hu_persona_t *persona,
    const hu_persona_overlay_t *overlay,  /* may be NULL */
    const char *raw_text,
    hu_allocator_t *alloc,
    char **out_rendered);   /* caller frees */
```

Behavior:
- If `overlay == NULL`, return a copy of `raw_text` (no-op).
- Apply `formality`, `length`, `emoji`, `response_speed` in a fixed order.
- Document each transform's idempotence and order-sensitivity.

Tests: 12 unit tests covering each overlay field independently + combinations
+ NULL overlay + NULL persona.

### Phase 2 — Wire into 4 Tier-1 channels

For each of `telegram.c`, `discord.c`, `slack.c`, `imessage.c`:

1. Locate the outbound send entry point (e.g., `telegram_send`).
2. Just before the network call, look up the channel's overlay from the agent's
   persona (helper: `hu_persona_get_overlay(persona, channel_name)`).
3. Call `hu_persona_render_for_channel()` to rewrite the outbound text.
4. Use the rendered text for the network call. Free after send.

Per-channel test additions:
- One test per channel asserting overlay is applied (matches the AC above).
- One test per channel asserting `overlay == NULL` produces today's behavior.

### Phase 3 — Fan out

After Tier-1 lands, repeat for the remaining 39 channels in a single follow-up
PR. Behavior is identical; the helper is reused.

## Out of scope

- Inbound persona inference (e.g., adapting tone to user's tone). Separate spec.
- Voice channel overlay (different schema — see `src/voice/`).
- Persona learning from feedback (the M3 bridge handles this elsewhere).

## Audit evidence

- All 43 channel vtables declare `hu_persona_overlay_t`; 0 use it in send/receive.
- Tier-1 send paths confirmed unit-test-only with mocks (no integration tests
  except iMessage).
- `src/channels/telegram.c:telegram_send`, `discord.c:discord_send`,
  `slack.c:slack_send`, `imessage.c` AppleScript/imsg path — none read overlay.

## Risks

- **Overlay schema drift.** If the helper is added then per-channel fields are
  added later, every channel must be updated again. *Mitigation:* helper takes
  the whole overlay struct, not individual fields, so adding a field is a
  helper-side change only.
- **Hidden ordering coupling.** If formality changes capitalization and length
  truncates after, results may surprise. *Mitigation:* spec orders the
  transforms and tests pin the order.
- **Test mock divergence.** Existing channel mocks don't see the overlay
  layer. *Mitigation:* AC-6 — overlay==NULL must be no-op, so mocks keep
  passing without modification.

## Status update (2026-05-17)

**Tier-1 shipped.** All four Tier-1 channels now apply
`hu_persona_render_for_channel` to outbound text before transmission:

| Channel  | Wiring site                                             | Setter                       |
|----------|---------------------------------------------------------|------------------------------|
| Telegram | `src/channels/telegram.c:755-770` (top of `telegram_send`) | `hu_telegram_set_persona`    |
| Discord  | `src/channels/discord.c:129-142` (top of `discord_send`)   | `hu_discord_set_persona`     |
| Slack    | `src/channels/slack.c:517-530` (top of `slack_send`)       | `hu_slack_set_persona`       |
| iMessage | `src/channels/imessage.c:1317-1332` (test) / `1364-1380` (production, BEFORE markdown strip & sanitizer) | `hu_imessage_set_persona` |

The helper itself lives in `src/persona/render.c` (declaration:
`include/human/persona.h:587-614`). Tests:
`tests/test_persona_overlay_render.c` (12 helper-level tests, all PASS)
and `tests/test_channel_overlay_apply.c` (9 per-channel + cross-channel
tests, all PASS). Full suite: 10668/10668 passed, 0 ASan errors.

**Still TODO — fan-out to ~39 non-Tier-1 channels.** None of the
remaining channels have been wired. Categories grouped by similarity for
follow-up dispatch:

- **Chat (similar to Slack/Discord):** mattermost, teams, google_chat,
  matrix, irc, dingtalk, lark, qq, onebot, line.
- **SMS/IM (similar to iMessage/Telegram):** twilio, whatsapp, signal,
  google_rcs.
- **Social/post (different surface — overlay applies but length/format
  rules will need per-channel buckets):** facebook, instagram, twitter,
  tiktok, nostr.
- **Email (presentation-heavy — overlay should apply to body only):**
  email, imap, gmail.
- **Voice / non-text:** voice_channel (skip — overlay schema differs;
  see "Out of scope" above).
- **Embedded / hardware:** mqtt, maixcam (low priority; minimal text
  surface).
- **CLI / web:** cli, web, pwa (review whether overlay applies — these
  are dev surfaces, not user-facing).

Each is a near-mechanical copy of the Telegram pattern:
add `const hu_persona_t *persona` to the ctx struct, expose
`hu_<chan>_set_persona`, and call `hu_persona_render_for_channel` with
the channel's own name string at the top of the send function. The
`overlay==NULL` no-op guarantee keeps existing tests green.
