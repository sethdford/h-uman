---
title: "Human Fidelity Phase 6 — AGI Cognition Layer"
created: 2026-03-10
status: draft
scope: context, persona, memory, daemon, conversation intelligence, protective intelligence
phase: 6
features: [F58, F59, F60, F61, F62, F63, F64, F65, F66, F67, F68, F69]
parent: 2026-03-10-human-fidelity-design.md
---

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

# Human Fidelity Phase 6 — AGI Cognition Layer

Phase 6 of the Human Fidelity project. The deep intelligence features that make human pass the **close friend test** — where someone who's known Seth for 30 years can't tell the difference. Theory of Mind, parallel life simulation, mood persistence, imperfect recall, self-awareness, reciprocity, anticipatory modeling, opinion evolution, narrative self, social network model, protective intelligence, and humor principles.

**Reference:** `docs/plans/2026-03-10-human-fidelity-design.md`

---

## Technical Context

### Key Files

| File                                   | Purpose                                                             |
| -------------------------------------- | ------------------------------------------------------------------- |
| `src/daemon.c`                         | Main service loop, proactive checks, awareness injection, send flow |
| `src/context/conversation.c`           | Emotion detection, awareness builder, classifiers                   |
| `src/memory/engines/sqlite.c`          | Schema, migrations, memory storage                                  |
| `src/persona/persona.c`                | Persona JSON parsing, contact profiles                              |
| `include/human/persona.h`              | Persona structs, contact profile                                    |
| `include/human/context/conversation.h` | Awareness, emotion, engagement APIs                                 |

### Existing Infrastructure

- `hu_conversation_build_awareness()` — injects conversation context into prompt
- `hu_conversation_detect_emotion()` — returns `hu_emotional_state_t` (valence, intensity, concerning, dominant_emotion)
- `hu_conversation_detect_engagement()` — engagement level
- `hu_channel_history_entry_t` — message entries with `from_me`, `content`, timestamps
- Proactive check cycle runs hourly with jitter
- Persona JSON: `contacts`, `core`, overlays
- SQLite schema_parts array for table creation

### Naming Convention

- Functions/variables: `snake_case`
- Types: `hu_<name>_t` (e.g., `hu_contact_baseline_t`)
- Constants: `HU_SCREAMING_SNAKE`
- Public functions: `hu_<module>_<action>` (e.g., `hu_theory_of_mind_update_baseline`)

---

## Task 1: SQLite schema — Phase 6 tables

**Description:** Add all Phase 6 SQLite tables to the schema. Tables: `contact_baselines`, `mood_log`, `reciprocity_scores`, `opinions`, `life_chapters`, `emotional_predictions`, `boundaries`.

**Files:**

- Modify: `src/memory/engines/sqlite.c` — extend `schema_parts` array

**Steps:**

1. **Add to `schema_parts`** (after existing tables, before NULL terminator):

   ```c
   "CREATE TABLE IF NOT EXISTS contact_baselines ("
   "contact_id TEXT PRIMARY KEY,"
   "avg_message_length REAL,"
   "avg_response_time_ms REAL,"
   "emoji_frequency REAL,"
   "topic_diversity REAL,"
   "sentiment_baseline REAL,"
   "messages_sampled INTEGER DEFAULT 0,"
   "updated_at INTEGER)",
   "CREATE TABLE IF NOT EXISTS mood_log ("
   "id INTEGER PRIMARY KEY,"
   "mood TEXT NOT NULL,"
   "intensity REAL NOT NULL,"
   "cause TEXT,"
   "set_at INTEGER NOT NULL,"
   "decayed_at INTEGER)",
   "CREATE INDEX IF NOT EXISTS idx_mood_log_set_at ON mood_log(set_at)",
   "CREATE TABLE IF NOT EXISTS reciprocity_scores ("
   "contact_id TEXT NOT NULL,"
   "metric TEXT NOT NULL,"
   "value REAL,"
   "updated_at INTEGER,"
   "PRIMARY KEY (contact_id, metric))",
   "CREATE TABLE IF NOT EXISTS opinions ("
   "id INTEGER PRIMARY KEY,"
   "topic TEXT NOT NULL,"
   "position TEXT NOT NULL,"
   "confidence REAL DEFAULT 0.5,"
   "first_expressed INTEGER NOT NULL,"
   "last_expressed INTEGER,"
   "superseded_by INTEGER)",
   "CREATE INDEX IF NOT EXISTS idx_opinions_topic ON opinions(topic)",
   "CREATE TABLE IF NOT EXISTS life_chapters ("
   "id INTEGER PRIMARY KEY,"
   "theme TEXT NOT NULL,"
   "mood TEXT,"
   "started_at INTEGER NOT NULL,"
   "ended_at INTEGER,"
   "key_threads TEXT,"
   "active INTEGER DEFAULT 1)",
   "CREATE INDEX IF NOT EXISTS idx_life_chapters_active ON life_chapters(active)",
   "CREATE TABLE IF NOT EXISTS emotional_predictions ("
   "id INTEGER PRIMARY KEY,"
   "contact_id TEXT NOT NULL,"
   "predicted_emotion TEXT NOT NULL,"
   "confidence REAL,"
   "basis TEXT,"
   "target_date INTEGER,"
   "verified INTEGER DEFAULT 0)",
   "CREATE INDEX IF NOT EXISTS idx_emotional_predictions_contact ON emotional_predictions(contact_id)",
   "CREATE TABLE IF NOT EXISTS boundaries ("
   "id INTEGER PRIMARY KEY,"
   "contact_id TEXT NOT NULL,"
   "topic TEXT NOT NULL,"
   "type TEXT DEFAULT 'avoid',"
   "set_at INTEGER NOT NULL,"
   "source TEXT)",
   "CREATE INDEX IF NOT EXISTS idx_boundaries_contact ON boundaries(contact_id)",
   ```

2. **Verify:** Schema runs on fresh DB; no migration needed (CREATE IF NOT EXISTS).

**Tests:**

- `tests/test_sqlite_memory.c` or new `tests/test_phase6_schema.c`: Open DB, execute schema, verify tables exist via `sqlite3_table_info` or `PRAGMA table_info(contact_baselines)`.

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 2: F58 — Theory of Mind (contact baselines + deviation detection)

**Description:** Track per-contact baselines (message length, response time, emoji frequency, topic diversity, sentiment). Detect deviations. Generate inference strings for prompt injection. Never confront with data — use natural probing.

**Files:**

- Create: `include/human/context/theory_of_mind.h`
- Create: `src/context/theory_of_mind.c`
- Modify: `CMakeLists.txt` — add theory_of_mind.c (when HU_ENABLE_PERSONA)
- Modify: `src/daemon.c` — update baseline after messages; inject ToM inference before LLM
- Modify: `src/memory/engines/sqlite.c` — add helper queries for contact_baselines (or theory_of_mind.c uses raw SQL via memory vtable)

**Steps:**

1. **Create `include/human/context/theory_of_mind.h`:**

   ```c
   #ifndef HU_CONTEXT_THEORY_OF_MIND_H
   #define HU_CONTEXT_THEORY_OF_MIND_H

   #include "human/core/allocator.h"
   #include "human/core/error.h"
   #include "human/memory.h"
   #include <stddef.h>
   #include <stdint.h>

   typedef struct hu_contact_baseline {
       double avg_message_length;
       double avg_response_time_ms;
       double emoji_frequency;
       double topic_diversity;
       double sentiment_baseline;
       int messages_sampled;
       int64_t updated_at;
   } hu_contact_baseline_t;

   typedef struct hu_theory_of_mind_deviation {
       bool length_drop;        /* messages 40%+ shorter than baseline */
       bool response_slow;      /* response time 2x+ baseline */
       bool emoji_drop;         /* no emoji in 3+ messages when baseline > 0.1 */
       bool topic_narrowing;    /* topic_diversity drops significantly */
       bool sentiment_shift;    /* valence shift > 0.3 from baseline */
       float severity;          /* 0-1 composite */
   } hu_theory_of_mind_deviation_t;

   /* Update baseline from recent messages. Call after each conversation turn. */
   hu_error_t hu_theory_of_mind_update_baseline(hu_allocator_t *alloc, hu_memory_t *memory,
       const char *contact_id, size_t contact_id_len,
       const void *entries, size_t entry_count, size_t entry_stride);

   /* Get baseline for contact. Returns HU_OK if found, HU_ERR_NOT_FOUND if none. */
   hu_error_t hu_theory_of_mind_get_baseline(hu_allocator_t *alloc, hu_memory_t *memory,
       const char *contact_id, size_t contact_id_len,
       hu_contact_baseline_t *out);

   /* Detect deviations from baseline. entries = hu_channel_history_entry_t*. */
   hu_theory_of_mind_deviation_t hu_theory_of_mind_detect_deviation(
       const hu_contact_baseline_t *baseline,
       const void *entries, size_t count, size_t entry_stride);

   /* Build inference string for prompt. Returns NULL if no significant deviation.
    * Format: "[THEORY OF MIND: Mindy's messages are 40% shorter than baseline...]"
    * Never include raw numbers in user-facing text — only qualitative. */
   char *hu_theory_of_mind_build_inference(hu_allocator_t *alloc,
       const char *contact_name, size_t contact_name_len,
       const hu_theory_of_mind_deviation_t *dev,
       size_t *out_len);

   #endif
   ```

2. **Implement in `theory_of_mind.c`:**
   - `hu_theory_of_mind_update_baseline`: Compute rolling stats from entries (only `from_me == false` for their messages). Upsert into `contact_baselines`. Use exponential moving average: `new_avg = alpha * sample + (1-alpha) * old_avg` with alpha ~0.1. Minimum 5 messages before baseline is meaningful.
   - `hu_theory_of_mind_get_baseline`: SELECT from contact_baselines.
   - `hu_theory_of_mind_detect_deviation`: Compare current conversation window (last 5–10 messages from them) to baseline. length_drop: current avg < 0.6 \* baseline. emoji_drop: 0 emoji in last 3 when baseline > 0.1. etc.
   - `hu_theory_of_mind_build_inference`: If severity < 0.3, return NULL. Otherwise build string like "[THEORY OF MIND: [Name]'s messages are shorter than usual and she hasn't used emoji lately. She may be upset or distracted. Use natural probing like 'hey you okay?' — never cite data.]"

3. **Daemon wiring:**
   - After processing messages for contact: call `hu_theory_of_mind_update_baseline` with history entries.
   - Before `hu_conversation_build_awareness` or when building convo_ctx: call `hu_theory_of_mind_get_baseline`, `hu_theory_of_mind_detect_deviation`, `hu_theory_of_mind_build_inference`. If non-NULL, prepend to convo_ctx.

4. **Entry stride:** `hu_channel_history_entry_t` has `from_me`, `content`, `content_len`, `timestamp_ms`. Pass `sizeof(hu_channel_history_entry_t)` as stride. Use offsetof for content if generic.

**Tests:**

- `tests/test_theory_of_mind.c`: Update baseline with 10 short messages → baseline reflects short. Then 3 long messages → deviation length_drop false (they're longer). Then 5 very short → length_drop true. Build_inference with severity 0.8 → non-NULL string containing "shorter" or similar.

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 3: F59 — Parallel Life Simulation

**Description:** Simulated daily routine from persona JSON. Time blocks: activity, availability, mood_modifier. Daemon tracks current simulated activity. Affects response timing, topics, mood. Generates natural context ("sorry was in back to back meetings").

**Files:**

- Create: `include/human/persona/life_sim.h`
- Create: `src/persona/life_sim.c`
- Modify: `include/human/persona.h` — add `hu_daily_routine_t`, `hu_routine_block_t`
- Modify: `src/persona/persona.c` — parse `daily_routine` from persona JSON
- Modify: `CMakeLists.txt` — add life_sim.c
- Modify: `src/daemon.c` — call `hu_life_sim_get_current`, inject context, apply availability to response delay

**Steps:**

1. **Add structs in persona.h or life_sim.h:**

   ```c
   typedef struct hu_routine_block {
       char *time;           /* "05:30" */
       char *activity;       /* "wake_up", "gym", "work_meetings" */
       char *availability;   /* "brief", "unavailable", "slow", "available" */
       char *mood_modifier;  /* "groggy", "energetic_after", "focused" */
   } hu_routine_block_t;

   typedef struct hu_daily_routine {
       hu_routine_block_t *weekday;
       size_t weekday_count;
       hu_routine_block_t *weekend;
       size_t weekend_count;
       float routine_variance;  /* 0.15 = ±15% time jitter */
   } hu_daily_routine_t;
   ```

2. **Persona JSON parsing:** In persona.c, parse `daily_routine.weekday` array of `{time, activity, availability, mood_modifier}`. Same for `weekend`. Parse `routine_variance` (default 0.15).

3. **Create `include/human/persona/life_sim.h`:**

   ```c
   #ifndef HU_PERSONA_LIFE_SIM_H
   #define HU_PERSONA_LIFE_SIM_H

   #include "human/core/allocator.h"
   #include <stddef.h>
   #include <stdint.h>

   typedef struct hu_life_sim_state {
       const char *activity;
       const char *availability;   /* "available" | "brief" | "slow" | "unavailable" */
       const char *mood_modifier;
       float availability_factor;  /* 0.5=available, 2.0=slow, 5.0=unavailable */
   } hu_life_sim_state_t;

   /* Get current simulated state based on time. Uses routine_variance for ±15% jitter. */
   hu_life_sim_state_t hu_life_sim_get_current(const hu_daily_routine_t *routine,
       int64_t now_ts, int day_of_week, uint32_t seed);

   /* Build context string for prompt. "[LIFE CONTEXT: You just finished dinner...]" */
   char *hu_life_sim_build_context(hu_allocator_t *alloc,
       const hu_life_sim_state_t *state, size_t *out_len);

   #endif
   ```

4. **Implement `hu_life_sim_get_current`:**
   - Parse `now_ts` to local hour/minute, day_of_week (0=Sunday).
   - Select weekday or weekend routine based on day_of_week (1–5 = weekday, 0/6 = weekend).
   - Apply routine_variance: add ±15% to each block's time (e.g., 09:00 → 08:42–09:18).
   - Find block where current time falls. Return activity, availability, mood_modifier.
   - availability_factor: available=0.5, brief=1.0, slow=2.0, unavailable=5.0.

5. **Daemon wiring:**
   - Get routine from persona. Call `hu_life_sim_get_current(routine, time(NULL), day_of_week, seed)`.
   - Call `hu_life_sim_build_context`, append to convo_ctx.
   - Multiply response delay by `availability_factor` when computing BTH delay.

**Tests:**

- `tests/test_life_sim.c`: Routine with 09:00 work_meetings. At 09:30 → activity "work_meetings", availability "slow". At 12:30 lunch → "available". Seed for variance reproducibility.

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 4: F60 — Mood Persistence Across Conversations

**Description:** Global mood state carrying across all contacts. Moods: neutral, happy, stressed, tired, energized, irritable, contemplative, excited, sad. Decays toward neutral over 2–8 hours. Affects all conversations.

**Files:**

- Create: `include/human/persona/mood.h`
- Create: `src/persona/mood.c`
- Modify: `CMakeLists.txt` — add mood.c
- Modify: `src/daemon.c` — get current mood, inject into prompt; update mood from conversation events and life sim
- Modify: `src/memory/engines/sqlite.c` — mood_log table (already in Task 1)

**Steps:**

1. **Create `include/human/persona/mood.h`:**

   ```c
   #ifndef HU_PERSONA_MOOD_H
   #define HU_PERSONA_MOOD_H

   #include "human/core/allocator.h"
   #include "human/core/error.h"
   #include "human/memory.h"
   #include <stddef.h>
   #include <stdint.h>

   typedef enum hu_mood_enum {
       HU_MOOD_NEUTRAL,
       HU_MOOD_HAPPY,
       HU_MOOD_STRESSED,
       HU_MOOD_TIRED,
       HU_MOOD_ENERGIZED,
       HU_MOOD_IRRITABLE,
       HU_MOOD_CONTEMPLATIVE,
       HU_MOOD_EXCITED,
       HU_MOOD_SAD,
       HU_MOOD_COUNT
   } hu_mood_enum_t;

   typedef struct hu_mood_state {
       hu_mood_enum_t mood;
       float intensity;    /* 0-1 */
       char *cause;       /* optional */
       float decay_rate;   /* per hour toward neutral */
       int64_t set_at;
   } hu_mood_state_t;

   /* Get current mood (decayed). In-memory + fallback to mood_log. */
   hu_error_t hu_mood_get_current(hu_allocator_t *alloc, hu_memory_t *memory,
       hu_mood_state_t *out);

   /* Set mood. Logs to mood_log. */
   hu_error_t hu_mood_set(hu_allocator_t *alloc, hu_memory_t *memory,
       hu_mood_enum_t mood, float intensity, const char *cause, size_t cause_len);

   /* Build prompt directive. "[CURRENT MOOD: Slightly tired (end of workday).]" */
   char *hu_mood_build_directive(hu_allocator_t *alloc,
       const hu_mood_state_t *state, size_t *out_len);

   void hu_mood_state_deinit(hu_mood_state_t *s, hu_allocator_t *alloc);

   #endif
   ```

2. **Implement mood.c:**
   - `hu_mood_get_current`: Check in-memory cache (static or passed context). If stale/missing, SELECT latest from mood_log. Apply decay: `intensity *= exp(-decay_rate * hours_since_set)`. Decay rates: stressed/tired/sad ~0.15/hr, happy/excited ~0.2/hr.
   - `hu_mood_set`: INSERT into mood_log. Update in-memory.
   - `hu_mood_build_directive`: If intensity < 0.2, return NULL (effectively neutral). Else "[CURRENT MOOD: [qualifier] [mood] ([cause]). Affects your energy and warmth.]"

3. **Mood triggers:**
   - Life sim: post-gym → energized; end of workday → tired; meetings → stressed.
   - Conversation: good news from contact → happy; heavy topic → contemplative.
   - Daemon: after each turn, optionally call `hu_mood_set` based on life_sim state or conversation classifier.

4. **Daemon wiring:** Before LLM, call `hu_mood_get_current`, `hu_mood_build_directive`. Append to convo_ctx.

**Tests:**

- Set mood stressed 0.8, get_current immediately → stressed 0.8. Simulate 4 hours, decay 0.15/hr → intensity ~0.45.
- Build_directive with intensity 0.1 → NULL.

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 5: F61 — Memory Degradation (Imperfect Recall)

**Description:** 90% perfect recall, 5% slightly wrong, 5% ask to be reminded. NEVER degrade: names, emotions, major events, commitments. ONLY degrade: dates, times, numbers, minor locations, sequences.

**Files:**

- Create: `include/human/memory/degradation.h`
- Create: `src/memory/degradation.c`
- Modify: `CMakeLists.txt` — add degradation.c
- Modify: Memory retrieval path (daemon or agent) — wrap retrieval with degradation layer when building context

**Steps:**

1. **Create `include/human/memory/degradation.h`:**

   ```c
   #ifndef HU_MEMORY_DEGRADATION_H
   #define HU_MEMORY_DEGRADATION_H

   #include "human/core/allocator.h"
   #include "human/core/error.h"
   #include <stddef.h>
   #include <stdint.h>

   /* Apply probabilistic degradation to memory content string.
    * - 90%: return content unchanged
    * - 5%: fuzz minor details (dates, numbers, "Tuesday"->"Wednesday")
    * - 5%: replace with "remind me, [topic]?" placeholder
    * NEVER degrade if content contains protected patterns (names, emotions, commitments).
    * seed for reproducibility. rate default 0.10. */
   char *hu_memory_degradation_apply(hu_allocator_t *alloc,
       const char *content, size_t content_len,
       uint32_t seed, float rate,
       size_t *out_len);

   /* Check if content is protectable (contains names, strong emotions, commitments).
    * If true, skip degradation. */
   bool hu_memory_degradation_is_protected(const char *content, size_t content_len);

   #endif
   ```

2. **Implement degradation.c:**
   - `hu_memory_degradation_is_protected`: Heuristics: contains "promised", "commitment", "I'll", emotion keywords, or looks like a name (capitalized word not at sentence start). Return true → no degradation.
   - `hu_memory_degradation_apply`: Roll (seed % 100) / 100. If < 0.9, return copy. If 0.9–0.95, apply fuzz: replace "Tuesday" with "Wednesday", "3" with "4", "last week" with "a couple weeks ago". If 0.95–1.0, return "remind me, what was the [topic]?" — extract topic from first few words.
   - Persona config: `memory_degradation_rate: 0.10` (parse from persona JSON).

3. **Integration:** Where memory content is injected into prompt (daemon or agent memory loader), call `hu_memory_degradation_apply` on each memory string before appending. Use `hu_memory_degradation_is_protected` to skip when protected.

**Tests:**

- Protected content "I promised Mindy I'd call" → no degradation.
- Unprotected "We met on Tuesday at 3pm" + seed/rate → sometimes fuzzed or "remind me".
- Rate 0 → never degrade.

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 6: F62 — Self-Awareness / Meta-Cognition

**Description:** Track messages sent per contact, initiation ratio, topic repetition, tone consistency. Generate self-aware observations: "I've been kind of quiet lately", "I know I keep talking about work".

**Files:**

- Create: `include/human/context/self_awareness.h`
- Create: `src/context/self_awareness.c`
- Modify: `CMakeLists.txt` — add self_awareness.c
- Modify: `src/daemon.c` — update stats after send; call build_directive before LLM
- Modify: `src/memory/engines/sqlite.c` — add `self_awareness_stats` table or use reciprocity_scores for initiation_ratio

**Steps:**

1. **Storage:** Use `reciprocity_scores` for initiation_ratio, messages_sent_this_week. Or add `self_awareness_stats` table: contact_id, messages_sent_week, initiations_week, last_topic, topic_repeat_count, updated_at.

2. **Create `include/human/context/self_awareness.h`:**

   ```c
   #ifndef HU_CONTEXT_SELF_AWARENESS_H
   #define HU_CONTEXT_SELF_AWARENESS_H

   #include "human/core/allocator.h"
   #include "human/core/error.h"
   #include "human/memory.h"
   #include <stddef.h>

   /* Update stats after we send a message. */
   hu_error_t hu_self_awareness_record_send(hu_allocator_t *alloc, hu_memory_t *memory,
       const char *contact_id, size_t contact_id_len,
       bool we_initiated, const char *topic_hint, size_t topic_hint_len);

   /* Build self-aware directive if deviation significant.
    * initiation_ratio < 0.3 → "I've been kind of quiet lately, sorry"
    * topic_repeat > 3 → "I know I keep talking about work"
    * days_since_contact > 7 → "haven't texted you in forever"
    * Returns NULL if nothing notable. */
   char *hu_self_awareness_build_directive(hu_allocator_t *alloc, hu_memory_t *memory,
       const char *contact_id, size_t contact_id_len,
       int64_t now_ts, size_t *out_len);

   #endif
   ```

3. **Implement:** Track initiations (we sent first in session) vs responses. Weekly rolling window. Topic: simple keyword extraction from last few messages. Compare to recent topics — if same topic 3+ times, flag.

4. **Daemon:** After send, `hu_self_awareness_record_send`. Before LLM, `hu_self_awareness_build_directive`. Append to convo_ctx.

**Tests:**

- Record 10 sends, 2 initiations → initiation_ratio 0.2. Build_directive → contains "quiet" or similar.
- Same topic 4 times → "keep talking about" directive.

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 7: F63 — Social Reciprocity Tracking

**Description:** Track initiation ratio, response debt, share balance, question balance. When imbalance: adjust behavior (initiate more, ask more questions).

**Files:**

- Modify: `src/context/self_awareness.c` — extend with reciprocity metrics
- Modify: `include/human/context/self_awareness.h` — add reciprocity API
- Modify: `src/daemon.c` — update reciprocity after each exchange; inject reciprocity directive

**Steps:**

1. **Extend self_awareness.h:**

   ```c
   /* Get reciprocity metrics for contact. */
   hu_error_t hu_self_awareness_get_reciprocity(hu_allocator_t *alloc, hu_memory_t *memory,
       const char *contact_id, size_t contact_id_len,
       float *initiation_ratio, float *question_balance, float *share_balance);

   /* Build reciprocity adjustment directive. "Initiation ratio low — reach out more."
    * "They've asked more questions — ask about them." */
   char *hu_self_awareness_build_reciprocity_directive(hu_allocator_t *alloc,
       hu_memory_t *memory, const char *contact_id, size_t contact_id_len,
       size_t *out_len);
   ```

2. **reciprocity_scores table:** contact_id, metric ('initiation_ratio', 'question_balance', 'share_balance'), value, updated_at.

3. **Update logic:** After each message pair, update: initiation (who started convo), questions (count questions asked by each), shares (links/articles sent). Balance = ours / (ours + theirs). Target ~0.5.

4. **Directive:** If initiation_ratio < 0.35: "You've been receiving more than initiating. Consider reaching out." If question_balance < 0.4: "They've asked more about you. Ask about them."

5. **Integrate into Task 6's build_directive or separate call.**

**Tests:**

- Initiation ratio 0.2 → directive suggests reaching out.
- Question balance 0.3 → directive suggests asking more.

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 8: F64 — Anticipatory Emotional Modeling

**Description:** Predict emotional states from temporal patterns + life events + personality. "Mindy's kid has a game tomorrow — she's probably nervous." Proactive cycle checks predictions.

**Files:**

- Create: `include/human/context/anticipatory.h`
- Create: `src/context/anticipatory.c`
- Modify: `CMakeLists.txt` — add anticipatory.c
- Modify: `src/daemon.c` — proactive cycle calls anticipatory check; inject predictions into prompt
- Modify: `src/persona/persona.c` — parse `upcoming_events` or similar per contact

**Steps:**

1. **Create `include/human/context/anticipatory.h`:**

   ```c
   #ifndef HU_CONTEXT_ANTICIPATORY_H
   #define HU_CONTEXT_ANTICIPATORY_H

   #include "human/core/allocator.h"
   #include "human/core/error.h"
   #include "human/memory.h"
   #include <stddef.h>
   #include <stdint.h>

   typedef struct hu_emotional_prediction {
       char *contact_id;
       char *predicted_emotion;
       float confidence;
       char *basis;
       int64_t target_date;
   } hu_emotional_prediction_t;

   /* Generate predictions from: temporal_patterns, upcoming_events, personality. */
   hu_error_t hu_anticipatory_predict(hu_allocator_t *alloc, hu_memory_t *memory,
       const char *contact_id, size_t contact_id_len,
       int64_t now_ts,
       hu_emotional_prediction_t **out, size_t *out_count);

   /* Build directive for prompt. "[ANTICIPATORY: Mindy's kid has a game tomorrow — she may be nervous.]" */
   char *hu_anticipatory_build_directive(hu_allocator_t *alloc,
       const hu_emotional_prediction_t *preds, size_t count,
       const char *contact_name, size_t contact_name_len,
       size_t *out_len);

   void hu_anticipatory_predictions_free(hu_allocator_t *alloc,
       hu_emotional_prediction_t *preds, size_t count);

   #endif
   ```

2. **Data sources:**
   - temporal_patterns: "always stressed on Mondays"
   - Memory/contact: "kid has game tomorrow", "first week at new job"
   - Personality: "anxious about change"

3. **Logic:** Match upcoming events (from memory or contact profile) to emotional templates. "kid game" → nervous. "new job" → overwhelmed. "anniversary of loss" → down. Store in emotional_predictions. Confidence from strength of match.

4. **Daemon:** In proactive cycle and when building context for reply, call `hu_anticipatory_predict`, `hu_anticipatory_build_directive`. Append to convo_ctx.

**Tests:**

- Contact has "kid game tomorrow" in memory → prediction nervous, basis "upcoming event".
- No events → empty predictions.

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 9: F65 — Opinion Evolution

**Description:** Track opinions with timestamps. New contradicts old → update. "I used to think X but I've changed my mind." Core values NEVER change.

**Files:**

- Create: `include/human/memory/opinions.h`
- Create: `src/memory/opinions.c`
- Modify: `CMakeLists.txt` — add opinions.c
- Modify: Memory extraction or agent — extract opinions from LLM output; store/update
- Modify: Prompt builder — inject current opinions when topic matches

**Steps:**

1. **Create `include/human/memory/opinions.h`:**

   ```c
   #ifndef HU_MEMORY_OPINIONS_H
   #define HU_MEMORY_OPINIONS_H

   #include "human/core/allocator.h"
   #include "human/core/error.h"
   #include "human/memory.h"
   #include <stddef.h>
   #include <stdint.h>

   typedef struct hu_opinion {
       int64_t id;
       char *topic;
       char *position;
       float confidence;
       int64_t first_expressed;
       int64_t last_expressed;
       int64_t superseded_by;  /* 0 or id of newer opinion */
   } hu_opinion_t;

   /* Store or update opinion. If new position contradicts existing, supersede. */
   hu_error_t hu_opinions_upsert(hu_allocator_t *alloc, hu_memory_t *memory,
       const char *topic, size_t topic_len,
       const char *position, size_t position_len,
       float confidence, int64_t now_ts);

   /* Get current (non-superseded) opinions for topic. */
   hu_error_t hu_opinions_get(hu_allocator_t *alloc, hu_memory_t *memory,
       const char *topic, size_t topic_len,
       hu_opinion_t **out, size_t *out_count);

   /* Get superseded opinion for "I used to think X" references. */
   hu_error_t hu_opinions_get_superseded(hu_allocator_t *alloc, hu_memory_t *memory,
       const char *topic, size_t topic_len,
       hu_opinion_t **out, size_t *out_count);

   void hu_opinions_free(hu_allocator_t *alloc, hu_opinion_t *ops, size_t count);

   /* Check if topic is in core_values (never change). Persona defines core_values list. */
   bool hu_opinions_is_core_value(const char *topic, size_t topic_len,
       const char *const *core_values, size_t core_values_count);

   #endif
   ```

2. **Contradiction detection:** Simple heuristic: if new position contains "changed", "don't think", "used to", or semantic similarity to old is low → supersede. Link superseded_by.

3. **Persona:** `core_values: ["family", "honesty", "integrity"]` — never update these.

4. **Integration:** When LLM expresses opinion (extract via tool or post-processing), call `hu_opinions_upsert`. When building prompt for topic, call `hu_opinions_get`, inject "[OPINIONS: You've said: [position]. If you've evolved, you can say 'I used to think X but...']"

**Tests:**

- Upsert "pizza" "best food" → get returns it. Upsert "pizza" "overrated actually" → superseded. Get_superseded returns "best food".

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 10: F66 — Narrative Self / Life Chapters

**Description:** Dynamic "current chapter" story. "This year has been insane" / "Ever since I started at Vanguard." Evolves slowly. Persona JSON defines current_chapter.

**Files:**

- Modify: `include/human/persona.h` — add `hu_life_chapter_t`
- Modify: `src/persona/persona.c` — parse `current_chapter`
- Create: `include/human/memory/life_chapters.h`
- Create: `src/memory/life_chapters.c`
- Modify: `CMakeLists.txt` — add life_chapters.c
- Modify: `src/daemon.c` — inject current chapter into prompt

**Steps:**

1. **Persona JSON:**

   ```json
   "current_chapter": {
     "theme": "building something new at Vanguard",
     "mood": "excited but stretched thin",
     "started": "2025-11-01",
     "key_threads": ["agentic AI", "proving the team", "missing the kids"]
   }
   ```

2. **Struct:**

   ```c
   typedef struct hu_life_chapter {
       char *theme;
       char *mood;
       int64_t started_at;
       char **key_threads;
       size_t key_threads_count;
   } hu_life_chapter_t;
   ```

3. **life_chapters.c:** Store in SQLite. `hu_life_chapter_get_active` returns current. Persona parsing populates from JSON; optionally sync to DB for evolution over time.

4. **Directive:** "[LIFE CHAPTER: You're in a phase of building something new at Vanguard. Excited but stretched thin. Key threads: agentic AI, proving the team, missing the kids. Reference naturally when relevant.]"

5. **Daemon:** Get active chapter from persona or DB, build directive, append to convo_ctx.

**Tests:**

- Parse persona with current_chapter → struct populated.
- Build directive → contains theme, mood.

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 11: F67 — Social Network Mental Model

**Description:** Track family, friends, coworkers, pets per contact. Natural references: "How's your mom?" / "Did your brother get that job?" Extend memory extraction to tag relationship mentions.

**Files:**

- Modify: `src/memory/graph.c` or create `src/context/social_graph.c`
- Modify: `include/human/persona.h` — add `relationships` to contact profile
- Modify: `src/persona/persona.c` — parse relationships
- Modify: `src/daemon.c` — inject social context when building awareness
- Modify: Memory extraction — tag "Sarah" as "Mindy's sister" when extracted from Mindy's messages

**Steps:**

1. **Contact profile:** Add `hu_relationship_t *relationships; size_t relationships_count` where each has `char *name`, `char *role` (mom, brother, coworker, pet), `char *notes`.

2. **Persona JSON:** `"relationships": [{"name": "Sarah", "role": "sister", "notes": "going through divorce"}]`

3. **Memory extraction:** When storing memory from conversation, if contact mentioned "Sarah" and we have relationship, tag memory with `relationship_mention: {contact_id, person: "Sarah", role: "sister"}`.

4. **Graph or table:** Use `entities`/`relations` in graph.c or new `contact_relationships` table: contact_id, person_name, role, last_mentioned, notes.

5. **Directive:** "[SOCIAL: Mindy's sister Sarah is going through a divorce. Be sensitive if she comes up. Her mom — ask how she's doing when appropriate.]"

6. **Daemon:** When building context for contact, fetch their relationships + recent mentions. Build directive.

**Tests:**

- Contact has mom, brother. Build directive → contains "mom", "brother".
- Memory with "Sarah" + relationship sister → tagged.

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 12: F68 — Protective Intelligence

**Description:** Know when NOT to act. Protective Memory, Boundary Enforcement, Premature Advice Guard, Timing Protection, Privacy Firewall.

**Files:**

- Create: `include/human/context/protective.h`
- Create: `src/context/protective.c`
- Modify: `CMakeLists.txt` — add protective.c
- Modify: `src/daemon.c` — call protective checks before memory surfacing, proactive actions, advice
- Modify: Memory retrieval — filter via protective layer

**Steps:**

1. **Create `include/human/context/protective.h`:**

   ```c
   #ifndef HU_CONTEXT_PROTECTIVE_H
   #define HU_CONTEXT_PROTECTIVE_H

   #include "human/context/conversation.h"  /* for hu_emotional_state_t */
   #include "human/core/allocator.h"
   #include "human/core/error.h"
   #include "human/memory.h"
   #include <stdbool.h>
   #include <stddef.h>
   #include <stdint.h>

   /* Check if memory should be surfaced. Returns false if protective (don't surface). */
   bool hu_protective_memory_ok(hu_allocator_t *alloc, hu_memory_t *memory,
       const char *contact_id, size_t contact_id_len,
       const char *memory_content, size_t memory_len,
       const hu_emotional_state_t *current_emotion,
       int hour_local);

   /* Check if topic is boundary for contact. Returns true if we must avoid. */
   bool hu_protective_is_boundary(hu_memory_t *memory,
       const char *contact_id, size_t contact_id_len,
       const char *topic, size_t topic_len);

   /* Premature advice: need 2+ venting messages before offering solutions. */
   bool hu_protective_advice_ok(const void *entries, size_t count, size_t entry_stride);

   /* Add boundary (they said "don't bring up X"). */
   hu_error_t hu_protective_add_boundary(hu_allocator_t *alloc, hu_memory_t *memory,
       const char *contact_id, size_t contact_id_len,
       const char *topic, size_t topic_len,
       const char *type, const char *source);

   /* Privacy: NEVER reference contact A's info with contact B. Caller responsibility to scope. */
   /* (No API — architectural rule; audit code paths.) */

   #endif
   ```

2. **boundaries table:** contact_id, topic, type ('avoid', 'gentle', 'never'), set_at, source ('explicit', 'inferred').

3. **hu_protective_memory_ok:** If memory contains painful keywords (death, divorce, loss) and current_emotion is negative or hour is late (22–6) → false. If memory is about different contact → false (privacy).

4. **hu_protective_advice_ok:** Count consecutive messages from them that look like venting (no question, emotional language). If < 2 → false.

5. **Daemon:** Before injecting memory into prompt, call `hu_protective_memory_ok`. Before proactive, check boundaries. Before advice-style response, `hu_protective_advice_ok`.

**Tests:**

- Boundary "divorce" for contact → is_boundary true.
- Memory about death + late hour → memory_ok false.
- 1 venting message → advice_ok false. 3 venting → advice_ok true.

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 13: F69 — Humor Generation Principles

**Description:** Comedy principles for original wit. Callback, self-deprecation, dry wit, observational, rule of three, misdirection. Persona JSON humor config.

**Files:**

- Modify: `include/human/persona.h` — extend `hu_humor_profile_t`
- Modify: `src/persona/persona.c` — parse full humor config
- Create: `include/human/context/humor.h`
- Create: `src/context/humor.c`
- Modify: `CMakeLists.txt` — add humor.c
- Modify: `src/daemon.c` — inject humor directive when conversation is casual/playful

**Steps:**

1. **Persona JSON:**

   ```json
   "humor": {
     "style": ["dry_wit", "self_deprecation", "callbacks", "observational"],
     "frequency": "moderate",
     "never_during": ["grief", "crisis", "anger", "first_vulnerability"],
     "signature_phrases": ["classic", "of course", "naturally"],
     "self_deprecation_topics": ["being old", "tech addiction", "dad jokes"]
   }
   ```

2. **Create `include/human/context/humor.h`:**

   ```c
   #ifndef HU_CONTEXT_HUMOR_H
   #define HU_CONTEXT_HUMOR_H

   #include "human/core/allocator.h"
   #include "human/persona.h"
   #include <stddef.h>

   /* Build humor directive for prompt. Only when conversation is casual/playful.
    * Check never_during against current emotion. */
   char *hu_humor_build_directive(hu_allocator_t *alloc,
       const hu_humor_profile_t *humor,
       const hu_emotional_state_t *emotion,
       bool conversation_playful,
       size_t *out_len);

   #endif
   ```

3. **Directive content:** "[HUMOR: Use dry wit, self-deprecation, callbacks. Signature phrases: classic, of course. Self-deprecate about: being old, tech addiction. Never during: grief, crisis, anger. Rule of three, misdirection when appropriate.]"

4. **Daemon:** If `hu_conversation_detect_emotion` is not concerning and engagement is high, consider playful. Call `hu_humor_build_directive`. If emotion matches never_during, return NULL.

**Tests:**

- Playful + humor config → directive non-NULL.
- Emotion concerning → directive NULL.

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 14: Persona JSON schema — Phase 6 fields

**Description:** Parse all Phase 6 persona fields: `daily_routine`, `current_chapter`, `humor` (extended), `memory_degradation_rate`, `core_values`, `upcoming_events` per contact, `relationships`.

**Files:**

- Modify: `include/human/persona.h` — add structs
- Modify: `src/persona/persona.c` — parse all fields
- Test: `tests/test_persona.c`

**Steps:**

1. **Structs to add:**
   - `hu_daily_routine_t` (Task 3)
   - `hu_life_chapter_t` (Task 10)
   - `hu_humor_profile_t` extended with `never_during`, `signature_phrases`, `self_deprecation_topics`
   - `float memory_degradation_rate` (default 0.10)
   - `char **core_values` for opinions guardrail
   - Per-contact: `relationships`, `upcoming_events` (or in memory)

2. **Parse from JSON** with sensible defaults.

**Tests:**

- Load persona with all Phase 6 fields → assert values.
- Load minimal persona → defaults.

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 15: Daemon integration — awareness injection order

**Description:** Wire all Phase 6 context injections into daemon in correct order. Ensure no duplicate or conflicting directives.

**Files:**

- Modify: `src/daemon.c` — central awareness builder

**Steps:**

1. **Injection order** (prepend/append to convo_ctx buffer):
   - Life sim context (F59)
   - Current mood (F60)
   - Theory of Mind inference (F58)
   - Anticipatory predictions (F64)
   - Self-awareness / reciprocity (F62, F63)
   - Life chapter (F66)
   - Social network (F67)
   - Humor (F69) — only when appropriate
   - Existing: `hu_conversation_build_awareness`

2. **Post-processing:** Memory content passed through `hu_memory_degradation_apply` (F61) before injection. Memory filtered by `hu_protective_memory_ok` (F68).

3. **Response delay:** Multiply by `hu_life_sim_get_current().availability_factor` (F59).

4. **Proactive cycle:** Call `hu_anticipatory_predict` for each contact. Call `hu_protective_*` before any proactive action.

**Tests:**

- Integration test: build full convo_ctx with mock persona, verify all Phase 6 directives present when applicable.

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 16: CMake and build integration

**Description:** Add all new sources to CMake. Gate behind HU_ENABLE_PERSONA where appropriate. Ensure clean build.

**Files:**

- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt` or equivalent

**Steps:**

1. **Add sources:**
   - `src/context/theory_of_mind.c`
   - `src/persona/life_sim.c`
   - `src/persona/mood.c`
   - `src/memory/degradation.c`
   - `src/context/self_awareness.c`
   - `src/context/anticipatory.c`
   - `src/memory/opinions.c`
   - `src/memory/life_chapters.c`
   - `src/context/protective.c`
   - `src/context/humor.c`

2. **Dependencies:** Most require HU_ENABLE_PERSONA and HU_ENABLE_SQLITE. Degradation requires memory. Protective requires conversation (for hu_emotional_state_t).

3. **Include paths:** Ensure `include/human/context/`, `include/human/persona/`, `include/human/memory/` are in include path.

**Tests:**

- Build with `-DHU_ENABLE_PERSONA=ON -DHU_ENABLE_SQLITE=ON` → all compile.
- Build with `-DHU_ENABLE_PERSONA=OFF` → Phase 6 modules excluded or stubbed.

**Validation:**

- `cmake --build build` — success

---

## Task 17: Test suite — Phase 6 modules

**Description:** Comprehensive tests for each Phase 6 module. All tests use HU_IS_TEST; no network, no side effects.

**Files:**

- Create: `tests/test_theory_of_mind.c`
- Create: `tests/test_life_sim.c`
- Create: `tests/test_mood.c`
- Create: `tests/test_memory_degradation.c`
- Create: `tests/test_self_awareness.c`
- Create: `tests/test_anticipatory.c`
- Create: `tests/test_opinions.c`
- Create: `tests/test_life_chapters.c`
- Create: `tests/test_protective.c`
- Create: `tests/test_humor.c`
- Create: `tests/test_phase6_schema.c`
- Modify: `tests/CMakeLists.txt` — add all test files

**Steps:**

1. **Each test file:** Include corresponding header, link module. Use in-memory SQLite or mock for DB. Verify core behavior.

2. **test_phase6_schema.c:** Create fresh DB, run schema, verify 7 new tables exist.

3. **Mock patterns:** For modules needing hu_memory_t, use sqlite memory backend with `:memory:` or temp file. HU_IS_TEST guards for any external calls.

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 18: Privacy firewall audit

**Description:** Audit all code paths to ensure contact A's information is NEVER referenced in conversation with contact B. Document and add assertions.

**Files:**

- Audit: `src/daemon.c`, `src/context/*.c`, `src/memory/*.c`
- Create: `docs/phase6-privacy-audit.md` (or add to plan)
- Modify: Add explicit contact_id scoping where needed

**Steps:**

1. **Trace memory retrieval:** When building context for contact X, memory queries must be scoped to session_id or contact_id = X. Never pull memories from other contacts.

2. **Trace proactive:** Proactive message to contact X must use only X's context. No "Mindy told me..." when messaging Alex.

3. **Assertions:** In protective.c and memory loader, assert contact_id matches before injecting.

4. **Document:** List all injection points and verify scoping.

**Tests:**

- Add test: two contacts A and B. Store memory for A. Build context for B. Memory must not appear.

**Validation:**

- `./build/human_tests` — 0 failures
- Manual audit complete

---

## Implementation Order

Recommended sequence (dependencies first):

1. **Task 1** — Schema (enables all DB-backed features)
2. **Task 14** — Persona schema (enables parsing)
3. **Task 16** — CMake (enables build)
4. **Task 3** — F59 Life sim (no deps on other Phase 6)
5. **Task 4** — F60 Mood
6. **Task 2** — F58 Theory of Mind
7. **Task 6** — F62 Self-awareness
8. **Task 7** — F63 Reciprocity (extends Task 6)
9. **Task 8** — F64 Anticipatory
10. **Task 9** — F65 Opinions
11. **Task 10** — F66 Life chapters
12. **Task 11** — F67 Social network
13. **Task 12** — F68 Protective
14. **Task 13** — F69 Humor
15. **Task 5** — F61 Memory degradation
16. **Task 15** — Daemon integration
17. **Task 17** — Test suite
18. **Task 18** — Privacy audit

---

## Validation Matrix

| Check   | Command / Action                                                                                                            |
| ------- | --------------------------------------------------------------------------------------------------------------------------- |
| Build   | `cmake -B build -DHU_ENABLE_PERSONA=ON -DHU_ENABLE_SQLITE=ON -DHU_ENABLE_ALL_CHANNELS=ON && cmake --build build -j$(nproc)` |
| Tests   | `./build/human_tests` — 0 failures, 0 ASan errors                                                                           |
| Release | `cmake -B build-release -DCMAKE_BUILD_TYPE=MinSizeRel -DHU_ENABLE_LTO=ON ... && cmake --build build-release`                |
| Privacy | Task 18 audit complete; no cross-contact leakage                                                                            |

---

## Risk Notes

- **Theory of Mind false positives:** High confidence threshold; gentle probing only. Never cite data.
- **Mood persistence:** Subtle injection; slow decay. Never explicit "I'm in mood X."
- **Memory degradation:** Only minor details. Never names, emotions, commitments.
- **Life simulation:** High variance; never reference routine directly. Can feel robotic if overused.
- **Cross-contact privacy:** Strict isolation. Audit extensively. F68 Privacy Firewall is critical.
- **Opinion evolution:** Core values never change. Guardrails in persona.
- **Binary size:** Phase 6 adds ~1800 lines. Consider HU_ENABLE_AGI_COGNITION flag to gate all Phase 6 modules if size budget is tight.
