---
title: "Human Fidelity Phase 1 — Foundation: Fix & Wire"
created: 2026-03-10
status: draft
scope: iMessage channel, daemon, conversation intelligence
phase: 1
features: [F1, F2, F4, F5, F6, F7, F10, F11, F15, F40, F41, F42, F43, F44]
parent: 2026-03-10-human-fidelity-design.md
---

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

# Human Fidelity Phase 1 — Foundation: Fix & Wire

Phase 1 of the Human Fidelity project. Fixes broken plumbing, wires message_id for tapbacks, adds auto-vision for photos, timing refinements, and platform feature investigations.

**Reference:** `docs/plans/2026-03-10-human-fidelity-design.md`

---

## Task 1: F1 — Wire message_id in iMessage poll loop

**Description:** The daemon already calls `ch->vtable->react()` with `msgs[batch_end].message_id`, but the iMessage poll loop never sets `message_id`. It remains -1 (or uninitialized), so tapbacks never fire.

**Files:**

- Modify: `src/channels/imessage.c` (poll loop, ~line 1174)
- Modify: `src/channels/imessage.c` (HU_IS_TEST mock branch, ~line 1049)
- Test: `tests/test_channel_all.c` or `tests/test_imessage.c`

**Steps:**

1. **Production poll loop:** In `hu_imessage_poll()`, after populating `msgs[count].content` and before `c->last_rowid = rowid`, add:

   ```c
   msgs[count].message_id = rowid;
   ```

2. **HU_IS_TEST mock:** In the mock poll branch, set `msgs[i].message_id` to a deterministic value (e.g. `(int64_t)(i + 1)`) so tests can verify tapback receives correct message_id.

3. **Verify daemon flow:** The daemon at `daemon.c:2928` uses `msgs[batch_end].message_id`. With batch_end pointing to the last message in a batch, ensure single-message batches get that message's rowid. No daemon changes needed.

**Tests:**

- Add test: inject mock message, poll, assert `msgs[0].message_id` is set (non-negative in prod path; in HU_IS_TEST use injectable message_id if we extend mock struct).
- For HU_IS_TEST: `hu_imessage_test_inject_mock` could accept optional message_id; or mock sets `message_id = 1` for first message. Simplest: mock sets `msgs[i].message_id = (int64_t)(i + 1)`.
- Test `hu_imessage_test_get_last_reaction` receives correct `last_reaction_message_id` when react is called.

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors
- Manual: run daemon with iMessage, send "lol" from contact; verify tapback is attempted (may still fail if JXA is broken — that's Task 2)

---

## Task 2: F1 (continued) — Rewrite tapback JXA to target specific message

**Description:** Current JXA opens AX context menu on the last row of the Messages table — unreliable and doesn't select tapback type or target the correct message. Investigate and implement a more reliable approach.

**Files:**

- Modify: `src/channels/imessage.c` (`imessage_react`, ~lines 647–767)
- Create: `scripts/imessage_tapback.applescript` (optional — if AppleScript approach works)
- Test: `tests/test_imessage.c` (HU_IS_TEST path unchanged; manual e2e)

**Steps:**

1. **Research:** iMessage tapbacks in chat.db use `associated_message_type`: 2000=love, 2001=like, 2002=dislike, 2003=laugh, 2004=emphasize, 2005=question. They reference the original via `associated_message_guid`. AppleScript Messages app does NOT expose "send tapback" — only `send` for text. Options:
   - **Option A:** Improve JXA: locate the specific message row by content/position, then trigger AXShowMenu and select tapback from menu. Requires mapping `message_id` (ROWID) to UI row — chat.db ROWID does not map 1:1 to table row order.
   - **Option B:** Use `message.guid` from chat.db: `SELECT guid FROM message WHERE ROWID = ?`. Pass guid to script; JXA may not have access to message objects by GUID.
   - **Option C:** Keep accessibility approach but fix row targeting: scroll to message, find row containing the text, then AXShowMenu. More robust than "last row" but still fragile.

2. **Implement:** Choose most reliable option. Recommendation: **Option C** — pass `message_id` and `content` (or content prefix) to JXA; query chat.db in JXA via `sqlite3` shell or Node.js `better-sqlite3` if available; or have C write a temp file with `message_id,content` and JXA reads it to find the right row. Simpler: pass content substring; JXA iterates table rows, finds row where `value` contains that substring, then performs AXShowMenu. Limitation: duplicate content could target wrong row.

3. **JXA changes:**
   - Accept `message_id` and `content` (or `content_prefix`) as arguments.
   - Replace "last row" logic with: iterate `win.tables()[0].rows()`, find row where `row.value()` or similar contains the message text (or use `rows()[row_index]` if we can derive index from ROWID — we cannot reliably).
   - Fallback: if no matching row found, use last row (current behavior).
   - After AXShowMenu, add `delay(0.5)` then use System Events to select the tapback menu item (e.g. "Love", "Like") — currently the script only opens the menu, doesn't select.

4. **AppleScript/JXA interface:** Current flow: `perl -e "alarm 15; exec @ARGV" osascript -l JavaScript -e "<script>`. Script receives tapback name and target. Extend to receive content substring for row matching.

**Tests:**

- HU_IS_TEST: `imessage_react` mock path unchanged — still records `last_reaction` and `last_reaction_message_id`.
- Manual: Full Disk Access + Accessibility; send message from contact; run human; verify tapback appears on correct message.

**Validation:**

- Tapback appears on the correct message (not always the last)
- All 6 tapback types (love, like, dislike, laugh, emphasize, question) work
- Graceful failure when Messages.app not in foreground or accessibility denied

---

## Task 3: F2 — Tapback-vs-type decision engine

**Description:** New classifier that decides: tapback only, text only, both, or no response. Replaces/adjusts the current flow where we always send both reaction and text when reaction != NONE.

**Files:**

- Create: `include/human/context/conversation.h` — add `hu_tapback_decision_t` enum and `hu_conversation_classify_tapback_decision()` declaration
- Modify: `src/context/conversation.c` — implement classifier
- Modify: `src/daemon.c` — use decision to gate reaction vs text vs both
- Modify: `include/human/persona.h` — add `tapback_style` to contact profile (optional for Phase 1; can use defaults)
- Test: `tests/test_conversation.c`

**Steps:**

1. **Add enum and function to conversation.h:**

   ```c
   typedef enum hu_tapback_decision {
       HU_TAPBACK_ONLY,      /* send tapback, no text */
       HU_TEXT_ONLY,         /* send text, no tapback */
       HU_TAPBACK_AND_TEXT,  /* both */
       HU_NO_RESPONSE,       /* neither (may still SKIP for other reasons) */
   } hu_tapback_decision_t;

   hu_tapback_decision_t hu_conversation_classify_tapback_decision(
       const char *message, size_t message_len,
       const hu_channel_history_entry_t *entries, size_t entry_count,
       const hu_contact_profile_t *contact, uint32_t seed);
   ```

2. **Implement classifier in conversation.c:**
   - Inputs: message content, history, contact (nullable), seed
   - Heuristics: short message (e.g. <15 chars) → tapback more likely; humor ("lol","haha") → haha tapback; agreement ("yeah","nice") → thumbs up; emotional → text preferred; question → text preferred
   - Use seed for probabilistic tie-breaking (e.g. 70% tapback for "lol")
   - Return HU_NO_RESPONSE only when message is truly skippable (e.g. "k", "ok") — otherwise prefer tapback or text
   - If contact has `tapback_style.frequency` (future), factor it in; for now use defaults

3. **Wire in daemon.c:**
   - Before calling `hu_conversation_classify_reaction`, call `hu_conversation_classify_tapback_decision`
   - If `HU_TAPBACK_ONLY`: call `react()`, skip LLM and text send
   - If `HU_TEXT_ONLY`: skip `react()`, do full LLM flow
   - If `HU_TAPBACK_AND_TEXT`: current behavior — react then LLM
   - If `HU_NO_RESPONSE`: treat as SKIP (or combine with existing HU_RESPONSE_SKIP logic)
   - Ensure `hu_conversation_classify_reaction` is only called when decision is TAPBACK_ONLY or TAPBACK_AND_TEXT

**Tests:**

- `hu_conversation_classify_tapback_decision("lol", 3, NULL, 0, NULL, 0)` → TAPBACK_ONLY or TAPBACK_AND_TEXT
- `hu_conversation_classify_tapback_decision("what time is dinner?", 20, NULL, 0, NULL, 0)` → TEXT_ONLY
- `hu_conversation_classify_tapback_decision("k", 1, NULL, 0, NULL, 0)` → NO_RESPONSE or TEXT_ONLY (brief)
- With history showing recent tapbacks, reduce tapback probability

**Validation:**

- Daemon sends tapback-only for appropriate short messages
- Daemon sends text-only for questions
- No double-send (tapback + redundant "lol") when TAPBACK_ONLY

---

## Task 4: F4 — Auto-vision pipeline for incoming photos

**Description:** When poll returns a message with an attachment (image), automatically run vision to describe it and inject into context. Currently vision uses `hu_imessage_get_latest_attachment_path(contact_id)` which is contact-scoped, not message-scoped. We need per-message attachment resolution.

**Files:**

- Modify: `src/channels/imessage.c` — extend poll to detect attachments; extend `hu_channel_loop_msg_t` or add attachment metadata
- Modify: `include/human/channel_loop.h` — add `has_attachment` or `attachment_message_id` to `hu_channel_loop_msg_t`
- Modify: `src/daemon.c` — use `message_id` from poll to call `hu_imessage_get_attachment_path(alloc, message_id)` for each message with attachment
- Test: `tests/test_imessage.c`, `tests/test_vision.c`

**Steps:**

1. **Poll: detect attachments**
   - Extend SQL to LEFT JOIN attachment tables:
     ```sql
     LEFT JOIN message_attachment_join maj ON maj.message_id = m.ROWID
     LEFT JOIN attachment a ON maj.attachment_id = a.ROWID
     ```
   - Add `a.filename` or `a.transfer_name` to SELECT to check extension
   - Image extensions: `.jpg`, `.jpeg`, `.png`, `.heic`, `.gif`, `.webp`
   - Set `msgs[count].has_attachment = true` when attachment exists and is image (or add `attachment_message_id = rowid` so daemon can fetch path)

2. **Channel loop struct:** Add to `hu_channel_loop_msg_t`:

   ```c
   bool has_attachment;  /* true if message has image attachment */
   /* message_id already exists — use it for hu_imessage_get_attachment_path */
   ```

3. **Daemon: per-message vision**
   - When processing batch, for each message with `has_attachment` and `message_id > 0`:
     - `path = hu_imessage_get_attachment_path(alloc, message_id)`
     - If path and provider supports vision: `hu_vision_describe_image(...)`
     - Build context: `"[They sent a photo: {description}]"`
   - Merge with `combined` or inject into `convo_ctx` before LLM
   - Current code uses `hu_imessage_get_latest_attachment_path(contact)` — replace with per-message `hu_imessage_get_attachment_path(message_id)` when we have message_id from poll

4. **Poll SQL change:** The main poll query filters `m.text IS NOT NULL AND LENGTH(m.text) > 0`. Photo-only messages have NULL or empty text. We need to either:
   - Include messages with attachments but no text (use placeholder "[Photo]") so they get processed
   - Or run a separate query for attachment-only messages. Simpler: extend poll to also return messages where `m.text IS NULL OR LENGTH(m.text)=0` but `m.ROWID` has attachment. Use `COALESCE(m.text, '[Photo]')` for content.

**Tests:**

- Mock: inject message with `has_attachment=true`, `message_id=123`; daemon path with HU_IS_TEST: `hu_imessage_get_attachment_path` returns NULL in test (no real DB). Add test that when `has_attachment` and provider supports vision, vision code path is exercised (mock provider).
- Integration: manual test with real photo message

**Validation:**

- Photo message triggers vision description
- Description is injected into prompt
- LLM responds naturally to photo content

---

## Task 5: F5 — Photo reaction decision classifier

**Description:** Extend tapback decision for photo context. When the message is a photo (with vision description), decide: heart tapback, haha tapback + text, love + warm response, etc.

**Files:**

- Modify: `src/context/conversation.c` — extend `hu_conversation_classify_tapback_decision` or add `hu_conversation_classify_photo_reaction`
- Modify: `include/human/context/conversation.h` — declaration
- Modify: `src/daemon.c` — when `has_attachment` and vision description exists, call photo reaction classifier
- Test: `tests/test_conversation.c`

**Steps:**

1. **Add function:**

   ```c
   hu_reaction_type_t hu_conversation_classify_photo_reaction(
       const char *vision_description, size_t desc_len,
       const hu_contact_profile_t *contact, uint32_t seed);
   ```

   Returns: HU_REACTION_HEART for sunset/family/selfie, HU_REACTION_HAHA for funny, HU_REACTION_NONE if text response preferred (e.g. screenshot with question)

2. **Heuristics:** Keyword matching on description: "sunset","landscape","nature" → heart; "funny","meme","silly" → haha; "food" → brief text preferred (no tapback); "screenshot" → depends on content

3. **Daemon:** When we have vision description and `has_attachment`, call `hu_conversation_classify_photo_reaction`. If non-NONE, consider tapback-first (or tapback+short text). Integrate with F2 tapback decision — for photos, F5 overrides reaction type.

**Tests:**

- "A beautiful sunset over the ocean" → HU_REACTION_HEART
- "A funny meme with text" → HU_REACTION_HAHA
- "A screenshot of an error message" → HU_REACTION_NONE

**Validation:**

- Photo of sunset gets heart tapback
- Funny photo gets haha tapback

---

## Task 6: F6 — Photo viewing delay

**Description:** When a message has an attachment (photo), add 3–8 seconds of "viewing time" before the response timer starts. Simulates actually looking at the photo.

**Files:**

- Modify: `src/daemon.c` — in the adaptive timing block (~line 1365), add attachment-aware delay

**Steps:**

1. **Detect photo in batch:** If any message in `msgs[batch_start..batch_end]` has `has_attachment`, set `photo_viewing_delay_ms = 3000 + (seed % 5000)` (3–8 seconds).

2. **Apply:** Add `photo_viewing_delay_ms` to `base_delay` (or to the initial sleep before LLM call). Scale with late-night multiplier if applicable.

3. **Config:** For now use constants. Future: persona `photo_viewing_delay_range: [3, 8]`.

**Tests:**

- Unit test: mock batch with `has_attachment=true`, verify delay is added (test via timing or injected callback)
- HU_IS_TEST: can assert that when attachment present, `extra_delay_ms` or equivalent is non-zero

**Validation:**

- Response to photo message is delayed by 3–8 seconds beyond normal

---

## Task 7: F7 — Video awareness

**Description:** Same as F4 but for video attachments. Options: extract first frame for vision, or just acknowledge ("let me watch this", "looks like a short video"). Add proportional viewing delay (2–10 seconds).

**Files:**

- Modify: `src/channels/imessage.c` — poll marks `has_video` or extend `has_attachment` to include video types
- Modify: `include/human/channel_loop.h` — add `has_video` or use `attachment_type` enum
- Modify: `src/daemon.c` — when `has_video`, add viewing delay; optionally run vision on first frame (requires ffmpeg or similar — may be Phase 2)
- Test: `tests/test_imessage.c`

**Steps:**

1. **Poll:** In attachment detection, add video extensions: `.mov`, `.mp4`, `.m4v`. Set `msgs[count].has_video = true` when attachment is video.

2. **Daemon:** When `has_video`:
   - Add 2–10 second viewing delay (proportional to estimated duration if available; otherwise 5 sec default)
   - Inject context: `"[They sent a video]"` or `"[They sent a short video]"` — no vision for video in Phase 1 (first-frame extraction is stretch)
   - LLM can respond with "let me watch this" or similar

3. **First-frame vision (optional):** If `ffmpeg` or `avfoundation` available, extract frame and run vision. Mark as optional — implement only if low effort.

**Tests:**

- Mock message with `has_video=true`; verify delay and context injection
- Manual: send video, verify acknowledgment

**Validation:**

- Video message gets viewing delay
- Response acknowledges video

---

## Task 8: F10 — Missed-message acknowledgment

**Description:** If response delay exceeds threshold (default 30 minutes), prepend acknowledgment: "sorry just saw this", "oh man missed this", "ha just woke up". Persona-driven phrasing.

**Files:**

- Modify: `src/daemon.c` — track message receive time vs response send time; prepend acknowledgment when delay exceeds threshold
- Modify: `include/human/persona.h` — add `missed_message_phrases` to persona (optional)
- Test: `tests/test_daemon.c` or new test file

**Steps:**

1. **Track receive time:** When poll returns messages, record `receive_time = time(NULL)` for the batch. Store in session or pass through to send path.

2. **At send time:** `delay_secs = time(NULL) - receive_time`. If `delay_secs > 30*60` (30 min) and not in natural late-night/early-morning gap:
   - Select phrase from persona `missed_message_phrases` or default: `["sorry just saw this", "oh man missed this", "ha just woke up"]`
   - Prepend to response: `"{phrase}\n\n{response}"`
   - Use seed to pick phrase

3. **Natural gap exception:** If receive was 2AM–6AM and now is 8AM, that's "just woke up" — use "ha just woke up" style. If receive was 11PM and now is 11:05PM, don't acknowledge (normal delay).

4. **Threshold:** Configurable; default 30 min. Persona: `missed_message_threshold_minutes: 30`.

**Tests:**

- Mock: receive_time = now - 45 min, verify prepended phrase
- Mock: receive_time = now - 5 min, verify no prepend
- Mock: receive at 2AM, respond at 8AM, verify "just woke up" style

**Validation:**

- Long-delayed responses get acknowledgment
- Short delays do not

---

## Task 9: F11 — Natural conversation drop-off refinement

**Description:** Refine `HU_RESPONSE_SKIP` logic for natural drop-off: after mutual farewell 90% silence; after low-energy exchange 60% silence; after emoji-only 70% silence; after our farewell with no reply 100% silence.

**Files:**

- Modify: `src/context/conversation.c` — extend `hu_conversation_classify_response` or add `hu_conversation_classify_dropoff`
- Modify: `src/daemon.c` — apply refined skip probabilities
- Test: `tests/test_conversation.c`

**Steps:**

1. **Add dropoff classifier or extend classify_response:**
   - Input: last incoming message, history (last 3–5 messages)
   - Cases:
     - Mutual farewell: both said "night"/"sleep well"/"bye" → return SKIP with 90% probability (use seed)
     - Low-energy: "yeah","cool","ok" from them → 60% SKIP
     - Emoji-only from them → 70% SKIP
     - Our farewell, they didn't reply → 100% SKIP (we already sent goodbye)
   - Integrate with existing `hu_conversation_classify_response` — it already returns HU_RESPONSE_SKIP for some cases. Add these as additional triggers with probabilities.

2. **Daemon:** When `action == HU_RESPONSE_SKIP` from these cases, don't override. When action is FULL/BRIEF but dropoff classifier says "likely skip", apply probability: e.g. `if (dropoff_skip_prob > 0 && (seed % 100) < dropoff_skip_prob) action = HU_RESPONSE_SKIP`.

3. **Track "our farewell":** If our last message was farewell-like ("night","bye","ttyl") and they replied with something minimal ("k","you too"), that's mutual farewell. If they didn't reply at all, we're done — next poll would be a new conversation.

**Tests:**

- History: them "night", us "night" → 90% SKIP
- History: them "yeah" → 60% SKIP
- History: them "👍" (emoji only) → 70% SKIP
- History: us "bye", them "k" → 90% SKIP

**Validation:**

- Conversation ends naturally after farewells
- No "lol" in response to "k" after "night"

---

## Task 10: F15 — Response length calibration

**Description:** Match response length to incoming message within ~1.5x ratio. Short message (1–20 chars) → short reply (1–30 chars). Already partially implemented via `hu_conversation_calibrate_length` and "brief mode". Extend with finer granularity.

**Files:**

- Modify: `src/context/conversation.c` — extend `hu_conversation_calibrate_length` or add ratio-based guidance
- Modify: `src/daemon.c` — ensure calibration is applied to `max_response_chars` and prompt
- Test: `tests/test_conversation.c`

**Steps:**

1. **Review current calibration:** `hu_conversation_calibrate_length` produces directive strings. Check if it already encodes length ratios. If not, add:
   - Short (1–20 chars): "1-30 chars"
   - Medium (20–100): "20-150 chars"
   - Long (100+): "proportional, max 300"

2. **Apply to max_response_chars:** Daemon uses `max_response_chars` for truncation/guidance. Set it from calibration: e.g. `max_response_chars = incoming_len * 3` capped at 300, or use calibration output to derive.

3. **Prompt injection:** `hu_conversation_build_awareness` already calls `hu_conversation_calibrate_length`. Ensure output is injected into system prompt. Verify in daemon flow.

4. **Quality gate:** `hu_conversation_evaluate_quality` may already penalize length mismatch. Ensure brevity score reflects ratio.

**Tests:**

- "k" (1 char) → calibration suggests 1-15 chars
- "what are you up to tonight?" (26 chars) → 20-80 chars
- Long paragraph → proportional

**Validation:**

- Response to "k" is short (not 200 words)
- Response to long message is proportional

---

## Task 11: F40 — Inline replies (reply to specific message)

**Description:** Reply to a specific earlier message in the thread. iMessage stores reply via `associated_message_guid` and `associated_message_type = 1`. Need to track GUIDs in poll and send reply with reference.

**Files:**

- Modify: `src/channels/imessage.c` — poll returns `associated_message_guid` for each message; add to `hu_channel_loop_msg_t`
- Modify: `include/human/channel_loop.h` — add `reply_to_guid[64]` or similar to `hu_channel_loop_msg_t`
- Modify: `src/channels/imessage.c` — `imessage_send` or new `imessage_send_reply` to send with reply context
- Modify: `src/context/conversation.c` — add `hu_conversation_should_inline_reply` classifier
- Modify: `src/daemon.c` — when classifier says inline reply, pass reply_to_guid to send
- Test: `tests/test_imessage.c`, `tests/test_conversation.c`

**Steps:**

1. **Poll SQL:** Add `m.guid` and `m.associated_message_guid` to SELECT. For incoming messages, `guid` is the message's own GUID. Store in `msgs[count].guid` (or `message_guid`).

2. **Channel loop struct:** Add `char reply_to_guid[96];` — when we want to reply to a specific message, we need the target message's GUID. The poll returns the incoming message — its `guid` is what we'd reference if we're replying TO it. So for each polled message, `msgs[i].guid` = that message's GUID. When we reply, we're replying to that message, so we pass `msgs[i].guid` as `reply_to_guid`.

3. **AppleScript:** Research: Does Messages AppleScript support "reply to message"? Standard `send` does not. Options:
   - **URL scheme:** `imessage://?action=reply&guid=...` — may not exist
   - **JXA:** Access Messages app, find message by GUID, use "reply" if available
   - **Fallback:** Prepend quoted text: `"> {original}\n\n{response}"` — works without platform support

4. **Classifier:** `hu_conversation_should_inline_reply(history, count, last_msg)` — true when: multiple questions in thread, or they referenced something from earlier, or conversation has multiple threads. Use heuristics: question count, "you said", "earlier", etc.

5. **Daemon:** When classifier says inline reply and we have `reply_to_guid`:
   - Try platform reply (if implemented)
   - Else: prepend `"> {quoted_message}\n\n"` to response. Get quoted_message from history (last incoming).

**Tests:**

- Classifier: multi-question history → true
- Classifier: single message → false
- Fallback: prepended quote format is correct

**Validation:**

- Inline reply appears as reply bubble on recipient's device (if platform supports)
- Fallback: quoted text appears in message

---

## Task 12: F41 — Message editing (investigation)

**Description:** Investigate feasibility of editing sent messages (iOS 16+ style) via AppleScript or private API. If not possible, document and keep `*correction` pattern.

**Files:**

- Create: `docs/investigations/imessage-edit-feasibility.md`
- No code changes until investigation complete

**Steps:**

1. **Research:**
   - AppleScript Messages dictionary: any "edit" or "replace" verb?
   - JXA: can we access sent message and modify?
   - Private frameworks: `IMCore`, `IMDaemonCore`, `IMDMessageStore` — do they expose edit? (Likely require private API, not suitable for human)
   - Community: imsg, imessage_tools, golift/imessage — any edit support?

2. **Document findings:**
   - If possible: outline API and implementation approach
   - If not: state "Not feasible via public API; retain \*correction pattern"
   - Risk: High — private API could break on macOS updates

**Validation:**

- Document complete with clear recommendation

---

## Task 13: F42 — Screen & bubble effects

**Description:** Send messages with iMessage effects (confetti, balloons, lasers). Triggered by keywords or explicit choice. Per-contact `effects_enabled` flag.

**Files:**

- Modify: `src/channels/imessage.c` — extend `imessage_send` to support effect parameter
- Modify: `include/human/channel.h` — extend send signature or add `send_with_effect`
- Modify: `src/context/conversation.c` — add `hu_conversation_classify_effect` (birthday → confetti, congrats → balloons)
- Modify: `src/daemon.c` — when effect classified, pass to send
- Test: `tests/test_imessage.c`

**Steps:**

1. **Research:** iMessage effects via AppleScript. Messages app may support:
   - `send "Happy birthday!" with effect` or similar
   - Or effects are triggered by keywords (e.g. "Happy birthday!" auto-triggers confetti)
   - Or: no AppleScript support — document as limitation

2. **If supported:** Add `effect` parameter to channel send (or new vtable method). Effects: `confetti`, `balloons`, `lasers`, `invisible_ink`, etc.

3. **Classifier:** `hu_conversation_classify_effect(msg, len)` → effect name or NULL. Triggers: "happy birthday" → confetti, "congratulations" → balloons, "pew pew" → lasers.

4. **Daemon:** When effect classified and contact has `effects_enabled`, call send with effect. Otherwise send normally.

**Tests:**

- "Happy birthday!" → confetti
- "congratulations!" → balloons
- HU_IS_TEST: send with effect records effect in mock

**Validation:**

- Effect appears on recipient's device (if platform supports)

---

## Task 14: F43 — Abandoned typing pattern (investigation)

**Description:** Investigate simulating "started typing then stopped" — the psychologically loaded iMessage behavior. iMessage typing indicator is not controllable via AppleScript (F3). Alternative: send then unsend? Investigate.

**Files:**

- Create: `docs/investigations/imessage-abandoned-typing-feasibility.md`
- No code changes until investigation complete

**Steps:**

1. **Research:**
   - Typing indicator: confirmed not in AppleScript
   - Send + unsend: does unsend exist? (F44)
   - Any other way to create "saw something then disappeared" effect?

2. **Document:**
   - Feasibility: likely not implementable
   - Mark as aspirational for future platform support

**Validation:**

- Document complete

---

## Task 15: F44 — Unsend (investigation)

**Description:** Investigate unsending a recently sent message via AppleScript or API. iOS 16+ / macOS Ventura+ support unsend. Frequency would be extremely rare (<0.5%).

**Files:**

- Create: `docs/investigations/imessage-unsend-feasibility.md`
- No code changes until investigation complete

**Steps:**

1. **Research:**
   - AppleScript: any "unsend" or "remove" verb for messages?
   - JXA: access last sent message, delete?
   - chat.db: deleting row would require write access and may not sync to iCloud

2. **Document:**
   - If possible: outline approach
   - If not: "Not feasible; skip implementation"

**Validation:**

- Document complete

---

## Implementation Order

Recommended sequence (dependencies first):

1. **Task 1** (F1 message_id) — unblocks tapback
2. **Task 2** (F1 tapback JXA) — depends on Task 1
3. **Task 4** (F4 auto-vision) — extends poll, unblocks F5/F6
4. **Task 3** (F2 tapback decision) — can parallel with Task 2
5. **Task 5** (F5 photo reaction) — depends on F4
6. **Task 6** (F6 photo delay) — depends on F4
7. **Task 7** (F7 video) — similar to F4
8. **Task 10** (F15 length calibration) — independent
9. **Task 9** (F11 drop-off) — independent
10. **Task 8** (F10 missed ack) — independent
11. **Task 11** (F40 inline reply) — more complex
12. **Task 13** (F42 effects) — depends on research
13. **Tasks 12, 14, 15** (investigations) — can run in parallel, no code

---

## Validation Matrix

Before considering Phase 1 complete:

| Check          | Command / Action                                                                                     |
| -------------- | ---------------------------------------------------------------------------------------------------- |
| Build          | `cmake -B build -DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_SQLITE=ON && cmake --build build -j$(nproc)` |
| Tests          | `./build/human_tests` — 0 failures, 0 ASan errors                                                  |
| Tapback        | Manual: send "lol", verify tapback on correct message                                                |
| Vision         | Manual: send photo, verify description in response                                                   |
| Length         | "k" → short reply                                                                                    |
| Drop-off       | "night" / "night" → no response                                                                      |
| Investigations | All 3 docs in `docs/investigations/`                                                                 |

---

## Risk Notes

- **Tapback JXA:** Accessibility-based approach is fragile across macOS versions. Consider documenting as "best effort."
- **Inline reply:** AppleScript may not support; fallback to quoted text is always available.
- **Effects:** May not be exposed in AppleScript; low impact if skipped.
