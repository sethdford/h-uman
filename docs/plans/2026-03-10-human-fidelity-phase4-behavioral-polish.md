---
title: "Human Fidelity Phase 4 — Behavioral Polish & Context"
created: 2026-03-10
status: draft
scope: daemon, conversation intelligence, proactive, memory, persona, iMessage
phase: 4
features: [F8, F9, F12, F28, F32, F47, F48, F49, F51, F52, F54, F55, F56, F57]
parent: 2026-03-10-human-fidelity-design.md
---

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

# Human Fidelity Phase 4 — Behavioral Polish & Context

Phase 4 of the Human Fidelity project. The finishing touches that make conversations feel completely natural: delayed follow-ups, double-text patterns, bookend messages, linguistic mirroring, style consistency, context awareness (weather, sports, time zones), group chat intelligence, and multi-thread energy management.

**Reference:** `docs/plans/2026-03-10-human-fidelity-design.md`

---

## Technical Context

### Key Files

| File                          | Purpose                                                               |
| ----------------------------- | --------------------------------------------------------------------- |
| `src/daemon.c`                | Main service loop (~4000 lines), proactive checks, timing, poll cycle |
| `src/context/conversation.c`  | Classifiers, awareness builder, group classifiers                     |
| `src/channels/imessage.c`     | iMessage channel, poll, send                                          |
| `src/agent/proactive.c`       | Proactive messaging, check-ins                                        |
| `src/memory/engines/sqlite.c` | SQLite backend, schema                                                |

### Existing Infrastructure

- Proactive check cycle runs hourly with jitter (0–30 min)
- `hu_conversation_classify_group()` — returns `HU_GROUP_SKIP` / `HU_GROUP_RESPOND` / `HU_GROUP_BRIEF`
- `hu_conversation_build_awareness()` — injects conversation context into prompt
- `hu_channel_loop_msg_t` — `session_key`, `content`, `is_group`, `message_id`
- Per-contact overlays: `formality`, `emoji_usage`, `style_notes`
- `hu_proactive_check_silence()`, `hu_proactive_check_events()` — proactive action types

### New SQLite Tables (from master design)

```sql
CREATE TABLE IF NOT EXISTS style_fingerprints (
    contact_id TEXT NOT NULL PRIMARY KEY,
    uses_lowercase INTEGER DEFAULT 0,
    uses_periods INTEGER DEFAULT 0,
    laugh_style TEXT,
    avg_message_length INTEGER,
    common_phrases TEXT,
    distinctive_words TEXT,
    updated_at INTEGER
);

CREATE TABLE IF NOT EXISTS delayed_followups (
    id INTEGER PRIMARY KEY,
    contact_id TEXT NOT NULL,
    topic TEXT NOT NULL,
    scheduled_at INTEGER NOT NULL,
    sent INTEGER DEFAULT 0
);
```

### Persona JSON Additions

```json
"follow_up_style": {
  "delayed_follow_up_probability": 0.15,
  "min_delay_minutes": 20,
  "max_delay_hours": 4
},
"bookend_messages": {
  "enabled": true,
  "morning_window": [7, 9],
  "evening_window": [22, 23],
  "frequency_per_week": 2.5,
  "phrases_morning": ["morning min"],
  "phrases_evening": ["night"]
},
"context_awareness": {
  "weather_enabled": false,
  "sports_teams": [],
  "news_topics": []
}
```

---

## Task 1: F8 — Delayed follow-up engine

**Description:** After a conversation ends (no new messages for 15+ minutes), probabilistically schedule a follow-up. Topics come from memory (unresolved questions, emotional moments, shared plans). Delay: 20 min–4 hrs. Check `delayed_followups` table each daemon poll cycle.

**Files:**

- Modify: `src/memory/engines/sqlite.c` — add `delayed_followups` table to schema
- Create: `src/memory/delayed_followup.c` (or extend sqlite.c)
- Create: `include/human/memory/delayed_followup.h` (or `memory.h` extension)
- Modify: `src/daemon.c` — schedule follow-ups on conversation end; check due follow-ups each poll
- Modify: `src/agent/proactive.c` — add `HU_PROACTIVE_DELAYED_FOLLOW_UP` action type
- Modify: `src/persona/persona.c` — parse `follow_up_style` from persona
- Test: `tests/test_delayed_followup.c`, `tests/test_daemon.c`

**Steps:**

1. **Add SQLite table** in `sqlite.c` schema_parts:

   ```c
   "CREATE TABLE IF NOT EXISTS delayed_followups ("
   "id INTEGER PRIMARY KEY,"
   "contact_id TEXT NOT NULL,"
   "topic TEXT NOT NULL,"
   "scheduled_at INTEGER NOT NULL,"
   "sent INTEGER DEFAULT 0)",
   "CREATE INDEX IF NOT EXISTS idx_delayed_followups_scheduled ON delayed_followups(scheduled_at)",
   "CREATE INDEX IF NOT EXISTS idx_delayed_followups_contact ON delayed_followups(contact_id)",
   ```

2. **Add API** in `delayed_followup.c` or memory module:

   ```c
   hu_error_t hu_delayed_followup_schedule(hu_allocator_t *alloc, hu_memory_t *memory,
       const char *contact_id, size_t contact_id_len,
       const char *topic, size_t topic_len,
       int64_t scheduled_at);

   hu_error_t hu_delayed_followup_get_due(hu_allocator_t *alloc, hu_memory_t *memory,
       int64_t now_ts, hu_delayed_followup_t **out, size_t *out_count);

   hu_error_t hu_delayed_followup_mark_sent(hu_memory_t *memory, int64_t id);
   ```

3. **Conversation-end detection:** When daemon last poll returned messages for contact X, and subsequent poll(s) return no new messages for X for 15+ minutes, consider conversation ended. Store `last_message_time` per contact.

4. **Schedule logic:** On conversation end, if `(seed % 100) < (probability * 100)` (default 15%). Extract topic from last N messages via memory or LLM extraction (or simple keyword heuristic). Compute `scheduled_at = now + random(min_delay_minutes, max_delay_hours)` in seconds. Call `hu_delayed_followup_schedule`.

5. **Daemon poll check:** Each poll cycle (or each cron tick), call `hu_delayed_followup_get_due`. For each due follow-up:
   - Build prompt: "You were thinking about what [contact] said about [topic]. Send a natural follow-up."
   - Send via channel.
   - Call `hu_delayed_followup_mark_sent`.

6. **Persona:** Parse `follow_up_style.delayed_follow_up_probability`, `min_delay_minutes`, `max_delay_hours`. Defaults: 0.15, 20, 4.

**Tests:**

- Schedule follow-up, advance time, get_due returns it
- Mark sent, get_due no longer returns it
- Probability 0 → never schedule
- Probability 1.0 + roll → schedule

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 2: F9 — Double-text pattern

**Description:** After sending a response, 5–10% chance of sending a second message 10–45 seconds later. "oh and" / "wait actually" / "lol i just realized". Not message splitting — a genuinely new thought. Never after farewell or in late-night hours.

**Files:**

- Modify: `src/daemon.c` — after send, schedule double-text timer
- Modify: `src/context/conversation.c` — add `hu_conversation_should_double_text`
- Modify: `include/human/context/conversation.h`
- Modify: `src/persona/persona.c` — parse `humanization.double_text_probability`
- Test: `tests/test_conversation.c`, `tests/test_daemon.c`

**Steps:**

1. **Add double-text decision** in `conversation.h`:

   ```c
   bool hu_conversation_should_double_text(const char *last_response, size_t resp_len,
       const hu_channel_history_entry_t *entries, size_t count,
       uint8_t hour_local, uint32_t seed, float probability);
   ```

2. **Implement in conversation.c:**
   - Return false if last_response is farewell-like ("night", "bye", "ttyl", "sleep well")
   - Return false if hour_local >= 23 or hour_local <= 5 (late night)
   - Return false if probability roll fails
   - Higher energy = more likely: if conversation has exclamation marks, "lol", "omg" → boost probability by 1.5x
   - Default probability: 0.08 (5–10% range)

3. **Daemon:** After successful send, if `hu_conversation_should_double_text`:
   - Sleep 10–45 seconds (random with seed)
   - Build prompt: "You just said: [last_response]. Add a natural afterthought (oh and, wait actually, lol i just realized). One short sentence."
   - Send second message via channel

4. **State:** Store pending double-text per contact: `double_text_timeout_ts`, `contact_id`, `last_response`. Daemon main loop must check: if `now >= double_text_timeout_ts`, fire and clear.

5. **Persona:** `humanization.double_text_probability` default 0.08.

**Tests:**

- Farewell response → should_double_text false
- Late night (23:00) → false
- High-energy message + probability 1.0 → true
- Probability 0 → false

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 3: F12 — Morning/evening bookends

**Description:** For contacts with `bookend_messages: true`, send "morning min" / "night" phatic messages 2–3x/week. Morning: 7–9 AM with jitter. Evening: 22–23 with jitter. Skip if conversation that day; skip if contact hasn't messaged in >48 hrs.

**Files:**

- Modify: `src/agent/proactive.c` — add `hu_proactive_check_bookends`
- Modify: `include/human/agent/proactive.h` — add `HU_PROACTIVE_BOOKEND`

  ```c
  HU_PROACTIVE_BOOKEND_MORNING,
  HU_PROACTIVE_BOOKEND_EVENING,
  ```

- Modify: `src/daemon.c` — call bookend check in proactive cycle
- Modify: `src/persona/persona.c` — parse `bookend_messages` per contact
- Modify: `include/human/persona.h` — add `hu_bookend_config_t`
- Test: `tests/test_proactive.c`

**Steps:**

1. **Add bookend config struct** in persona.h:

   ```c
   typedef struct hu_bookend_config {
       bool enabled;
       uint8_t morning_start;   /* 7 */
       uint8_t morning_end;    /* 9 */
       uint8_t evening_start;  /* 22 */
       uint8_t evening_end;   /* 23 */
       float frequency_per_week; /* 2.5 */
       char **phrases_morning;
       size_t phrases_morning_count;
       char **phrases_evening;
       size_t phrases_evening_count;
   } hu_bookend_config_t;
   ```

2. **Add `hu_proactive_check_bookends`** in proactive.c:

   ```c
   hu_error_t hu_proactive_check_bookends(hu_allocator_t *alloc,
       const hu_contact_profile_t *cp,
       uint64_t last_contact_ms, uint64_t now_ms,
       uint8_t hour_local, uint32_t day_of_week,
       const hu_bookend_config_t *config,
       hu_proactive_result_t *out);
   ```

   - If !config->enabled, return HU_OK
   - If last_contact_ms < 48 hrs ago: skip (respect their silence)
   - Track per-contact: last_bookend_ts (morning/evening). Store in kv or SQLite.
   - Frequency: 2.5/week → ~0.36 per day. Roll: if (seed % 100) < 36 and in window, fire.
   - Morning window: hour_local in [7,9], pick random minute within window
   - Evening window: hour_local in [22,23]
   - Pick phrase from config->phrases_morning or phrases_evening

3. **Wire in daemon:** In `hu_service_run_proactive_checkins`, for each contact with bookend enabled, call `hu_proactive_check_bookends`. If action returned, send via channel.

4. **Skip if conversation today:** Query memory or last_contact: if last message from contact was today, skip bookend (already talked).

**Tests:**

- Disabled config → no action
- Last contact 50 hrs ago → may fire
- Last contact 24 hrs ago → skip

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 4: F28 — Linguistic mirroring

**Description:** Mirror contact's distinctive phrases/words 20% of the time. If Mindy says "legit" often, use it 20%. Track `distinctive_words` per contact in memory; inject into prompt.

**Files:**

- Modify: `src/context/conversation.c` — add `hu_conversation_should_mirror_word`, `hu_conversation_build_mirror_directive`
- Modify: `include/human/context/conversation.h`
- Modify: `src/memory/` — store/retrieve distinctive words per contact (or use style_fingerprints.distinctive_words)
- Modify: `src/daemon.c` — inject mirror directive when building awareness
- Modify: `src/context/style_tracker.c` (create if needed) — extract distinctive words from their messages
- Test: `tests/test_conversation.c`

**Steps:**

1. **Distinctive word extraction:** From last N messages from contact, count word frequency. Words that appear >2x and are not common stopwords → distinctive. Store in `style_fingerprints.distinctive_words` (JSON array).

2. **Add mirror directive builder** in conversation.c:

   ```c
   size_t hu_conversation_build_mirror_directive(hu_allocator_t *alloc,
       const char *distinctive_words_json, size_t json_len,
       uint32_t seed, float probability,
       char *buf, size_t cap);
   ```

   - Parse JSON array of words
   - If (seed % 100) >= (probability \* 100), return 0
   - Output: `[VOCAB: Occasionally use their words: legit, vibes, lowkey. Use 1–2 of these naturally.]`

3. **Wire in daemon:** When building awareness, if style_fingerprint has distinctive_words for contact, call `hu_conversation_build_mirror_directive`. Append to convo_ctx. Default probability: 0.2.

4. **Persona:** `humanization.vocabulary_mirroring_probability` default 0.2.

**Tests:**

- Empty distinctive_words → no directive
- Probability 0 → no directive
- Probability 1.0 + words → directive contains words

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 5: F32 — Style consistency enforcement

**Description:** Track and enforce consistent texting style per contact. If human used "haha" in last 5 messages, don't switch to "lol". If lowercase, don't capitalize. If no periods, don't add them. Store in `style_fingerprints`; inject into prompt.

**Files:**

- Create: `src/context/style_tracker.c`
- Create: `include/human/context/style_tracker.h`
- Modify: `src/memory/engines/sqlite.c` — add `style_fingerprints` table
- Modify: `src/daemon.c` — update fingerprint after send; inject before LLM
- Modify: `src/context/conversation.c` — integrate with existing `hu_conversation_apply_typing_quirks`
- Test: `tests/test_style_tracker.c`

**Steps:**

1. **Add SQLite table** in sqlite.c:

   ```c
   "CREATE TABLE IF NOT EXISTS style_fingerprints ("
   "contact_id TEXT NOT NULL PRIMARY KEY,"
   "uses_lowercase INTEGER DEFAULT 0,"
   "uses_periods INTEGER DEFAULT 0,"
   "laugh_style TEXT,"
   "avg_message_length INTEGER,"
   "common_phrases TEXT,"
   "distinctive_words TEXT,"
   "updated_at INTEGER)",
   ```

2. **Add style tracker API** in style_tracker.c:

   ```c
   typedef struct hu_style_fingerprint {
       bool uses_lowercase;
       bool uses_periods;
       char *laugh_style;  /* "haha", "lol", "ha", "lmao" */
       int avg_message_length;
       char *common_phrases_json;
       char *distinctive_words_json;
   } hu_style_fingerprint_t;

   hu_error_t hu_style_fingerprint_update(hu_allocator_t *alloc, hu_memory_t *memory,
       const char *contact_id, size_t contact_id_len,
       const char *message, size_t message_len,
       bool from_me);

   hu_error_t hu_style_fingerprint_get(hu_allocator_t *alloc, hu_memory_t *memory,
       const char *contact_id, size_t contact_id_len,
       hu_style_fingerprint_t *out);

   void hu_style_fingerprint_deinit(hu_style_fingerprint_t *fp, hu_allocator_t *alloc);
   ```

3. **Update logic:** When message is from_me (our response), analyze: lowercase ratio, period usage, laugh type (regex: "haha|lol|ha|lmao|hehe"). Update running average for last N messages. Upsert into style_fingerprints.

4. **Directive builder:**

   ```c
   size_t hu_style_fingerprint_build_directive(const hu_style_fingerprint_t *fp,
       char *buf, size_t cap);
   ```

   Output: `[STYLE: Your recent style with this contact: lowercase, no periods, 'haha' not 'lol'. Match it.]`

5. **Wire in daemon:** Before LLM call, get fingerprint for contact. If present, append directive to convo_ctx. After send, call `hu_style_fingerprint_update` with our response.

**Tests:**

- Update with "haha that's great" → laugh_style "haha", uses_lowercase true
- Get fingerprint, build directive → contains style hints

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 6: F47 — Content forwarding (HIGH COMPLEXITY)

**Description:** "look what mom sent me" style sharing. Proactive behavior triggered by memory associations. When memory surfaces a connection between content and a contact → share it. Requires source of shareable content (saved links, memory-tagged content).

**Complexity:** High. Requires content ingestion pipeline, memory tagging, and association logic.

**Files:**

- Create: `src/memory/content_forwarding.c`
- Modify: `src/memory/engines/sqlite.c` — add `shareable_content` table (or extend memories)
- Modify: `src/daemon.c` — proactive check for content associations
- Modify: `src/agent/proactive.c` — add `HU_PROACTIVE_CONTENT_FORWARD`
- Modify: `src/persona/persona.c` — parse `content_forwarding.enabled` per contact
- Test: `tests/test_content_forwarding.c`

**Steps:**

1. **Content ingestion:** Define shareable content. Options:
   - A) Manual: user adds links to persona JSON `saved_links: [{url, title, tags}]`
   - B) Memory-tagged: memories with `category=shareable` or `tags=["shareable"]`
   - C) Tool: `save_for_later` tool stores URL + summary in memory

2. **Add table** (if using dedicated storage):

   ```sql
   CREATE TABLE IF NOT EXISTS shareable_content (
       id INTEGER PRIMARY KEY,
       url TEXT,
       title TEXT,
       summary TEXT,
       tags TEXT,
       contact_id TEXT,
       created_at INTEGER
   );
   ```

3. **Association logic:** For each contact with `content_forwarding.enabled`, scan shareable content. Match by: tags in contact interests, keyword overlap with contact recent_topics. Pick one with probability per proactive cycle.

4. **Proactive action:** Build prompt: "You have [content]. Share it naturally with [contact]: 'look what mom sent me' style. Include link or summary."

5. **Rate limit:** Max 1 content forward per contact per week.

**Tests:**

- No shareable content → no action
- Content matches contact interests → may fire
- Rate limit exceeded → no action

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 7: F48 — Meme/image sharing (STRETCH GOAL — investigation)

**Description:** Proactively find and send relevant memes/images. "this is literally you" + relevant meme. Requires image search (Giphy, Google Images, or curated library).

**Status:** Stretch goal. Investigation task only.

**Files:**

- Create: `docs/investigations/meme-image-sharing-feasibility.md`
- No code changes until investigation complete

**Steps:**

1. **Research:**
   - Giphy API: free tier, search by keyword, returns GIF URL
   - Tenor API: similar
   - Google Custom Search API: requires API key, rate limits
   - Curated meme library: static JSON of URLs keyed by topic — no external API

2. **Document findings:**
   - API choice, cost, rate limits
   - How to map conversation topic → search query
   - iMessage attachment format (GIF vs image)
   - Privacy: avoid sending inappropriate content
   - Recommendation: Giphy free tier or curated library for Phase 4

3. **If implemented later:** New module `src/context/meme_finder.c`, `hu_meme_find(alloc, topic, &url_out)`. Daemon calls in proactive cycle when topic matches.

**Validation:**

- Document complete with clear recommendation

---

## Task 8: F49 — "Call me" escalation

**Description:** Know when text isn't enough. "hey can you call me?" / "this is too much to type lol call me when you get a sec". Classifier: message complexity + emotional intensity → escalation score. If > threshold: suggest a call. Never actually place a call.

**Files:**

- Modify: `src/context/conversation.c` — add `hu_conversation_should_escalate_to_call`
- Modify: `include/human/context/conversation.h`
- Modify: `src/daemon.c` — when classifier says escalate, prompt LLM to suggest call
- Test: `tests/test_conversation.c`

**Steps:**

1. **Add escalation classifier** in conversation.h:

   ```c
   typedef struct hu_call_escalation {
       bool should_suggest;
       float score;
   } hu_call_escalation_t;

   hu_call_escalation_t hu_conversation_should_escalate_to_call(
       const char *msg, size_t msg_len,
       const hu_channel_history_entry_t *entries, size_t count);
   ```

2. **Implement heuristics:**
   - Emotional crisis: "i need you", "help me", "can't deal", "breaking down" → high score
   - Complex topic: message length > 200 chars, multiple questions, "it's complicated"
   - Emotional intensity from `hu_conversation_detect_emotion` > 0.8
   - Score = weighted sum. Threshold 0.6 → suggest call

3. **Daemon:** When `should_suggest`, add to prompt: "This conversation may be better suited for a call. Suggest they call you. Don't actually place a call. Example: 'hey can you call me when you get a sec? this is too much to type lol'"

4. **Never escalate:** For logistics, quick questions, casual chat. Prefer false negatives.

**Tests:**

- "i need you right now" → should_suggest true
- "what's for dinner" → should_suggest false
- Long emotional message → may suggest

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 9: F51 — Weather/location context

**Description:** "stay dry out there" when it's raining in contact's location. Ambient awareness. Use OpenWeatherMap API via libcurl. Contact location stored in persona JSON.

**Files:**

- Create: `src/context/weather.c`
- Create: `include/human/context/weather.h`
- Modify: `src/daemon.c` — fetch weather when building proactive/awareness for contact with location
- Modify: `src/persona/persona.c` — parse `location` or `city` per contact
- Modify: `include/human/persona.h` — add `location` to contact profile
- Modify: `CMakeLists.txt` — gate weather behind `HU_ENABLE_CURL`
- Test: `tests/test_weather.c` (HU_IS_TEST mock)

**Steps:**

1. **Add contact location:** In `hu_contact_profile_t`, add `char *location` (city name or "lat,lon"). Parse from persona JSON `contacts[].location`.

2. **Weather API** in weather.c:

   ```c
   typedef struct hu_weather_info {
       char *description;  /* "light rain", "clear" */
       float temp_c;
       int humidity;
       bool notable;      /* storm, extreme heat, etc. */
   } hu_weather_info_t;

   hu_error_t hu_weather_fetch(hu_allocator_t *alloc, const char *location,
       size_t location_len, hu_weather_info_t *out);

   void hu_weather_info_deinit(hu_weather_info_t *info, hu_allocator_t *alloc);
   ```

   - OpenWeatherMap: `https://api.openweathermap.org/data/2.5/weather?q={city}&appid={key}&units=metric`
   - Requires `HU_WEATHER_API_KEY` env or config. HU_IS_TEST: return mock.

3. **Notable weather:** Storm, heavy rain, snow, extreme heat (>35°C), extreme cold (<-10°C) → notable=true

4. **Inject into prompt:** When building awareness for contact with location and `context_awareness.weather_enabled`, call `hu_weather_fetch`. If notable, append: `[WEATHER: It's [description] in their area. Mention naturally if relevant: "stay dry out there", "how's the snow up there?"]`

5. **Cache:** Cache weather per location for 30 min to avoid API spam.

**Tests:**

- HU_IS_TEST: hu_weather_fetch returns mock without network
- Notable weather → inject directive

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 10: F52 — Sports/current events

**Description:** Shared-world topical awareness. "did you see the game?!" after a big game. "crazy about [news event]". RSS or news API for configurable topics. Per-contact interests in persona.

**Files:**

- Create: `src/context/current_events.c`
- Create: `include/human/context/current_events.h`
- Modify: `src/daemon.c` — fetch events when building proactive context
- Modify: `src/persona/persona.c` — parse `context_awareness.sports_teams`, `news_topics`
- Test: `tests/test_current_events.c` (HU_IS_TEST mock)

**Steps:**

1. **Config:** Persona `context_awareness.sports_teams: ["Lakers", "49ers"]`, `news_topics: ["tech", "ai"]`

2. **Event source:** Options:
   - A) RSS: parse RSS feeds for sports/news (libcurl + simple XML parser)
   - B) News API: newsapi.org (requires key)
   - C) Static: curated list of "major events" — manual update

3. **API** in current_events.c:

   ```c
   typedef struct hu_current_event {
       char *title;
       char *source;
       char *topic;  /* "sports", "tech", etc. */
   } hu_current_event_t;

   hu_error_t hu_current_events_fetch(hu_allocator_t *alloc,
       const char *const *topics, size_t topic_count,
       hu_current_event_t **out, size_t *out_count);

   void hu_current_events_free(hu_allocator_t *alloc,
       hu_current_event_t *events, size_t count);
   ```

4. **HU_IS_TEST:** Return mock events. No network.

5. **Inject:** When building proactive prompt for contact with sports_teams, fetch events. If topic matches, append: `[EVENTS: Notable: [title]. Mention naturally if relevant.]`

6. **Rate limit:** Fetch once per day, cache.

**Tests:**

- HU_IS_TEST: returns mock
- Topic match → inject

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 11: F54 — Time zone awareness

**Description:** Adjust behavior for contacts in different time zones. Store `timezone` in contact profile. Late-night delays based on CONTACT's local time. "oh wait it's like 2am there isn't it, sorry" when messaging across zones.

**Files:**

- Modify: `include/human/persona.h` — add `timezone` to `hu_contact_profile_t`
- Modify: `src/persona/persona.c` — parse `timezone` (e.g. "America/Denver")
- Create: `src/context/timezone.c` (or use platform utils)
- Modify: `src/daemon.c` — convert all timing to contact local time when applicable
- Modify: `src/context/conversation.c` — inject timezone awareness when time delta is large
- Test: `tests/test_timezone.c`, `tests/test_persona.c`

**Steps:**

1. **Add timezone to contact:** `char *timezone` in `hu_contact_profile_t`. Parse from persona JSON.

2. **Timezone utility:**

   ```c
   int hu_timezone_offset_hours(const char *tz_name, size_t tz_len, int64_t now_ts);
   int hu_timezone_hour_local(const char *tz_name, size_t tz_len, int64_t now_ts);
   ```

   - Use `tzset`, `mktime` with `tm.tm_isdst`, or platform-specific. Fallback: assume UTC if unknown.

3. **Daemon timing:** When computing "late night" for contact, use `hu_timezone_hour_local(contact->timezone, ...)` instead of local machine hour. Apply delays based on contact's hour.

4. **Awareness injection:** When our hour is 14:00 and contact's hour is 02:00, inject: `[TIMEZONE: It's 2am for them. Keep it brief, avoid long messages. "oh wait it's like 2am there isn't it, sorry" if appropriate.]`

5. **Bookends:** Morning/evening bookends should fire at contact's local 7–9 AM, 22–23. So schedule based on contact timezone.

**Tests:**

- "America/Denver" at UTC noon → hour_local ~5 or 6
- Unknown timezone → fallback to UTC

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 12: F55 — Group chat lurking

**Description:** Read everything, rarely contribute. Track per-group response frequency. Default: respond only when directly addressed or mentioned. Occasionally chime in on high-relevance topics. "Group lurker" mode: respond to <10% of messages.

**Files:**

- Modify: `src/context/conversation.c` — extend `hu_conversation_classify_group`
- Modify: `src/daemon.c` — use group response rate from contact config
- Modify: `include/human/persona.h` — add `group_response_rate` to contact (default 0.1)
- Test: `tests/test_conversation.c`

**Steps:**

1. **Add contact config:** `float group_response_rate` (0.0–1.0). Default 0.1 = respond to 10% of eligible messages.

2. **Extend classify_group:** Current logic: HU_GROUP_SKIP, HU_GROUP_RESPOND, HU_GROUP_BRIEF. Add probability layer:
   - When would have returned HU_GROUP_RESPOND (direct @, question): always respond
   - When would have returned HU_GROUP_BRIEF: apply probability. If (seed % 100) >= (group_response_rate \* 100), return HU_GROUP_SKIP instead
   - When would have returned HU_GROUP_SKIP: stay SKIP

3. **Track per-group:** Store `group_id` -> `response_count`, `message_count` in session or kv. If we've responded to >10% of last 20 messages in this group, increase skip probability.

4. **High-relevance override:** If message contains topic from contact's interests and we haven't responded in 5+ messages, allow brief response with higher probability.

**Tests:**

- group_response_rate 0 → always SKIP when not directly addressed
- group_response_rate 1.0 → respond to all BRIEF cases
- Direct @ → always RESPOND

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 13: F56 — Group chat @ mentions

**Description:** When responding in a group, optionally address specific people. "@ Mindy what do you think?" Uses inline reply (F40) or name mention. Group members stored in contact profiles.

**Files:**

- Modify: `src/context/conversation.c` — add `hu_conversation_should_mention_in_group`
- Modify: `include/human/context/conversation.h`
- Modify: `src/daemon.c` — when group, pass group members to prompt; LLM can @ mention
- Modify: `include/human/persona.h` — add `group_members` to contact (for group sessions)
- Modify: `src/channels/imessage.c` — extend poll to return group members for group chats
- Test: `tests/test_conversation.c`

**Steps:**

1. **Group members:** When `is_group` in poll, session_key may encode group. Need to resolve group_id → member names. Options:
   - A) Persona JSON: `groups: { "group_id": { "members": ["Mindy", "Alex"] } }`
   - B) chat.db: group chat has participants

2. **Classifier:** When we're about to respond in group and message was from specific person (or addressed to group), decide: should we @ mention someone?
   - When asking "what do you think?" → @ the person who asked or most relevant
   - When replying to multi-person thread → @ the person we're addressing

3. **Prompt injection:** `[GROUP: Members: Mindy, Alex. You can @ mention: "@ Mindy" to address them.]`

4. **iMessage @:** Format may be "@Mindy" or "Mindy" — check iMessage conventions. May need AppleScript to send with mention.

5. **Simplest:** Inject member names into prompt. LLM generates "@ Mindy what do you think?" — we send as-is. iMessage may auto-link @ mentions.

**Tests:**

- Group with 2 members → prompt contains member names
- Non-group → no group directive

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 14: F57 — Multi-thread energy management

**Description:** Maintain different tones across simultaneous conversations. Track active conversation contexts in daemon. Each conversation has its own energy/tone state. Don't leak tone from one conversation to another.

**Files:**

- Modify: `src/daemon.c` — per-conversation context isolation
- Modify: `src/context/conversation.c` — add `hu_conversation_energy_state_t` per session
- Modify: `include/human/context/conversation.h`
- Test: `tests/test_daemon.c`, `tests/test_conversation.c`

**Steps:**

1. **Per-session state:** Store for each `session_key` (contact):
   - `energy_level` (from F13)
   - `escalation_state` (from F14)
   - `last_tone` (serious, casual, playful, etc.)

2. **Context isolation:** When building prompt for contact A, use ONLY session A's state. Don't mix in session B's emotion or energy.

3. **Daemon structure:** Already processes messages per contact in batch. Ensure `convo_ctx`, `insights`, `bth_metrics` are built per session, not reused across sessions. Audit: any static or shared buffer that could leak?

4. **State update:** After each response, update session's energy_state, last_tone from the conversation. Store in `consec_contact_keys`-style array or similar.

5. **Explicit reset:** When switching from contact A to B in same poll cycle, clear any temporary state. Use separate `hu_conversation_build_awareness` call per session with fresh context.

**Tests:**

- Two contacts: A serious, B casual. Respond to A → serious tone. Respond to B → casual tone. No cross-contamination.

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 15: Persona JSON schema extensions

**Description:** Add all Phase 4 persona fields to parsing. Ensure `follow_up_style`, `bookend_messages`, `context_awareness`, `humanization.double_text_probability`, `group_response_rate`, `timezone`, `location` are parsed.

**Files:**

- Modify: `src/persona/persona.c`
- Modify: `include/human/persona.h`
- Test: `tests/test_persona.c`

**Steps:**

1. **Add structs** in persona.h:

   ```c
   typedef struct hu_follow_up_style {
       float delayed_follow_up_probability;
       short min_delay_minutes;
       short max_delay_hours;
   } hu_follow_up_style_t;

   typedef struct hu_bookend_config {
       bool enabled;
       uint8_t morning_window[2];
       uint8_t evening_window[2];
       float frequency_per_week;
       char **phrases_morning;
       size_t phrases_morning_count;
       char **phrases_evening;
       size_t phrases_evening_count;
   } hu_bookend_config_t;

   typedef struct hu_context_awareness {
       bool weather_enabled;
       char **sports_teams;
       size_t sports_teams_count;
       char **news_topics;
       size_t news_topics_count;
   } hu_context_awareness_t;
   ```

2. **Add to contact profile:** `hu_follow_up_style_t follow_up_style`, `hu_bookend_config_t bookend_messages`, `hu_context_awareness_t context_awareness`, `timezone`, `location`, `group_response_rate`.

3. **Add to humanization:** `double_text_probability`.

4. **Parse from JSON** with defaults. Document schema in persona JSON example.

**Tests:**

- Load persona with all Phase 4 fields, assert values
- Load without, assert defaults

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Implementation Order

Recommended sequence (dependencies first):

1. **Task 15** (Persona schema) — no deps
2. **Task 1** (F8 Delayed follow-up) — uses SQLite, schema
3. **Task 4** (F28 Linguistic mirroring) — uses style_fingerprints
4. **Task 5** (F32 Style consistency) — uses style_fingerprints, creates table
5. **Task 3** (F12 Bookends) — uses Task 15
6. **Task 2** (F9 Double-text) — uses Task 15
7. **Task 8** (F49 Call escalation) — independent
8. **Task 11** (F54 Time zone) — uses Task 15
9. **Task 9** (F51 Weather) — uses Task 15, curl
10. **Task 10** (F52 Sports/events) — uses Task 15
11. **Task 12** (F55 Group lurking) — uses Task 15
12. **Task 13** (F56 Group @ mentions) — uses group logic
13. **Task 14** (F57 Multi-thread energy) — uses conversation state
14. **Task 6** (F47 Content forwarding) — high complexity, do last
15. **Task 7** (F48 Meme investigation) — stretch, no code

---

## Validation Matrix

| Check             | Command / Action                                                                                                                                |
| ----------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| Build             | `cmake -B build -DHU_ENABLE_ALL_CHANNELS=ON -DHU_ENABLE_SQLITE=ON -DHU_ENABLE_PERSONA=ON -DHU_ENABLE_CURL=ON && cmake --build build -j$(nproc)` |
| Tests             | `./build/human_tests` — 0 failures, 0 ASan errors                                                                                             |
| Release           | `cmake -B build-release -DCMAKE_BUILD_TYPE=MinSizeRel -DHU_ENABLE_LTO=ON -DHU_ENABLE_ALL_CHANNELS=ON && cmake --build build-release`            |
| F48 Investigation | `docs/investigations/meme-image-sharing-feasibility.md` exists                                                                                  |

---

## Risk Notes

- **F47 Content forwarding:** High complexity. Requires content ingestion pipeline. Consider Phase 4.1 if scope creep.
- **F51 Weather:** Requires API key. Document in config. HU_IS_TEST must mock.
- **F52 Events:** Requires RSS/API. May add dependency. Consider minimal static fallback.
- **F48 Meme sharing:** Stretch goal. Investigation only; no implementation in Phase 4.
