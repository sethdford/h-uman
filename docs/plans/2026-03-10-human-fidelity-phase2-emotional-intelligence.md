---
title: "Human Fidelity Phase 2 — Emotional Intelligence"
created: 2026-03-10
status: draft
scope: conversation intelligence, daemon, memory, persona
phase: 2
features: [F13, F14, F16, F17, F25, F27, F29, F33, F45, F46]
parent: 2026-03-10-human-fidelity-design.md
---

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

# Human Fidelity Phase 2 — Emotional Intelligence

Phase 2 of the Human Fidelity project. Makes human detect, respond to, and match emotional energy in conversations. Extends existing conversation classifiers, awareness builder, and memory infrastructure.

**Reference:** `docs/plans/2026-03-10-human-fidelity-design.md`

---

## Technical Context

### Key Files

| File                                     | Purpose                                                                     |
| ---------------------------------------- | --------------------------------------------------------------------------- |
| `src/context/conversation.c`             | Conversation classifiers, awareness builder (~2000 lines)                   |
| `include/human/context/conversation.h` | Conversation API declarations                                               |
| `src/daemon.c`                           | Main service loop (~4000 lines), orchestrates awareness + emotion injection |
| `src/memory/engines/sqlite.c`            | SQLite memory backend, schema initialization                                |
| `include/human/persona.h`              | Persona struct, contact profiles                                            |

### Existing Infrastructure

- `hu_conversation_build_awareness()` — builds conversation context; has basic emotional tone from punctuation (exclamation density)
- `hu_conversation_detect_emotion()` — returns `hu_emotional_state_t` (valence, intensity, concerning, dominant_emotion)
- `hu_conversation_classify_response()` — returns `HU_RESPONSE_SKIP` / `HU_RESPONSE_BRIEF` / `HU_RESPONSE_FULL` / `HU_RESPONSE_DELAY` / `HU_RESPONSE_THINKING`
- `hu_conversation_apply_typing_quirks()` — lowercase, no_periods, etc.
- `hu_conversation_apply_fillers()` — injects "haha", "lol", "yeah", "honestly", etc. (~20% chance)
- `hu_bth_metrics_t` — `emotions_surfaced` counter; daemon injects narrative/engagement/emotion when `emo_meaningful`
- `hu_contact_profile_t` — per-contact persona overlays
- Daemon at ~line 1930: calls `hu_conversation_detect_emotion()`, injects `- Emotional tone: %s` into insights when meaningful

### New SQLite Tables (from master design)

```sql
CREATE TABLE IF NOT EXISTS emotional_moments (
    id INTEGER PRIMARY KEY,
    contact_id TEXT NOT NULL,
    topic TEXT NOT NULL,
    emotion TEXT NOT NULL,
    intensity REAL,
    created_at INTEGER NOT NULL,
    follow_up_date INTEGER,
    followed_up INTEGER DEFAULT 0
);

CREATE TABLE IF NOT EXISTS comfort_patterns (
    contact_id TEXT NOT NULL,
    emotion TEXT NOT NULL,
    response_type TEXT NOT NULL,
    engagement_score REAL,
    sample_count INTEGER DEFAULT 0,
    PRIMARY KEY (contact_id, emotion, response_type)
);
```

### Persona JSON Additions

```json
"humanization": {
  "disfluency_frequency": 0.15,
  "backchannel_probability": 0.3,
  "burst_message_probability": 0.03
},
"context_modifiers": {
  "serious_topics_reduction": 0.4,
  "personal_sharing_warmth_boost": 1.6,
  "high_emotion_breathing_boost": 1.5,
  "early_turn_humanization_boost": 1.4
}
```

### voiceai Context Modifiers to Port

| Modifier                        | Value | Effect                                                      |
| ------------------------------- | ----- | ----------------------------------------------------------- |
| `serious_topics_reduction`      | 0.4   | Reduce humor by 60% for heavy topics                        |
| `personal_sharing_warmth_boost` | 1.6   | Boost warmth 60% for personal shares                        |
| `high_emotion_breathing_boost`  | 1.5   | Slow down for high emotion (shorter sentences, line breaks) |
| `early_turn_humanization_boost` | 1.4   | Warmer in first few messages                                |

---

## Task 1: F13 — Energy matching

**Description:** Detect emotional energy of incoming message and inject a matching directive into the prompt. Extend `hu_conversation_build_awareness()` / daemon flow with explicit energy-level detection and `[ENERGY: ...]` directive.

**Files:**

- Modify: `src/context/conversation.c`
- Modify: `include/human/context/conversation.h`
- Modify: `src/daemon.c` (merge energy directive into convo_ctx or insights)
- Test: `tests/test_conversation.c`

**Steps:**

1. **Add energy enum and detection function** in `conversation.h`:

   ```c
   typedef enum hu_energy_level {
       HU_ENERGY_NEUTRAL,
       HU_ENERGY_EXCITED,
       HU_ENERGY_SAD,
       HU_ENERGY_PLAYFUL,
       HU_ENERGY_ANXIOUS,
       HU_ENERGY_CALM,
   } hu_energy_level_t;

   hu_energy_level_t hu_conversation_detect_energy(const char *msg, size_t msg_len,
       const hu_channel_history_entry_t *entries, size_t count);
   ```

2. **Implement `hu_conversation_detect_energy()`** in `conversation.c`:
   - Use last incoming message + recent history
   - Map `hu_emotional_state_t` valence/intensity + keyword heuristics to energy level
   - Excited: positive valence, exclamation marks, "omg", "amazing", "love", "so happy"
   - Sad: negative valence, "sad", "depressed", "hurt", "lonely"
   - Playful: "lol", "haha", teasing phrases, light tone
   - Anxious: "worried", "stressed", "anxious", "scared"
   - Calm: low intensity, neutral valence
   - Default: HU_ENERGY_NEUTRAL

3. **Add energy directive builder** in `conversation.c`:

   ```c
   size_t hu_conversation_build_energy_directive(hu_energy_level_t energy,
       char *buf, size_t cap);
   ```

   Output examples:
   - `[ENERGY: They're excited. Match their energy. Be enthusiastic.]`
   - `[ENERGY: They're down. Be gentle, shorter, empathetic. No jokes.]`
   - `[ENERGY: They're playful. Match the playfulness.]`
   - `[ENERGY: They're anxious. Be calm, reassuring, grounding.]`

4. **Wire in daemon:** After `hu_conversation_detect_emotion()`, call `hu_conversation_detect_energy()`. If not HU_ENERGY_NEUTRAL, append energy directive to insights buffer (or convo_ctx). Increment `bth_metrics->emotions_surfaced` when energy directive injected.

**Tests:**

- `hu_conversation_detect_energy` with "omg that's amazing!!" → HU_ENERGY_EXCITED
- `hu_conversation_detect_energy` with "i'm so sad today" → HU_ENERGY_SAD
- `hu_conversation_detect_energy` with "lol you're ridiculous" → HU_ENERGY_PLAYFUL
- `hu_conversation_detect_energy` with "i'm really worried about this" → HU_ENERGY_ANXIOUS
- `hu_conversation_detect_energy` with "ok sounds good" → HU_ENERGY_NEUTRAL
- `hu_conversation_build_energy_directive` produces non-empty string for each non-NEUTRAL level

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 2: F14 — Emotional escalation detection

**Description:** Track emotional trajectory across multiple messages. When 3+ messages show increasing negative sentiment, switch to de-escalation mode (shorter, more empathetic, fewer jokes).

**Files:**

- Modify: `src/context/conversation.c`
- Modify: `include/human/context/conversation.h`
- Modify: `src/daemon.c`
- Test: `tests/test_conversation.c`

**Steps:**

1. **Add escalation detection** in `conversation.h`:

   ```c
   typedef struct hu_escalation_state {
       bool escalating;
       int consecutive_negative;
       float trajectory;  /* negative = worsening */
   } hu_escalation_state_t;

   hu_escalation_state_t hu_conversation_detect_escalation(
       const hu_channel_history_entry_t *entries, size_t count);
   ```

2. **Implement `hu_conversation_detect_escalation()`** in `conversation.c`:
   - Iterate last 6–8 their messages (from_me=false)
   - For each, compute valence via existing emotion keyword logic (or call `hu_conversation_detect_emotion` on sliding window)
   - Track: if valence[i] < valence[i-1] for 3+ consecutive their messages → escalating
   - Reset if they say "i'm fine", "just kidding", "lol", "haha" (explicit reset signals)
   - Set `escalating = (consecutive_negative >= 3)`

3. **Add de-escalation directive builder**:

   ```c
   size_t hu_conversation_build_deescalation_directive(char *buf, size_t cap);
   ```

   Output: `[DE-ESCALATION: Their mood is escalating negatively. Be shorter, more empathetic, fewer jokes. "hey you okay?" energy.]`

4. **Wire in daemon:** After emotion/energy, if `escalation.escalating`, append de-escalation directive to insights. This overrides energy matching (de-escalation takes precedence).

**Tests:**

- 3 messages: "i'm stressed" → "it's getting worse" → "i can't deal" → escalating=true
- 3 messages then "lol jk" → escalating=false (reset)
- 2 negative messages → escalating=false
- Mixed positive/negative → escalating=false

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 3: F16 — Context modifiers (ported from voiceai)

**Description:** Reduce humor for heavy topics, boost warmth for personal sharing, slow down for high emotion, warmer in first few messages.

**Files:**

- Modify: `src/context/conversation.c`
- Modify: `include/human/context/conversation.h`
- Modify: `src/persona/persona.c` (parse humanization/context_modifiers from JSON)
- Modify: `include/human/persona.h` (add `hu_humanization_config_t` or extend persona)
- Test: `tests/test_conversation.c`, `tests/test_persona.c`

**Steps:**

1. **Define context modifier struct** in `persona.h` or new `include/human/context/context_modifiers.h`:

   ```c
   typedef struct hu_context_modifiers {
       float serious_topics_reduction;      /* 0.4 = reduce humor 60% */
       float personal_sharing_warmth_boost; /* 1.6 = boost warmth 60% */
       float high_emotion_breathing_boost;   /* 1.5 = slow down */
       float early_turn_humanization_boost; /* 1.4 = warmer early */
   } hu_context_modifiers_t;
   ```

2. **Add topic/heavy detection** in `conversation.c`:
   - Heavy topics: death, illness, job loss, divorce, breakup, cancer, funeral, diagnosis
   - Keywords: "died", "passed", "cancer", "divorce", "fired", "laid off", "diagnosis", "funeral", "breakup", "lost my job"

3. **Add personal sharing detection**:
   - "i need to tell you", "can i be honest", "don't judge me", "this is hard to say", "i never told", "first time i'm saying", "i've been meaning to tell"

4. **Add early-turn detection**: count exchanges; if ≤3, early turn.

5. **Implement modifier directive builder**:

   ```c
   size_t hu_conversation_build_context_modifiers(
       const hu_channel_history_entry_t *entries, size_t count,
       const hu_emotional_state_t *emo,
       const hu_context_modifiers_t *mods,
       char *buf, size_t cap);
   ```

   Output (concatenate applicable lines):
   - `[CONTEXT: Heavy topic detected. Reduce humor by 60%.]`
   - `[CONTEXT: They're sharing something personal. Boost warmth 60%.]`
   - `[CONTEXT: High emotion. Use shorter sentences, more line breaks.]`
   - `[CONTEXT: Early in conversation. Be warmer, more human.]`

6. **Persona JSON parsing:** Add `context_modifiers` to persona load. Defaults: 0.4, 1.6, 1.5, 1.4.

7. **Wire in daemon:** Call `hu_conversation_build_context_modifiers()` with persona modifiers (or defaults). Append to convo_ctx/insights.

**Tests:**

- Heavy topic "my dad passed last week" → serious_topics_reduction applied
- Personal "can i be honest, i've been struggling" → personal_sharing_warmth_boost
- High emotion (emo.intensity > 1.0) → high_emotion_breathing_boost
- count <= 6, exchanges <= 3 → early_turn_humanization_boost

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 4: F17 — First-time vulnerability detection

**Description:** Detect when someone shares something personal for the first time. Memory check: has this person ever mentioned [topic] before? If first time: respond with extra care, weight, acknowledgment. "that's huge. thanks for telling me." not "here's what you should do."

**Files:**

- Modify: `src/context/conversation.c`
- Modify: `include/human/context/conversation.h`
- Modify: `src/memory/` — add topic-first-time query (or use existing memory search)
- Modify: `include/human/memory.h` or memory backend
- Modify: `src/daemon.c`
- Test: `tests/test_conversation.c`, `tests/test_memory.c`

**Steps:**

1. **Add vulnerability topic extraction** in `conversation.c`:
   - Categories: illness, job_loss, divorce, family_issue, mental_health, loss
   - Keywords per category: illness→"diagnosis","cancer","sick","hospital"; job_loss→"fired","laid off","lost my job"; etc.

2. **Add memory query** in `src/memory/` (sqlite or common layer):
   - `bool hu_memory_has_topic_before(memory, contact_id, topic_category, session_id)` — search memories for contact + topic; return true if any prior mention.
   - Use FTS or key prefix like `contact:${contact_id}:topic:${category}`.

3. **Implement first-time vulnerability detection**:

   ```c
   typedef struct hu_vulnerability_state {
       bool first_time;
       const char *topic_category;
       float intensity;
   } hu_vulnerability_state_t;

   hu_vulnerability_state_t hu_conversation_detect_first_time_vulnerability(
       const char *msg, size_t msg_len,
       const hu_channel_history_entry_t *entries, size_t count,
       hu_memory_t *memory, const char *contact_id, size_t contact_id_len);
   ```

4. **Add directive**:
   - `[VULNERABILITY: First time they've shared this. Extra care. Acknowledge weight. Don't pivot to advice. "that's huge. thanks for telling me."]`

5. **Wire in daemon:** When memory available and contact_id known, call detection. If first_time, append directive. Optionally store new memory for topic so future calls return false.

**Tests:**

- Message "i got diagnosed with X" + no prior memory for topic → first_time=true
- Same message + prior memory for topic → first_time=false
- Non-vulnerability message → first_time=false, topic_category=NULL

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 5: F25 — Emotional check-ins (1–3 days later)

**Description:** After someone shares something stressful/sad/difficult, schedule a check-in 1–3 days later. "hey how are you doing with the [stressful thing]?" One check-in per emotional moment.

**Files:**

- Create: `src/memory/emotional_moments.c` (or extend sqlite.c)
- Modify: `src/memory/engines/sqlite.c` — add emotional_moments table to schema
- Modify: `include/human/memory.h` or new `include/human/memory/emotional_moments.h`
- Modify: `src/daemon.c` — record moments, check due check-ins in proactive cycle
- Test: `tests/test_emotional_moments.c` or `tests/test_memory.c`

**Steps:**

1. **Add SQLite table** in `sqlite.c` schema_parts:

   ```c
   "CREATE TABLE IF NOT EXISTS emotional_moments ("
   "id INTEGER PRIMARY KEY,"
   "contact_id TEXT NOT NULL,"
   "topic TEXT NOT NULL,"
   "emotion TEXT NOT NULL,"
   "intensity REAL,"
   "created_at INTEGER NOT NULL,"
   "follow_up_date INTEGER,"
   "followed_up INTEGER DEFAULT 0)",
   "CREATE INDEX IF NOT EXISTS idx_emotional_moments_contact ON emotional_moments(contact_id)",
   "CREATE INDEX IF NOT EXISTS idx_emotional_moments_follow_up ON emotional_moments(follow_up_date)",
   ```

2. **Add API** in `emotional_moments.c` or memory module:

   ```c
   hu_error_t hu_emotional_moment_record(hu_allocator_t *alloc, hu_memory_t *memory,
       const char *contact_id, size_t contact_id_len,
       const char *topic, size_t topic_len,
       const char *emotion, size_t emotion_len,
       float intensity);

   hu_error_t hu_emotional_moment_get_due(hu_allocator_t *alloc, hu_memory_t *memory,
       int64_t now_ts, hu_emotional_moment_t **out, size_t *out_count);

   hu_error_t hu_emotional_moment_mark_followed_up(hu_memory_t *memory, int64_t id);
   ```

3. **Record logic:** When daemon detects emotional moment (F13/F14/F17 — high negative emotion, vulnerability, or escalation), call `hu_emotional_moment_record`. Set `follow_up_date = now + random(1,3) days` (86400*1 to 86400*3 seconds).

4. **Proactive check:** In `hu_service_run_proactive_checkins` or daemon cron, call `hu_emotional_moment_get_due`. For each due moment, inject check-in prompt: "You had an emotional moment with [contact] about [topic] 1–3 days ago. Send a natural check-in: 'hey how are you doing with [topic]?'" Send via channel. Call `hu_emotional_moment_mark_followed_up`.

5. **Deduplication:** Before recording, query if same contact+topic already has unfollowed moment in last 7 days — skip to avoid duplicates.

**Tests:**

- Record moment, advance time, get_due returns it
- Mark followed_up, get_due no longer returns it
- Same contact+topic within 7 days → no duplicate record

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 6: F27 — Comfort pattern learning

**Description:** Learn what helps when someone is down. Track: when contact expressed negative emotion, what response type did they engage with? Categories: distraction, empathy, space, advice. Apply learned preference when negative emotion detected.

**Files:**

- Modify: `src/memory/engines/sqlite.c` — add comfort_patterns table
- Create: `src/memory/comfort_patterns.c` (or extend sqlite)
- Modify: `include/human/memory.h` or new header
- Modify: `src/daemon.c` — record engagement after response, apply pattern when building prompt
- Modify: `src/context/conversation.c` — add comfort directive builder
- Test: `tests/test_comfort_patterns.c`

**Steps:**

1. **Add SQLite table** in `sqlite.c`:

   ```c
   "CREATE TABLE IF NOT EXISTS comfort_patterns ("
   "contact_id TEXT NOT NULL,"
   "emotion TEXT NOT NULL,"
   "response_type TEXT NOT NULL,"
   "engagement_score REAL,"
   "sample_count INTEGER DEFAULT 0,"
   "PRIMARY KEY (contact_id, emotion, response_type))",
   ```

2. **Add API**:

   ```c
   hu_error_t hu_comfort_pattern_record(hu_memory_t *memory,
       const char *contact_id, size_t contact_id_len,
       const char *emotion, size_t emotion_len,
       const char *response_type, size_t response_type_len,
       float engagement_score);

   hu_error_t hu_comfort_pattern_get_preferred(hu_allocator_t *alloc, hu_memory_t *memory,
       const char *contact_id, size_t contact_id_len,
       const char *emotion, size_t emotion_len,
       char **out_type, size_t *out_len);
   ```

3. **Response type classification:** After we send a response to negative emotion, classify our response: distraction (humor, topic change), empathy (reflection, validation), space (brief ack), advice (suggestions). Use keyword heuristics or simple rules.

4. **Engagement scoring:** On next their message, compute engagement: did they reply? length? positive words? Simple: reply_len > 20 + positive sentiment → engagement_score 0.8; brief "thanks" → 0.4; no reply (different turn) → 0.2. Running average per (contact, emotion, response_type).

5. **Apply pattern:** When building prompt and emotion is negative, call `hu_comfort_pattern_get_preferred`. If we have a preferred type with sample_count >= 2, inject: `[COMFORT: This contact tends to respond well to [distraction/empathy/space/advice] when [emotion].]`

6. **Default:** Until pattern learned, use empathy (empathetic listening).

**Tests:**

- Record 3 distraction engagements, get_preferred returns "distraction"
- No data → get_preferred returns NULL
- Different emotions → separate patterns

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 7: F29 — Active listening cues (backchannels)

**Description:** Text equivalents of "mmhm", "yeah", "totally", "right", "100%", "for real". Standalone responses when the other person is telling a story. Configurable probability (default 30%).

**Files:**

- Modify: `src/context/conversation.c`
- Modify: `include/human/context/conversation.h`
- Modify: `src/daemon.c`
- Modify: `src/persona/persona.c` — parse humanization.backchannel_probability
- Test: `tests/test_conversation.c`

**Steps:**

1. **Add narrative/venting detection:** Message is narrative if: length > 80 chars, no question mark, contains "and then", "so i", "anyway", "long story", first person ("i", "my", "me"). Or: last 2–3 their messages are long and we haven't replied yet (they're in flow).

2. **Add backchannel classifier**:

   ```c
   bool hu_conversation_should_backchannel(const char *msg, size_t msg_len,
       const hu_channel_history_entry_t *entries, size_t count,
       uint32_t seed, float probability);
   ```

   Returns true if: narrative detected AND probability roll passes (seed-based).

3. **Add backchannel response action:** Extend `hu_response_action_t` or add branch:
   - New enum value `HU_RESPONSE_BACKCHANNEL` or handle in classify: when should_backchannel, return HU_RESPONSE_BRIEF with special flag.
   - Simpler: in classify_response, when narrative + roll, return HU_RESPONSE_BRIEF and set a flag `use_backchannel=true` (or new field in a struct).

4. **Backchannel phrases:** Static list: "yeah", "totally", "right", "100%", "for real", "mmhm", "mhm", "oh wow", "damn". Pick one at random (seed).

5. **Wire in daemon:** When classify returns backchannel path, send only the backchannel phrase (no LLM call). Saves cost and ensures brevity.

6. **Persona:** Read `humanization.backchannel_probability` (default 0.3). Pass to should_backchannel.

**Tests:**

- Long narrative message, probability 1.0 → should_backchannel true
- Short "k" → should_backchannel false
- Narrative + probability 0.0 → false

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 8: F33 — Text disfluency

**Description:** Natural text imperfections: "I mean", "like", "you know what I mean", trailing "...", self-correction "wait no I meant". Configurable frequency (default 15%). Casual conversations only.

**Files:**

- Modify: `src/context/conversation.c`
- Modify: `include/human/context/conversation.h`
- Modify: `src/persona/persona.c` — parse humanization.disfluency_frequency
- Modify: `src/daemon.c` — apply after LLM response, before send
- Test: `tests/test_conversation.c`

**Steps:**

1. **Add disfluency application function**:

   ```c
   size_t hu_conversation_apply_disfluency(char *buf, size_t len, size_t cap,
       uint32_t seed, float frequency, const hu_contact_profile_t *contact);
   ```

2. **Implement:**
   - Skip if contact has relationship_type "coworker" or formality "formal"
   - Roll with frequency (seed). If pass:
     - 40%: prepend "i mean " or "like " at start
     - 30%: append "..." or " you know" at end (before period)
     - 20%: insert "wait no " or "actually " mid-sentence (after first clause)
     - 10%: self-correction fragment "\*meant X" (reuse correction logic style)
   - Ensure buffer has capacity; truncate if needed.

3. **Wire in daemon:** After `hu_conversation_apply_typos` / `hu_conversation_apply_fillers`, call `hu_conversation_apply_disfluency`. Pass persona humanization.disfluency_frequency (default 0.15).

4. **Order of operations:** quirks → fillers → disfluency → typos (or similar). Document in conversation.c.

**Tests:**

- frequency 1.0, casual contact → at least one disfluency applied
- frequency 0.0 → no change
- formal contact → no disfluency
- Buffer capacity edge cases

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 9: F45 — Burst messaging

**Description:** Send 3–4 independent thoughts in rapid succession. "oh my god" → "just saw the news" → "are you okay" → "call me". Different from message splitting (one thought); each is a new thought. For high-energy moments. Inter-burst delay 1–3 seconds.

**Files:**

- Modify: `src/context/conversation.c`
- Modify: `include/human/context/conversation.h`
- Modify: `src/daemon.c`
- Test: `tests/test_conversation.c`

**Steps:**

1. **Add burst detection:** When incoming message suggests urgency/excitement: "omg", "oh my god", "just saw", "are you okay", "call me", "emergency", "holy shit", exclamation-heavy. And energy is HU_ENERGY_EXCITED or HU_ENERGY_ANXIOUS (urgent).

2. **Add burst decision**:

   ```c
   bool hu_conversation_should_burst(const char *msg, size_t msg_len,
       const hu_channel_history_entry_t *entries, size_t count,
       uint32_t seed, float probability);
   ```

   Returns true if: burst context + probability roll (default 0.03 from persona).

3. **Burst generation:** When should_burst, prompt LLM differently:
   - System addition: "Generate 3–4 short, independent messages as a burst reaction. Output as JSON array: [\"msg1\", \"msg2\", \"msg3\"]. Each message 2–10 words. Rapid-fire thoughts."
   - Or: single prompt "Generate 3 burst messages" and parse response.
   - Simpler: use existing chat, but ask for structured output. Parse JSON array from response.

4. **Daemon:** When burst, send each message with 1–3 second delay between (use existing delay machinery). Don't split one long message — each bubble is separate.

5. **Persona:** humanization.burst_message_probability (default 0.03).

**Tests:**

- "omg did you see the news" + probability 1.0 → should_burst true
- "what's for dinner" → should_burst false
- Burst sends 3+ messages with delays

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 10: F46 — Leave on read (deliberate)

**Description:** Intentionally not responding as a social signal. When: after disagreement, when needing space, when message doesn't require response but they expect one. Classify as HU_RESPONSE_LEAVE_ON_READ — don't respond for 2–24 hours, then respond normally.

**Files:**

- Modify: `src/context/conversation.c`
- Modify: `include/human/context/conversation.h`
- Modify: `src/daemon.c`
- Test: `tests/test_conversation.c`

**Steps:**

1. **Add enum value** to `hu_response_action_t`:

   ```c
   HU_RESPONSE_LEAVE_ON_READ = 5,
   ```

2. **Add classifier logic** in `hu_conversation_classify_response()`:
   - Disagreement: recent exchange had conflicting views (they disagreed with us, or we with them)
   - Needing space: they said "i need space", "give me a minute", "can we talk later"
   - Message doesn't require response: statement, no question, not emotional, e.g. "ok", "cool", "sure" — but they might expect reply. Low probability.
   - Very rare: <2% of messages. Use seed-based roll.
   - When triggered: return HU_RESPONSE_LEAVE_ON_READ, store `leave_on_read_until_ts` in daemon state (per contact).

3. **Daemon handling:** When HU_RESPONSE_LEAVE_ON_READ:
   - Set `leave_on_read_until = now + random(2, 24) hours` for this contact.
   - Skip response this cycle.
   - On subsequent polls, if new message from same contact before leave_on_read_until, still skip.
   - After leave_on_read_until, respond to next message normally (or to backlog).
   - Store in static array keyed by contact_id (similar to consec_response_count).

4. **Edge cases:** Don't leave on read for emotional crisis ("i need you", "help me"). Don't leave on read for direct questions. Prefer false negatives (respond when unsure).

**Tests:**

- "we'll have to agree to disagree" + seed → sometimes HU_RESPONSE_LEAVE_ON_READ
- "what do you think?" → never HU_RESPONSE_LEAVE_ON_READ
- Daemon skips response when HU_RESPONSE_LEAVE_ON_READ
- After delay, daemon responds to next message

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 11: Persona JSON humanization schema

**Description:** Add humanization and context_modifiers to persona JSON parsing. Ensure all Phase 2 features can read config.

**Files:**

- Modify: `src/persona/persona.c`
- Modify: `include/human/persona.h`
- Test: `tests/test_persona.c`

**Steps:**

1. **Add to persona struct** (or extend):

   ```c
   typedef struct hu_humanization_config {
       float disfluency_frequency;
       float backchannel_probability;
       float burst_message_probability;
   } hu_humanization_config_t;

   /* In hu_persona_t: */
   hu_humanization_config_t humanization;
   hu_context_modifiers_t context_modifiers;
   ```

2. **Parse from JSON** in persona load:
   - `humanization.disfluency_frequency` default 0.15
   - `humanization.backchannel_probability` default 0.3
   - `humanization.burst_message_probability` default 0.03
   - `context_modifiers.serious_topics_reduction` default 0.4
   - `context_modifiers.personal_sharing_warmth_boost` default 1.6
   - `context_modifiers.high_emotion_breathing_boost` default 1.5
   - `context_modifiers.early_turn_humanization_boost` default 1.4

3. **Add getters** if needed: `hu_persona_get_humanization()`, `hu_persona_get_context_modifiers()`.

**Tests:**

- Load persona JSON with humanization block, assert values
- Load without humanization, assert defaults

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Validation Matrix

| Check   | Command                                                                                                                              |
| ------- | ------------------------------------------------------------------------------------------------------------------------------------ |
| Build   | `cmake -B build -DHU_ENABLE_ALL_CHANNELS=ON -DHU_ENABLE_SQLITE=ON -DHU_ENABLE_PERSONA=ON && cmake --build build -j$(nproc)`          |
| Tests   | `./build/human_tests`                                                                                                              |
| ASan    | 0 errors in test output                                                                                                              |
| Release | `cmake -B build-release -DCMAKE_BUILD_TYPE=MinSizeRel -DHU_ENABLE_LTO=ON -DHU_ENABLE_ALL_CHANNELS=ON && cmake --build build-release` |

---

## Dependency Order

1. Task 11 (Persona schema) — no deps
2. Task 1 (F13 Energy) — no deps
3. Task 2 (F14 Escalation) — uses emotion detection
4. Task 3 (F16 Context modifiers) — uses Task 11
5. Task 4 (F17 First-time vulnerability) — uses memory
6. Task 5 (F25 Emotional check-ins) — uses SQLite, Tasks 1–2 for detection
7. Task 6 (F27 Comfort patterns) — uses SQLite
8. Task 7 (F29 Backchannels) — no deps
9. Task 8 (F33 Disfluency) — uses Task 11
10. Task 9 (F45 Burst) — uses Task 1 energy
11. Task 10 (F46 Leave on read) — no deps

Recommended execution order: 11 → 1 → 2 → 3 → 7 → 8 → 10 → 5 → 6 → 4 → 9
