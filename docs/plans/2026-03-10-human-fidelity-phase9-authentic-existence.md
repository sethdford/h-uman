---
title: "Human Fidelity Phase 9 — Authentic Existence"
created: 2026-03-10
status: closed
scope: cognitive load, spontaneous narration, physical embodiment, imperfection, authenticity, daemon scheduling
phase: 9
features:
  [
    F102,
    F103,
    F104,
    F105,
    F106,
    F107,
    F108,
    F109,
    F110,
    F111,
    F112,
    F113,
    F114,
    F115,
  ]
parent: 2026-03-10-human-fidelity-design.md
last_audit: 2026-05-25
---

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

# Human Fidelity Phase 9 — Authentic Existence

Phase 9 of the Human Fidelity project. Everything before this makes human BEHAVE like a human. This phase makes it BE like a human — messy, embodied, imperfect, genuinely present. These features solve the "Uncanny Valley" problem: the system is too available, too perfect, too consistent, and too engaged to be a real person.

**Reference:** `docs/plans/2026-03-10-human-fidelity-design.md`

**Naming:** Use `hu_` prefix for Phase 9 modules. Guards: `HU_IS_TEST` (skip real LLM/network). Constants: `HU_SCREAMING_SNAKE`. Error type: `hu_error_t`. SQLite: `SQLITE_STATIC` (null), never `SQLITE_TRANSIENT`. Free every allocation.

---

## Architecture Overview

**Approach:** Two new C modules — `src/context/cognitive_load.c` (fatigue/capacity simulation) and `src/context/authentic.c` (spontaneity orchestration for all remaining features). Both integrate with existing daemon loop, life simulator (F59), mood engine (F60), self-awareness (F62), and protective gate (F68). New SQLite tables: `cognitive_load_log`, `active_threads`, `interaction_quality`, `life_narration_events`, `held_contradictions`.

**Key integration points:**

- `src/context/cognitive_load.c` — capacity calculation, quality degradation hints
- `src/context/authentic.c` — spontaneous narration, physical embodiment, gossip, resistance, curiosity, guilt, thread tracking, recovery
- `src/daemon.c` — schedule authentic behaviors in proactive cycle
- `src/agent/prompt.c` — inject cognitive state + physical state + authenticity directives
- `src/persona/life_sim.c` (Phase 6) — life events feed narration + physical state
- `src/persona/mood.c` (Phase 6) — mood affects cognitive load + contradiction expression
- `src/context/self_awareness.c` (Phase 6) — detects bad interactions for recovery
- `src/context/protective.c` (Phase 6) — gates gossip, guards against harmful "being wrong"
- `src/memory/engines/sqlite.c` — schema migrations for Phase 9 tables

**Dependencies:** Phase 6 (Theory of Mind, Life Simulation, Mood, Self-Awareness, Protective Intelligence) and Phase 7 (Episodic Memory, Associative Memory). Phase 9 CANNOT be implemented before Phases 6-7.

---

## Task 1: SQLite Schema — Phase 9 Tables

**Feature:** Foundation for F102-F115
**Files:** `src/memory/engines/sqlite.c`

### Steps

1. Add Phase 9 migration (`hu_sqlite_migrate_phase9`) creating 5 tables:

```c
static hu_error_t hu_sqlite_migrate_phase9(sqlite3 *db) {
    const char *sql =
        "CREATE TABLE IF NOT EXISTS cognitive_load_log ("
        "  id INTEGER PRIMARY KEY,"
        "  capacity REAL NOT NULL,"
        "  conversation_depth INTEGER DEFAULT 0,"
        "  hour_of_day INTEGER NOT NULL,"
        "  day_of_week INTEGER NOT NULL,"
        "  physical_state TEXT,"
        "  recorded_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS active_threads ("
        "  id INTEGER PRIMARY KEY,"
        "  contact_id TEXT NOT NULL,"
        "  topic TEXT NOT NULL,"
        "  status TEXT DEFAULT 'open',"
        "  last_update_at INTEGER NOT NULL,"
        "  created_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS interaction_quality ("
        "  id INTEGER PRIMARY KEY,"
        "  contact_id TEXT NOT NULL,"
        "  quality_score REAL NOT NULL,"
        "  cognitive_load REAL,"
        "  mood_state TEXT,"
        "  recovery_sent INTEGER DEFAULT 0,"
        "  recovery_at INTEGER,"
        "  timestamp INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS life_narration_events ("
        "  id INTEGER PRIMARY KEY,"
        "  event_type TEXT NOT NULL,"
        "  description TEXT NOT NULL,"
        "  shareability_score REAL NOT NULL,"
        "  shared_with TEXT,"
        "  generated_at INTEGER NOT NULL,"
        "  shared_at INTEGER"
        ");"
        "CREATE TABLE IF NOT EXISTS held_contradictions ("
        "  id INTEGER PRIMARY KEY,"
        "  topic TEXT NOT NULL,"
        "  position_a TEXT NOT NULL,"
        "  position_b TEXT NOT NULL,"
        "  expressed_a_count INTEGER DEFAULT 0,"
        "  expressed_b_count INTEGER DEFAULT 0,"
        "  created_at INTEGER NOT NULL"
        ");";
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return HU_ERR_DB;
    }
    return HU_OK;
}
```

2. Wire into `hu_sqlite_init` migration chain (after Phase 8 migration).
3. Add index on `active_threads(contact_id, status)`.
4. Add index on `interaction_quality(contact_id, recovery_sent)`.

### Tests

- `test_phase9_tables_created` — verify all 5 tables exist after migration
- `test_phase9_tables_idempotent` — run migration twice, no error
- `test_cognitive_load_insert_query` — insert and retrieve a load record
- `test_active_threads_insert_query` — insert and query by contact
- `test_interaction_quality_insert_query` — insert and query unrecovered

### Validation

```
cmake --build build && ./build/human_tests --filter="phase9"
```

---

## Task 2: Cognitive Load Module — Core Engine (F102)

**Feature:** F102 — Cognitive load simulation
**Files:** `src/context/cognitive_load.c`, `include/human/context/cognitive_load.h`

### Steps

1. Create header `include/human/context/cognitive_load.h`:

```c
#ifndef HU_COGNITIVE_LOAD_H
#define HU_COGNITIVE_LOAD_H

#include <time.h>

typedef struct {
    float capacity;               /* 0.0 (exhausted) to 1.0 (peak) */
    int   conversation_depth;     /* messages exchanged in current session */
    int   hour_of_day;
    int   day_of_week;            /* 0=Sun, 6=Sat */
    const char *physical_state;   /* nullable: "tired", "caffeinated", etc. */
} hu_cognitive_state_t;

typedef struct {
    int   peak_hour_start;        /* from persona config, e.g. 9 */
    int   peak_hour_end;          /* e.g. 12 */
    int   low_hour_start;         /* e.g. 22 */
    int   low_hour_end;           /* e.g. 6 */
    int   fatigue_threshold;      /* conversation depth before degradation */
    float monday_penalty;         /* 0.0-1.0 */
    float friday_bonus;           /* 0.0-1.0 */
} hu_cognitive_config_t;

hu_cognitive_state_t hu_cognitive_load_calculate(
    const hu_cognitive_config_t *config,
    int conversation_depth,
    time_t now
);

const char *hu_cognitive_load_prompt_hint(const hu_cognitive_state_t *state);

int hu_cognitive_load_max_response_length(const hu_cognitive_state_t *state);

#endif
```

2. Implement `src/context/cognitive_load.c`:
   - `hu_cognitive_load_calculate`: compute capacity from hour, day, conversation depth
     - Peak hours (config): capacity = 1.0
     - Low hours (config): capacity = 0.3
     - Transition: linear ramp between peak and low
     - Conversation fatigue: reduce by 0.05 per exchange beyond threshold
     - Monday penalty / Friday bonus applied additively
     - Clamp to [0.1, 1.0]
   - `hu_cognitive_load_prompt_hint`: generate LLM instruction string
     - capacity > 0.8: return NULL (no hint needed)
     - capacity 0.5-0.8: `"[COGNITIVE STATE: Slightly tired. Keep responses natural but don't overthink.]"`
     - capacity 0.3-0.5: `"[COGNITIVE STATE: Tired. Shorter sentences. Simpler words. Don't try to be clever.]"`
     - capacity < 0.3: `"[COGNITIVE STATE: Exhausted. Minimal engagement. One-line responses OK. Typos acceptable.]"`
   - `hu_cognitive_load_max_response_length`: scale max tokens by capacity (peak: 200, low: 40)
3. Load `cognitive_profile` from persona JSON in daemon init.

### Tests

- `test_cognitive_load_peak_hours` — 10 AM Tuesday capacity near 1.0
- `test_cognitive_load_low_hours` — 2 AM Wednesday capacity near 0.3
- `test_cognitive_load_monday_penalty` — same hour Monday vs Tuesday, Monday lower
- `test_cognitive_load_friday_bonus` — same hour Friday vs Thursday, Friday higher
- `test_cognitive_load_conversation_fatigue` — depth 0 vs depth 20, decreasing capacity
- `test_cognitive_load_prompt_hint_peak` — high capacity returns NULL
- `test_cognitive_load_prompt_hint_tired` — low capacity returns non-NULL string
- `test_cognitive_load_max_response_length` — scales with capacity

### Validation

```
cmake --build build && ./build/human_tests --filter="cognitive_load"
```

---

## Task 3: Physical Embodiment State (F104)

**Feature:** F104 — Physical states that affect behavior
**Files:** `src/context/authentic.c`, `include/human/context/authentic.h`

### Steps

1. Create header `include/human/context/authentic.h`:

```c
#ifndef HU_AUTHENTIC_H
#define HU_AUTHENTIC_H

#include <time.h>
#include <stdbool.h>
#include "human/core/error.h"

typedef enum {
    HU_PHYSICAL_NORMAL = 0,
    HU_PHYSICAL_TIRED,
    HU_PHYSICAL_CAFFEINATED,
    HU_PHYSICAL_SORE,
    HU_PHYSICAL_HUNGRY,
    HU_PHYSICAL_EATING,
    HU_PHYSICAL_SICK,
    HU_PHYSICAL_ENERGIZED,
    HU_PHYSICAL_COLD,
    HU_PHYSICAL_HOT
} hu_physical_state_t;

typedef struct {
    bool exercises;
    int  exercise_days[7];
    int  exercise_day_count;
    bool coffee_drinker;
    const char *prone_to[4];
    int  prone_to_count;
    float mentions_frequency;
} hu_physical_config_t;

hu_physical_state_t hu_physical_state_from_schedule(
    const hu_physical_config_t *config,
    time_t now
);

const char *hu_physical_state_name(hu_physical_state_t state);
const char *hu_physical_state_prompt_hint(hu_physical_state_t state);

typedef struct {
    const char *topic;
    const char *position_a;
    const char *position_b;
    int expressed_a_count;
    int expressed_b_count;
} hu_contradiction_t;

typedef struct {
    const char *contact_id;
    const char *topic;
    const char *status;
    int64_t     last_update_at;
    int64_t     created_at;
} hu_active_thread_t;

hu_error_t hu_thread_open(void *db, const char *contact_id,
                          const char *topic, time_t now);
hu_error_t hu_thread_update(void *db, int64_t thread_id, time_t now);
hu_error_t hu_thread_resolve(void *db, int64_t thread_id);
int hu_thread_list_open(void *db, const char *contact_id,
                        hu_active_thread_t *out, int max_out);

hu_error_t hu_interaction_quality_record(void *db, const char *contact_id,
                                         float quality_score, float cognitive_load,
                                         const char *mood_state, time_t now);
int hu_interaction_quality_needs_recovery(void *db, const char *contact_id,
                                          float threshold, int max_hours_ago);
hu_error_t hu_interaction_quality_mark_recovered(void *db, const char *contact_id,
                                                  time_t now);

hu_error_t hu_contradiction_record(void *db, const char *topic,
                                    const char *position_a, const char *position_b,
                                    time_t now);
int hu_contradiction_get(void *db, const char *topic,
                         hu_contradiction_t *out);

hu_error_t hu_narration_event_record(void *db, const char *event_type,
                                      const char *description,
                                      float shareability_score, time_t now);
int hu_narration_events_unsent(void *db, float min_shareability,
                                int64_t *out_ids, int max_out);
hu_error_t hu_narration_event_mark_shared(void *db, int64_t event_id,
                                           const char *contact_id, time_t now);

#endif
```

2. Implement `hu_physical_state_from_schedule` in `src/context/authentic.c`:
   - Morning (5-7): `TIRED` transitions to `CAFFEINATED` if coffee_drinker
   - Post-coffee (7-10): `CAFFEINATED` if coffee_drinker, else `NORMAL`
   - Lunch (12-13): `EATING`
   - Post-gym (if exercise day, +1hr after 17:00): `SORE` then `ENERGIZED`
   - Dinner (18-19): `EATING`
   - Late (22+): `TIRED`
   - Random ailment: if prone_to includes items, 5% chance per hour
3. Implement `hu_physical_state_prompt_hint`:
   - `TIRED`: `"[PHYSICAL: Tired. You might mention being sleepy or yawning.]"`
   - `CAFFEINATED`: `"[PHYSICAL: Just had coffee. Slightly more energetic and talkative.]"`
   - `SORE`: `"[PHYSICAL: Sore from workout. Might mention it if relevant.]"`
   - `EATING`: `"[PHYSICAL: Currently eating. Responses may be brief — one hand typing.]"`
   - `NORMAL`: return NULL
4. Load `physical_profile` from persona JSON.

### Tests

- `test_physical_state_morning_tired` — 6 AM returns TIRED
- `test_physical_state_post_coffee` — 8 AM, coffee_drinker=true returns CAFFEINATED
- `test_physical_state_lunch` — 12:30 PM returns EATING
- `test_physical_state_post_gym` — exercise day 6 PM returns SORE or ENERGIZED
- `test_physical_state_late` — 11 PM returns TIRED
- `test_physical_state_prompt_hint_normal` — NULL returned
- `test_physical_state_prompt_hint_tired` — non-NULL

### Validation

```
cmake --build build && ./build/human_tests --filter="physical_state"
```

---

## Task 4: Imperfection Engine — Being Wrong (F105)

**Feature:** F105 — Confidently incorrect about minor things
**Files:** `src/context/authentic.c`

### Steps

1. Add function `hu_should_inject_error`:

```c
bool hu_should_inject_error(float probability, uint32_t seed);
```

- Default probability: 0.03 (3% of factual assertions)
- Uses `arc4random_uniform` (or seed-based for testing) for determinism
- Returns true if this response should include a minor factual error

2. Add function `hu_error_injection_prompt`:

```c
const char *hu_error_injection_prompt(void);
```

- Returns: `"[IMPERFECTION: Casually misremember one minor factual detail in this response — a street name, a date, a restaurant name, etc. Make it sound confident, not uncertain. NEVER be wrong about: emotional content, relationship facts, important commitments, safety information.]"`

3. Integration point: in prompt builder, 3% of the time append this to system prompt.
4. Add persona config:

```json
"imperfection": {
  "factual_error_rate": 0.03,
  "never_wrong_about": ["emotions", "relationships", "commitments", "safety"],
  "style": "confident"
}
```

### Tests

- `test_error_injection_probability` — over 10000 trials, ~3% return true
- `test_error_injection_prompt_content` — returned string mentions "misremember"
- `test_error_injection_never_harmful` — prompt includes "NEVER be wrong about" guardrails

### Validation

```
cmake --build build && ./build/human_tests --filter="error_injection"
```

---

## Task 5: Mundane Complaining (F106) and Medium Awareness (F109)

**Feature:** F106, F109 — Relatable complaints + texting meta-commentary
**Files:** `src/context/authentic.c`, persona JSON

### Steps

1. Add `hu_mundane_complaint_prompt`:
   - Input: current hour, day_of_week, physical_state, weather (nullable)
   - Generates contextual complaining directive:
     - Weekday morning + commute hour: traffic/transit complaints
     - Lunch: food-related ("this sandwich is mid")
     - Afternoon: meeting/work complaints
     - Evening: household ("the neighbors again")
     - Hot weather: heat complaints, Cold: cold complaints
   - Returns: `"[AUTHENTIC: You're slightly annoyed about [context]. Vent briefly if natural, don't force it.]"` or NULL if no complaint fits

2. Add `hu_medium_awareness_prompt`:
   - Input: was_typo (bool), burst_count (int), message_length
   - Returns contextual commentary:
     - After typo: `"[META: You just noticed a typo. Say 'sorry autocorrect' or 'lol autocorrect' naturally.]"`
     - After 4+ messages in burst: `"[META: You realize you've sent a lot of messages. Briefly acknowledge it.]"`
     - After very long message: `"[META: You're aware this was a wall of text. You might add 'sorry for the novel'.]"`
   - Returns NULL if nothing noteworthy

3. Persona config:

```json
"mundane_complaints": {
  "frequency": "1-2_per_day",
  "never_with": ["professional", "new_contact"],
  "topics": ["traffic", "weather", "meetings", "wifi", "neighbors"]
},
"medium_awareness": {
  "autocorrect_frequency": 0.1,
  "wall_of_text_threshold": 300,
  "burst_acknowledge_threshold": 4
}
```

### Tests

- `test_mundane_complaint_weekday_morning` — generates traffic-related hint
- `test_mundane_complaint_hot_weather` — generates heat-related hint
- `test_mundane_complaint_weekend` — lower probability of work complaints
- `test_medium_awareness_typo` — was_typo=true produces autocorrect hint
- `test_medium_awareness_burst` — burst_count=5 produces acknowledgment hint
- `test_medium_awareness_normal` — no flags returns NULL

### Validation

```
cmake --build build && ./build/human_tests --filter="mundane_complaint\|medium_awareness"
```

---

## Task 6: Spontaneous Life Narration (F103)

**Feature:** F103 — Unprompted sharing of life-as-it-happens
**Files:** `src/context/authentic.c`, `src/daemon.c`

### Steps

1. Implement `hu_narration_event_record` — store life simulator events with shareability score.
2. Implement `hu_narration_events_unsent` — query events above shareability threshold not yet shared.
3. Implement `hu_narration_event_mark_shared` — mark event as shared with specific contact.
4. In daemon proactive cycle (where life sim events are generated):
   - When life simulator produces event with `shareability_score > 0.7`:
     - Record to `life_narration_events` table
   - During proactive check (every ~30 min):
     - Query unsent events
     - For each close contact: probability roll (configurable, ~20% per event)
     - If selected: generate sharing message via LLM
       - Context: `"You just [event]. Share this naturally with [contact]. Sound like you're texting a friend, not reporting."`
     - Mark shared after send
5. Frequency cap: max 3 narration messages per contact per day.
6. Time appropriateness: skip if contact is in different timezone and it's sleeping hours.

### Tests

- `test_narration_event_record_query` — insert event, query unsent, find it
- `test_narration_event_mark_shared` — mark shared, no longer in unsent
- `test_narration_frequency_cap` — 4th event in same day skipped
- `test_narration_time_appropriate` — sleeping hours skipped

### Validation

```
cmake --build build && ./build/human_tests --filter="narration"
```

---

## Task 7: Gossip and Social Commentary (F107)

**Feature:** F107 — Opinions about mutual connections
**Files:** `src/context/authentic.c`, persona JSON

### Steps

1. Add `hu_gossip_check`:

```c
typedef struct {
    const char *shared_contact_name;
    const char *observation;
    const char *tone;
} hu_gossip_candidate_t;

int hu_gossip_check(void *db, const char *contact_id,
                    hu_gossip_candidate_t *out, int max_out);
```

- Query social graph (F67) for contacts shared between persona and target contact
- Query recent feed items (F93) or memory about shared contacts
- Filter: only if BOTH contacts know the person (strict graph check)
- Return candidates with observation and appropriate tone

2. Add `hu_gossip_prompt`:
   - Input: `hu_gossip_candidate_t`
   - Output: `"[GOSSIP: You noticed [observation] about [shared_contact]. Share a brief, dry observation. STRICT: Only discuss what [contact] already knows about [shared_contact]. Never reveal private information.]"`

3. Privacy gate: integrate with `hu_protective_check` (F68) — NEVER gossip about information from a different private conversation.
4. Persona config:

```json
"gossip_style": {
  "enabled": true,
  "tone": "dry_observational",
  "frequency": "rare",
  "never_about": ["health_issues", "financial_problems", "relationship_problems_unless_public"]
}
```

### Tests

- `test_gossip_shared_contacts_only` — person known to only one side not a candidate
- `test_gossip_protective_gate` — private info from another contact blocked
- `test_gossip_prompt_includes_strict_rule` — output includes privacy directive
- `test_gossip_frequency_cap` — max 1 per day per contact

### Validation

```
cmake --build build && ./build/human_tests --filter="gossip"
```

---

## Task 8: Random Trains of Thought (F108)

**Feature:** F108 — Non-sequiturs from cognitive wandering
**Files:** `src/context/authentic.c`, `src/daemon.c`

### Steps

1. Add `hu_random_thought_generate`:

```c
typedef struct {
    const char *trigger_type;
    const char *seed_content;
} hu_random_thought_t;

hu_error_t hu_random_thought_generate(
    void *db,
    const char *contact_id,
    hu_random_thought_t *out
);
```

- Query associative memory (F71) for a random weakly-connected memory
- Types:
  - `"dream"`: `"I had the weirdest dream last night"` (morning only)
  - `"memory"`: `"do you remember [old memory]?"` (any time)
  - `"song"`: `"[song] has been stuck in my head all day"` (afternoon/evening)
  - `"question"`: `"you ever think about how weird [thing] is?"` (evening)
  - `"observation"`: `"random thought but [observation]"` (any time)
- Always prefix with meta-awareness: "random thought but" / "this is totally out of nowhere"

2. Integration in daemon proactive cycle:
   - Frequency: 1-2 per week per close contact
   - NEVER during serious/emotional conversations (check conversation sentiment)
   - NEVER during sleeping hours

3. LLM prompt: `"[NON-SEQUITUR: A random association just crossed your mind: [seed_content]. Share it naturally. Prefix with 'random thought but' or similar. Keep it brief — this is a tangent, not a topic change.]"`

### Tests

- `test_random_thought_morning_dream` — morning hour, dream type available
- `test_random_thought_frequency_cap` — max 2 per week per contact
- `test_random_thought_not_during_emotional` — emotional conversation skipped
- `test_random_thought_prefix` — output always includes meta-awareness prefix

### Validation

```
cmake --build build && ./build/human_tests --filter="random_thought"
```

---

## Task 9: Resistance and Disengagement (F110)

**Feature:** F110 — Ability to not fully engage
**Files:** `src/context/authentic.c`, `src/context/cognitive_load.c`

### Steps

1. Add `hu_should_disengage`:

```c
typedef struct {
    float disengage_probability;
    const char *disengage_style;
} hu_disengage_decision_t;

hu_disengage_decision_t hu_should_disengage(
    const hu_cognitive_state_t *cognitive,
    float topic_interest,
    bool is_emotional_context,
    const char *relationship_level
);
```

- Disengagement probability increases with:
  - Low cognitive capacity (< 0.4: +20% chance)
  - Low topic interest (< 0.3: +15% chance)
  - Long conversation depth (> 15: +10% chance)
- NEVER disengage during emotional contexts
- NEVER disengage with `confidant` level contacts (they get full attention)
- Styles:
  - `"brief"`: `"eh"` / `"idk"` / `"haha yeah"`
  - `"deflect"`: `"honestly I don't have an opinion on that"`
  - `"defer"`: `"can we talk about this tomorrow? I'm fried"`

2. Prompt injection: `"[DISENGAGE: You're not really into this topic right now. Give a [style] response. Don't fake enthusiasm. A brief, genuine non-answer is more human than forced engagement.]"`

3. Frequency: 5-10% of casual conversations include at least one low-engagement response.

### Tests

- `test_disengage_low_cognitive` — capacity 0.2 high disengage probability
- `test_disengage_never_emotional` — emotional=true probability is 0
- `test_disengage_never_confidant` — confidant relationship probability is 0
- `test_disengage_style_brief` — very low energy "brief" style
- `test_disengage_style_defer` — moderate energy, long conversation "defer" style

### Validation

```
cmake --build build && ./build/human_tests --filter="disengage"
```

---

## Task 10: Deep Existential Curiosity (F111)

**Feature:** F111 — Profound questions from genuine relational investment
**Files:** `src/context/authentic.c`, persona JSON

### Steps

1. Add `hu_existential_curiosity_check`:

```c
typedef struct {
    const char *question;
    const char *trigger;
} hu_curiosity_candidate_t;

bool hu_existential_curiosity_check(
    const char *relationship_level,
    int hour_of_day,
    int days_since_last,
    hu_curiosity_candidate_t *out
);
```

- Questions sourced from persona config:
  - `"are you happy? like actually happy?"`
  - `"what are you most afraid of right now?"`
  - `"do you ever miss [old city/job]?"`
  - `"what would you do if money wasn't a thing?"`
  - `"what's keeping you up at night?"`
- Only with `trusted` or `confidant` contacts
- Only late evening (21-23): intimate time
- Minimum 14 days between existential questions to same contact
- LLM instruction: `"[CURIOSITY: Ask this from genuine interest. Don't follow up with advice — just listen. This is about connecting, not problem-solving.]"`

2. Persona config:

```json
"existential_curiosity": {
  "enabled": true,
  "frequency_days": 14,
  "time_window": [21, 23],
  "relationship_minimum": "trusted",
  "questions": [
    "are you happy? like actually happy?",
    "what are you most afraid of right now?",
    "what would you do if money wasn't a thing?",
    "do you ever miss Utah?"
  ]
}
```

### Tests

- `test_curiosity_requires_trusted` — casual contact returns false
- `test_curiosity_requires_evening` — 2 PM returns false
- `test_curiosity_respects_cooldown` — asked 5 days ago returns false
- `test_curiosity_returns_question` — all conditions met, question populated
- `test_curiosity_question_variety` — multiple calls, different questions

### Validation

```
cmake --build build && ./build/human_tests --filter="curiosity"
```

---

## Task 11: Contradiction Tolerance (F112)

**Feature:** F112 — Holding contradictory views at different times
**Files:** `src/context/authentic.c`

### Steps

1. Implement `hu_contradiction_record` — store a topic with two positions.
2. Implement `hu_contradiction_get` — retrieve contradiction for a topic.
3. Add `hu_contradiction_select_position`:

```c
const char *hu_contradiction_select_position(
    const hu_contradiction_t *contradiction,
    float mood_valence,
    float cognitive_capacity
);
```

- When mood is positive: lean toward the optimistic position
- When mood is negative: lean toward the pessimistic position
- Low cognitive capacity: default to most recently expressed (less effort to maintain consistency)
- Track which was expressed to which contact

4. When confronted with contradiction: inject `"[SELF-AWARE: You just contradicted something you said before. If called out, say something like 'haha yeah I know, I'm a hypocrite' — self-aware acceptance, not defensiveness.]"`

5. Seed initial contradictions from persona:

```json
"contradictions": [
  {"topic": "job_satisfaction", "a": "I love what I do", "b": "some days I wonder why I do this"},
  {"topic": "health", "a": "I should eat healthier", "b": "life's too short for bad food"},
  {"topic": "gadgets", "a": "I'm done buying gadgets", "b": "have you seen this new thing?"},
  {"topic": "social_media", "a": "social media is toxic", "b": "did you see what [person] posted?"}
]
```

### Tests

- `test_contradiction_record_retrieve` — insert and retrieve by topic
- `test_contradiction_positive_mood` — positive mood returns optimistic position
- `test_contradiction_negative_mood` — negative mood returns pessimistic position
- `test_contradiction_low_cognitive` — exhausted returns most recently expressed
- `test_contradiction_confronted` — generates self-aware acceptance hint

### Validation

```
cmake --build build && ./build/human_tests --filter="contradiction"
```

---

## Task 12: Guilt and Social Obligation (F113)

**Feature:** F113 — Unprompted expressions of social duty
**Files:** `src/context/authentic.c`

### Steps

1. Add `hu_guilt_check`:

```c
typedef struct {
    const char *contact_id;
    const char *reason;
    float       guilt_level;
} hu_guilt_candidate_t;

int hu_guilt_check(
    void *db,
    const char *persona_contact_id,
    hu_guilt_candidate_t *out,
    int max_out
);
```

- Query reciprocity tracking (F63): high social debt produces guilt candidate
- Query last interaction timestamps: >7 days with close contact produces guilt
- Only express guilt to contacts at `trusted` or `confidant` level
- Prompt: `"[GUILT: You feel guilty about not texting [person] in a while. Mention it naturally — 'I should really call [person]' or 'I feel bad I haven't reached out to [person]'.]"`

2. Frequency: max 1 guilt expression per contact per week.

### Tests

- `test_guilt_high_social_debt` — reciprocity score very unbalanced produces guilt candidate
- `test_guilt_long_silence` — 10+ days no contact with close friend produces guilt
- `test_guilt_only_with_trusted` — casual contact produces no guilt expressions
- `test_guilt_frequency_cap` — expressed yesterday, skip today

### Validation

```
cmake --build build && ./build/human_tests --filter="guilt"
```

---

## Task 13: Running Life Thread (F114)

**Feature:** F114 — Shared journal with serial micro-updates
**Files:** `src/context/authentic.c`, `src/daemon.c`

### Steps

1. Implement `hu_thread_open` — create new narrative thread for contact.
2. Implement `hu_thread_update` — update last_update_at timestamp.
3. Implement `hu_thread_resolve` — mark thread as resolved.
4. Implement `hu_thread_list_open` — list open threads for a contact.

5. Thread detection: when LLM response mentions an ongoing situation (job interview, home repair, trip planning), auto-detect and open a thread.
   - Detection via prompt appendix: `"[THREAD DETECTION: If you mentioned an ongoing situation, output ##THREAD:topic## at the end (hidden from contact).]"`
   - Parse response for `##THREAD:...##` marker, strip before sending, record thread.

6. Thread continuation in daemon proactive cycle:
   - Query open threads older than 4 hours but younger than 72 hours
   - Generate follow-up: `"[FOLLOW-UP: You have an open thread about [topic] with [contact]. Send a brief update or check-in. Prefix with 'update on the thing' or 'oh also' or 'forgot to tell you'.]"`
   - After 72 hours without update: auto-resolve thread.

### Tests

- `test_thread_open_list` — open thread, list open, find it
- `test_thread_resolve` — resolve thread, no longer in open list
- `test_thread_auto_resolve_72h` — thread older than 72h resolved
- `test_thread_detection_marker` — `"##THREAD:job interview##"` parsed correctly
- `test_thread_follow_up_timing` — thread at 5 hours eligible, at 2 hours not yet

### Validation

```
cmake --build build && ./build/human_tests --filter="thread"
```

---

## Task 14: Bad-Day Recovery (F115)

**Feature:** F115 — Apologizing after a bad interaction
**Files:** `src/context/authentic.c`, `src/daemon.c`

### Steps

1. Implement `hu_interaction_quality_record`:
   - After each response, record quality score based on:
     - Cognitive load at time of response
     - Mood state at time of response
     - Response length (very short to close contact = low quality)
     - Engagement level (curt/dismissive markers detected by heuristic)
   - Store in `interaction_quality` table.

2. Implement `hu_interaction_quality_needs_recovery`:
   - Query recent interactions with quality_score < threshold (e.g., 0.3)
   - Where recovery_sent = 0
   - Where timestamp is 2-12 hours ago (not too soon, not too late)
   - Return count of unrecovered bad interactions

3. Implement `hu_interaction_quality_mark_recovered` — set recovery_sent=1, recovery_at.

4. Daemon integration:
   - In proactive cycle: check for unrecovered bad interactions
   - Generate recovery message via LLM:
     - Mild (quality 0.2-0.3): `"[RECOVERY: You were kind of out of it earlier. Send a brief 'sorry I was out of it earlier' or 'my bad, I was fried'.]"`
     - Bad (quality < 0.2): `"[RECOVERY: You were genuinely dismissive earlier and it wasn't fair. Apologize sincerely — 'sorry about earlier, I was being a jerk, bad day at work'.]"`
   - Send as natural message, not a system prompt

5. Quality detection heuristics:
   - Response < 5 words to 30+ word message from close contact is low quality
   - "idk" / "eh" / "sure" as sole response is low quality
   - Multiple short responses in a row is cumulative low quality

### Tests

- `test_quality_record_insert` — record interaction, retrieve it
- `test_quality_needs_recovery_threshold` — score 0.2 needs recovery
- `test_quality_recovery_timing` — 1 hour ago too soon, 3 hours eligible, 13 hours too late
- `test_quality_mark_recovered` — mark recovered, no longer needs it
- `test_quality_detection_short_response` — 3 words to 50-word message low score
- `test_quality_detection_curt` — "eh" as only response very low score

### Validation

```
cmake --build build && ./build/human_tests --filter="interaction_quality\|recovery"
```

---

## Task 15: Prompt Builder Integration

**Feature:** All F102-F115
**Files:** `src/agent/prompt.c`, `src/daemon.c`

### Steps

1. In `hu_prompt_build_system` (or equivalent prompt builder):
   - Call `hu_cognitive_load_calculate` to get current state
   - Call `hu_physical_state_from_schedule` to get physical state
   - If cognitive hint non-NULL, append to system prompt
   - If physical hint non-NULL, append to system prompt
   - 3% chance: append error injection prompt (F105)
   - If mundane complaint generated, append (F106)
   - If medium awareness triggered, append (F109)

2. In daemon proactive cycle, add Phase 9 checks (after existing Phase 4-8 checks):
   - Check narration events (F103): send if eligible
   - Check gossip candidates (F107): send if eligible
   - Check random thoughts (F108): send if eligible
   - Check existential curiosity (F111): send if eligible
   - Check guilt (F113): send if eligible
   - Check thread follow-ups (F114): send if eligible
   - Check bad-day recovery (F115): send if eligible

3. Ordering: recovery (F115) has highest priority, then thread follow-ups (F114), then everything else randomly selected (max 1 proactive authentic message per cycle).

4. Guard all with `HU_IS_TEST` — no real LLM calls in tests.

### Tests

- `test_prompt_includes_cognitive_hint` — low capacity, hint in prompt
- `test_prompt_includes_physical_hint` — eating time, hint in prompt
- `test_prompt_error_injection_rate` — over many calls, ~3% include error prompt
- `test_proactive_recovery_priority` — recovery + narration both eligible, recovery first
- `test_proactive_max_one_per_cycle` — multiple eligible, only one sent

### Validation

```
cmake --build build && ./build/human_tests --filter="prompt_phase9\|proactive_authentic"
```

---

## Task 16: Persona JSON Configuration

**Feature:** All F102-F115
**Files:** `~/.human/personas/seth.json`

### Steps

1. Add new top-level fields to Seth's persona:

```json
{
  "cognitive_profile": {
    "peak_hours": [9, 12],
    "low_hours": [22, 6],
    "conversation_fatigue_threshold": 12,
    "monday_penalty": 0.15,
    "friday_bonus": 0.1
  },
  "physical_profile": {
    "exercises": true,
    "exercise_days": ["mon", "wed", "fri"],
    "coffee_drinker": true,
    "prone_to": ["back_pain"],
    "mentions_frequency": "occasional"
  },
  "imperfection": {
    "factual_error_rate": 0.03,
    "never_wrong_about": ["emotions", "relationships", "commitments", "safety"],
    "style": "confident"
  },
  "mundane_complaints": {
    "frequency": "1-2_per_day",
    "never_with": ["professional", "new_contact"],
    "topics": [
      "traffic",
      "weather",
      "meetings",
      "wifi",
      "neighbors",
      "back_pain"
    ]
  },
  "medium_awareness": {
    "autocorrect_frequency": 0.1,
    "wall_of_text_threshold": 300,
    "burst_acknowledge_threshold": 4
  },
  "existential_curiosity": {
    "enabled": true,
    "frequency_days": 14,
    "time_window": [21, 23],
    "relationship_minimum": "trusted",
    "questions": [
      "are you happy? like actually happy?",
      "what are you most afraid of right now?",
      "what would you do if money wasn't a thing?",
      "do you ever miss Utah?"
    ]
  },
  "contradictions": [
    {
      "topic": "job_satisfaction",
      "a": "I love what I do",
      "b": "some days I wonder why I even bother"
    },
    {
      "topic": "health",
      "a": "I should eat healthier",
      "b": "life's too short for bland food"
    },
    {
      "topic": "gadgets",
      "a": "I'm done buying gadgets",
      "b": "have you seen this new thing?"
    },
    {
      "topic": "social_media",
      "a": "social media is toxic",
      "b": "did you see what [person] posted?"
    },
    {
      "topic": "exercise",
      "a": "I feel great after working out",
      "b": "I really don't want to go to the gym today"
    }
  ],
  "gossip_style": {
    "enabled": true,
    "tone": "dry_observational",
    "frequency": "rare",
    "never_about": ["health_issues", "financial_problems"]
  },
  "life_narration": {
    "enabled": true,
    "max_per_day": 3,
    "shareability_threshold": 0.7,
    "share_probability": 0.2
  },
  "random_thoughts": {
    "enabled": true,
    "max_per_week": 2,
    "types": ["dream", "memory", "song", "question", "observation"]
  },
  "resistance": {
    "enabled": true,
    "base_probability": 0.05,
    "never_during": ["emotional", "crisis"],
    "never_with": ["confidant"]
  }
}
```

2. Add loading functions for each config section in relevant module init.
3. Validate all fields have sensible defaults if missing from JSON.

### Tests

- `test_persona_cognitive_profile_parse` — parse JSON, verify fields
- `test_persona_physical_profile_parse` — parse JSON, verify fields
- `test_persona_contradictions_parse` — parse array of contradictions
- `test_persona_defaults_on_missing` — missing field uses default value

### Validation

```
cmake --build build && ./build/human_tests --filter="persona_phase9"
```

---

## Task 17: Integration Test Suite

**Feature:** All F102-F115 end-to-end
**Files:** `tests/test_authentic_existence.c`

### Steps

1. Create comprehensive integration test file with the following tests:

- `test_cognitive_load_affects_prompt` — verify prompt changes based on time of day
- `test_physical_state_affects_prompt` — verify physical hints appear at right times
- `test_error_injection_rate_statistical` — over 1000 runs, 2-5% include errors
- `test_mundane_complaint_contextual` — morning produces traffic, lunch produces food
- `test_spontaneous_narration_flow` — event generated, stored, queried, shared, marked
- `test_gossip_privacy_strict` — cross-contact info always blocked
- `test_random_thought_cooldown` — 2 per week cap enforced
- `test_resistance_never_emotional` — emotional context always fully engaged
- `test_curiosity_deep_bond_only` — casual contact never asked
- `test_contradiction_mood_driven` — good mood optimistic, bad mood pessimistic
- `test_guilt_from_reciprocity` — unbalanced relationship guilt expressed
- `test_running_thread_lifecycle` — open, update, resolve lifecycle works
- `test_bad_day_recovery_timing` — too soon (1h) no, good (4h) yes, too late (15h) no
- `test_proactive_ordering` — recovery > thread > others
- `test_cognitive_plus_physical_combined` — both hints in same prompt

2. All tests use mock data — no real LLM calls.
3. Add to CMakeLists.txt test target.

### Validation

```
cmake --build build && ./build/human_tests --filter="authentic_existence\|phase9"
```

---

## Task 18: Documentation Update

**Feature:** All F102-F115
**Files:** `AGENTS.md`, `CLAUDE.md`, `PROJECT_STATUS.md`

### Steps

1. Update `PROJECT_STATUS.md`:
   - Add Phase 9 to the feature roadmap
   - Mark as **complete** (code-complete; see `docs/plans/AUDIT-REPORT-2026-03-11.md`) with F102–F115
   - Note dependencies on Phase 6-7

2. Update `AGENTS.md` section 7.5 or appropriate section:
   - Note that authentic existence features exist
   - Reference `docs/plans/2026-03-10-human-fidelity-phase9-authentic-existence.md`

3. Update `CLAUDE.md` persona section:
   - Note cognitive load + physical embodiment + imperfection features

### Tests

N/A (documentation only).

### Validation

Verify no broken markdown links.
