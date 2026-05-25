# iMessage Action Surface — Requirements

**Date:** 2026-05-25
**Spec dir:** `docs/plans/2026-05-25-imessage-action-surface/`
**Status:** APPROVED — proceeding to design phase
**Trigger:** Screenshot of Apple Messages right-click menu — user observation that h-uman replies don't look like Apple-native threaded replies; broader ask is "we should be able to do all those things in the drop down."

## Framing

Apple Messages exposes a per-message context menu (right-click / two-finger trackpad / long-press) with these items, all visible in the user's screenshot:

| Action            | Today in h-uman                                                                                  |
|-------------------|--------------------------------------------------------------------------------------------------|
| 6 classic tapbacks (Loved / Liked / Disliked / Laughed / Emphasized / Questioned) | ✅ supported via `ax_tapback` 3-tier chain |
| 6 custom-emoji tapbacks (Sonoma+ bottom row)                                       | ❌ not supported                          |
| **Reply…** (threaded inline reply, writes `reply_to_guid` in chat.db)              | ❌ flat send only — looks unthreaded     |
| **Attach Sticker…**                                                                | ❌ not supported                          |
| Tapback Details                                                                    | n/a (read-only UI, already have via DB)   |
| Forward / Delete                                                                   | ❌ out of scope this sprint (blast radius)|
| Copy / Show Times                                                                  | n/a                                       |

The headline gap: every reply we send today is a flat sibling message rather than a threaded reply, so it visually disconnects from the message it's responding to.

The deeper ask is **human-likeness, not rules-based behavior**. When to thread, when to send a sticker, when to react with a custom emoji vs reply with text — these should be persona-weighted judgments sampled with jitter, NOT a static rule like "always thread."

## User stories

- **US-1:** As Seth (the operator), I want h-uman's replies on iMessage to look like native Apple threaded replies (proper bubble nesting, `reply_to_guid` set) when context warrants threading, so the conversations don't feel mechanically detached.
- **US-2:** As Seth, I want h-uman to *sometimes* reply flat and *sometimes* thread, the way a real human does — never threading rapid-fire chat, often threading delayed/multi-question contexts — so my correspondents don't notice the agent's seam.
- **US-3:** As Seth, I want h-uman to be able to react with any of the 12 tapback options (6 classic + 6 custom emoji), so its acknowledgments feel like 2026 macOS Sonoma+ iMessage, not 2021 macOS Big Sur.
- **US-4:** As Seth, I want h-uman to be able to send a small sticker (from `~/.human/stickers/`) when the persona judges that casual visual acknowledgment fits the moment better than text, so the conversational palette matches what humans actually use on iMessage.
- **US-5:** As a developer maintaining h-uman, I want the "when to thread" decision to live in a pure predicate function I can unit-test with a truth table, so the human-likeness logic is verifiable without spawning AX subprocesses.
- **US-6:** As a developer, I want every reply-style decision logged with its facts and outcome, so we can mine the trail later to tune persona dials based on what actually felt natural.

## Acceptance criteria

- [ ] **AC-1 (predicate exists and is pure):** A new function `hu_imessage_choose_reply_style(const hu_reply_style_facts_t *facts, uint64_t rng_seed) → hu_reply_style_t` exists in `include/human/channels/imessage_action.h`. It performs no I/O, depends only on inputs, and returns one of `HU_REPLY_STYLE_{FLAT, THREADED, TAPBACK, TAPBACK_PLUS_FLAT}`. **Measurable:** unit test in `tests/test_imessage_reply_style.c` constructs 12 fact tuples and pins the expected style for each; same seed + same facts always yields the same style.

- [ ] **AC-2 (human-like sampling — never always-thread):** Under representative inbound distributions (≥100 synthetic fact-tuples covering rapid-fire / delayed / question-heavy / mirror / persona-formality sweep), the predicate produces a mix of styles: thread-rate falls between 15% and 65%, never < 5% and never > 85%. **Measurable:** `tests/test_imessage_reply_style.c::style_distribution_is_human_shaped` runs ≥100 cases, asserts the distribution. **Soft mirror:** when `other_threaded_replies_recent >= 2` the thread-rate on that subset is at least 2× the global rate. **Density damping:** when `conv_density_msgs_per_min > 6` the thread-rate on that subset is at most 0.5× the global rate.

- [ ] **AC-3 (emotional protection — never tapback-only on hard moments):** When `facts.parent_emotional_intensity >= HU_EMOTION_THRESHOLD_MEDIUM`, the predicate NEVER returns `HU_REPLY_STYLE_TAPBACK` alone (tapback-only acknowledgment of a vulnerable message reads as dismissive). Allowed returns in that case: `FLAT`, `THREADED`, or `TAPBACK_PLUS_FLAT`. **Measurable:** parametric test sweeps all other facts × intensity ≥ medium and asserts no `TAPBACK` solo return.

- [ ] **AC-4 (threaded reply send path):** A new vtable method `reply(ctx, target, target_len, parent_msg_guid, body, ...)` is added to `hu_channel_t`. The iMessage implementation tries Cmd-R (when the row is focusable in AX), falls back to AX `AXShowMenu` → "Reply…", falls back to a flat send with a log warning. **Measurable:** `tests/test_imessage_threaded_reply.c` uses an AX mock harness asserting each tier is attempted in order and the right menu item is selected; integration test against a fixture chat.db asserts the resulting outbound row has `reply_to_guid` set to the parent guid (or, in fallback mode, the row exists with no `reply_to_guid` AND a `WARN: reply degraded to flat` line is in the log).

- [ ] **AC-5 (custom-emoji tapback):** The existing `react()` vtable method is extended to accept an arbitrary Unicode emoji codepoint (in addition to the 6 classic enum tapbacks). When given a custom emoji, the iMessage impl uses the AX path navigating to the bottom-row emoji picker; falls back to closest classic tapback (e.g. ❤️ → Loved; 😂 → Laughed). **Measurable:** `tests/test_imessage_custom_tapback.c` covers 6 custom emojis (the screenshot's bottom row: 😂 ❤️ 😍 😒 👌 + one explicit fallback case like 🦄 → Loved).

- [ ] **AC-6 (sticker MVP):** A new vtable method `send_sticker(ctx, target, target_len, sticker_path)` is added. The iMessage impl sends the sticker file as an attachment via the existing `imsg send --file` path (NOT via Apple's sticker balloon-bundle — explicitly out of scope per design §5). A persona helper `hu_persona_pick_sticker(persona, conv_context, out_path, out_cap) → bool` picks from `~/.human/stickers/` weighted by filename tags (e.g. `casual-happy_001.png` matches casual+happy contexts). **Measurable:** `tests/test_imessage_sticker.c` constructs a fake `~/.human/stickers/` dir with 5 tagged stickers, asserts the picker returns a matching one for the input context, and the send path invokes `imsg send` with `--file`. Honest caveat documented in design: this sends as attachment, not as native sticker balloon.

- [ ] **AC-7 (latency budget + jitter — feels human, not too fast):** The reply path adds persona-weighted jitter on top of the AX cost so reply latency is NEVER < 1.2× the configured min-reply-delay for the active persona. **Measurable:** `tests/test_imessage_reply_pacing.c` runs the reply path with a stubbed AX (returning instantly) and asserts the elapsed wall-clock is `>= persona.min_reply_delay_ms * 1.2` across 20 iterations.

- [ ] **AC-8 (telemetry — every decision is mined):** Every call to `hu_imessage_choose_reply_style` AND every actual send attempt logs a JSON line to `~/.human/logs/imessage_action.jsonl` with `{ts, target_chat_id_hash, facts, style_chosen, send_result, tier_used, elapsed_ms}`. **Measurable:** `tests/test_imessage_action_telemetry.c` exercises the path and asserts the JSONL line is well-formed and contains all required keys.

- [ ] **AC-9 (regression-free):** Full h-uman test suite (11,900+ tests) passes with 0 failures and 0 ASan errors. **Measurable:** `./build/human_tests` exits 0. Existing tapback + send behavior unchanged when no new code paths trigger (config gate `iMessage.action_surface_v2.enabled` defaults to **true** on macOS, **off** on Linux/CI).

## Non-goals

- ❌ **NOT** implementing Forward or Delete in this sprint. Forward has chat-ID-misdirection leak risk; Delete is destructive and Apple only allows it within a 2-minute window. Both deserve their own spec with explicit guardrails.
- ❌ **NOT** sending true balloon-bundle stickers via private `IMSticker` API. The IMCore daemon-connection wall on macOS 26+ makes this infeasible now (see [imessage.c:3637 `imcore_init`](../../src/channels/imessage.c)). Sticker MVP sends as a file attachment with a "(sent sticker)" downgrade path documented in design.
- ❌ **NOT** rewriting the existing tapback chain (`ax_perform_tapback_on_row` etc.) — extend it, don't replace it.
- ❌ **NOT** adding LLM-in-the-loop decision for reply-style. The predicate is pure C math + a seeded RNG, sampled per turn. Adding a Gemini call for "should I thread?" defeats the latency budget and the cost story.
- ❌ **NOT** changing chat.db directly. All outbound paths go through `imsg send` or AX automation of the running Messages.app.
- ❌ **NOT** modifying any other channel (Telegram/Discord/Slack/etc.) in this sprint. The new vtable slots return `HU_ERR_NOT_SUPPORTED` from the 42 other channel implementations.
- ❌ **NOT** silencing the existing flat-send path. Threaded reply is an *additional* path the reply-style predicate may choose; FLAT is always a valid output of the predicate.

## Risk register

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| AX role/title for "Reply…" differs across macOS minor versions | Medium | Match on prefix `"Reply"` (handles `Reply…` Unicode ellipsis, `Reply...` three-dot, locale variants); fall back to Cmd-R; fall back to flat send. |
| AX selection of a specific message row by guid fails (we currently match by `content_prefix`/`row_offset`) | Medium | Reuse the proven `ax_find_message_group` retry-loop from tapback. Plus: telemetry on AC-8 captures `tier_used` so we can see real-world degradation rate. |
| Custom-emoji picker is a separate AX sub-menu with locale-dependent labels | High | Match by Unicode codepoint of the AX child element's value/title, not by label string. If picker not found, fall back to nearest classic tapback. |
| Persona `thread_affinity` dial requires user education / sensible default | Low | Default `thread_affinity = 0.3` (real-world iMessage threading rate is ~20-35% per the user's own message history; spec for measurement in AC-2). |
| Sticker MVP looks like "just an attachment" to power users | Acceptable | Documented caveat. Indistinguishable to ≥80% of recipients. Real balloon-bundle stickers tracked as follow-up. |
| Predicate non-determinism breaks test reproducibility | Low | All RNG flows through an explicit `rng_seed` parameter. Tests pass deterministic seeds; production uses `arc4random()` per turn. |
| Latency jitter (AC-7) makes urgent replies feel slow | Medium | `persona.min_reply_delay_ms` is per-persona — Seth's persona can set this low; formal personas can set it higher. Telemetry surfaces whether jitter is working. |

## Out-of-scope follow-up specs (one-liners only)

- `2026-XX-imessage-forward` — Forward to another chat with chat-ID-validation guardrails.
- `2026-XX-imessage-delete` — Delete-within-window with explicit "are you sure" predicate.
- `2026-XX-imessage-sticker-native` — True `IMSticker` balloon-bundle send (blocked until macOS 26+ entitlement story changes).
- `2026-XX-imessage-action-surface-multichannel` — Once stable on iMessage, propagate `reply()` + extended `react()` to Telegram, Discord, Slack which all have threaded-reply primitives.
