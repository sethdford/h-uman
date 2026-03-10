---
title: "Human Fidelity Phase 3 — Superhuman Memory & Intelligence"
created: 2026-03-10
status: draft
scope: memory, daemon, proactive, persona, conversation
phase: 3
features: [F18, F19, F20, F21, F22, F23, F24, F26, F30, F31, F50, F53]
parent: 2026-03-10-human-fidelity-design.md
---

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

# Human Fidelity Phase 3 — Superhuman Memory & Intelligence

Phase 3 of the Human Fidelity project. Implements BTH (Better Than Human) memory capabilities: micro-moments, inside jokes, commitments with deadlines, avoidance/pattern detection, temporal learning, spontaneous curiosity, callbacks, calendar awareness, and birthday/holiday messages.

**Reference:** `docs/plans/2026-03-10-human-fidelity-design.md`

---

## Architecture Overview

**Approach:** Extend existing systems — no new vtables. Add SQLite tables to `src/memory/engines/sqlite.c`, new helper module `src/memory/superhuman.c` for queries, extend `hu_proactive_check_extended` and daemon loop. Persona JSON gains `important_dates` and `context_awareness`.

**Key integration points:**

- `src/memory/engines/sqlite.c` — schema, migrations, raw table access
- `src/memory/superhuman.c` (new) — `hu_superhuman_*` API for all Phase 3 features
- `include/human/memory/superhuman.h` (new) — public API
- `src/agent/proactive.c` — new action types, extended check
- `src/daemon.c` — proactive cycle wiring
- `src/agent/agent_turn.c` — prompt injection for memory context
- `src/persona/persona.c` — `important_dates`, `context_awareness` parsing
- `include/human/persona.h` — new struct fields

---

## File Map

| File                                  | Responsibility                                                  |
| ------------------------------------- | --------------------------------------------------------------- |
| `src/memory/engines/sqlite.c`         | Schema (new tables), migration on open                          |
| `src/memory/superhuman.c`             | All `hu_superhuman_*` functions                                 |
| `include/human/memory/superhuman.h` | API declarations, structs                                       |
| `src/agent/proactive.c`               | New action types, calendar/birthday/inside-joke/callback checks |
| `src/daemon.c`                        | Proactive cycle: call superhuman queries, merge context         |
| `src/agent/agent_turn.c`              | Inject micro-moment, inside-joke, pattern, callback context     |
| `src/persona/persona.c`               | Parse `important_dates`, `context_awareness`                    |
| `include/human/persona.h`           | `hu_important_date_t`, `hu_context_awareness_t`                 |
| `scripts/calendar_query.applescript`  | macOS Calendar AppleScript (optional)                           |
| `tests/test_superhuman.c`             | Unit tests for storage, retrieval, edge cases                   |

---

## Task 1: SQLite schema — add Phase 3 tables

**Description:** Add new tables to the SQLite memory backend. Tables: `inside_jokes`, `commitments`, `temporal_patterns`, `delayed_followups`, `avoidance_patterns`, `topic_baselines`, `micro_moments`, `growth_milestones`. Schema runs on DB open (CREATE IF NOT EXISTS).

**Files:**

- Modify: `src/memory/engines/sqlite.c` (schema_parts array, init)

**Steps:**

1. Extend `schema_parts[]` with:

```sql
CREATE TABLE IF NOT EXISTS inside_jokes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contact_id TEXT NOT NULL,
    context TEXT NOT NULL,
    punchline TEXT,
    created_at INTEGER NOT NULL,
    last_referenced INTEGER,
    reference_count INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_inside_jokes_contact ON inside_jokes(contact_id);

CREATE TABLE IF NOT EXISTS commitments (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contact_id TEXT NOT NULL,
    description TEXT NOT NULL,
    who TEXT NOT NULL,
    deadline INTEGER,
    status TEXT DEFAULT 'pending',
    created_at INTEGER NOT NULL,
    followed_up_at INTEGER
);
CREATE INDEX IF NOT EXISTS idx_commitments_contact ON commitments(contact_id);
CREATE INDEX IF NOT EXISTS idx_commitments_deadline ON commitments(deadline) WHERE status='pending';

CREATE TABLE IF NOT EXISTS temporal_patterns (
    contact_id TEXT NOT NULL,
    day_of_week INTEGER NOT NULL,
    hour INTEGER NOT NULL,
    message_count INTEGER DEFAULT 0,
    avg_response_time_ms INTEGER,
    PRIMARY KEY (contact_id, day_of_week, hour)
);

CREATE TABLE IF NOT EXISTS delayed_followups (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contact_id TEXT NOT NULL,
    topic TEXT NOT NULL,
    scheduled_at INTEGER NOT NULL,
    sent INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_delayed_followups_contact ON delayed_followups(contact_id);
CREATE INDEX IF NOT EXISTS idx_delayed_followups_scheduled ON delayed_followups(scheduled_at) WHERE sent=0;

CREATE TABLE IF NOT EXISTS avoidance_patterns (
    contact_id TEXT NOT NULL,
    topic TEXT NOT NULL,
    mention_count INTEGER DEFAULT 0,
    change_count INTEGER DEFAULT 0,
    last_mentioned INTEGER,
    PRIMARY KEY (contact_id, topic)
);

CREATE TABLE IF NOT EXISTS topic_baselines (
    contact_id TEXT NOT NULL,
    topic TEXT NOT NULL,
    mention_count INTEGER DEFAULT 0,
    last_mentioned INTEGER,
    PRIMARY KEY (contact_id, topic)
);

CREATE TABLE IF NOT EXISTS micro_moments (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contact_id TEXT NOT NULL,
    fact TEXT NOT NULL,
    significance TEXT,
    created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_micro_moments_contact ON micro_moments(contact_id);

CREATE TABLE IF NOT EXISTS growth_milestones (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contact_id TEXT NOT NULL,
    topic TEXT NOT NULL,
    before_state TEXT,
    after_state TEXT,
    created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_growth_milestones_contact ON growth_milestones(contact_id);

CREATE TABLE IF NOT EXISTS pattern_observations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contact_id TEXT NOT NULL,
    topic TEXT NOT NULL,
    tone TEXT NOT NULL,
    day_of_week INTEGER,
    hour INTEGER,
    observed_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_pattern_observations_contact ON pattern_observations(contact_id);
```

2. Ensure schema runs during `hu_sqlite_memory_create` (existing loop over schema_parts).

**Tests:**

- Open SQLite memory, run `PRAGMA table_info(inside_jokes)` via test harness or raw SQL; verify columns exist.
- `tests/test_sqlite.c` or new `test_superhuman.c`: create memory, verify no error; list tables.

**Validation:**

- `./build/human_tests` — 0 failures
- New DB file has all tables after first open.

---

## Task 2: Superhuman module — API and SQLite access

**Description:** Create `src/memory/superhuman.c` and `include/human/memory/superhuman.h` with functions to read/write Phase 3 tables. Uses raw SQLite via `hu_sqlite_memory_get_db` or similar — or takes `sqlite3*` from sqlite memory ctx. Prefer: add `hu_sqlite_memory_get_db(sqlite3 **out)` to sqlite.c and have superhuman.c call it when given a `hu_memory_t` that is sqlite-backed.

**Files:**

- Create: `include/human/memory/superhuman.h`
- Create: `src/memory/superhuman.c`
- Modify: `src/memory/engines/sqlite.c` — add `hu_sqlite_memory_get_db` if not present
- Modify: `CMakeLists.txt` — add superhuman.c

**Steps:**

1. **Header** — declare:

```c
#ifndef HU_MEMORY_SUPERHUMAN_H
#define HU_MEMORY_SUPERHUMAN_H
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

typedef struct hu_inside_joke {
    int64_t id;
    char *contact_id;
    char *context;
    char *punchline;
    int64_t created_at;
    int64_t last_referenced;
    uint32_t reference_count;
} hu_inside_joke_t;

typedef struct hu_superhuman_commitment {
    int64_t id;
    char *contact_id;
    char *description;
    char *who;
    int64_t deadline;
    char *status;
    int64_t created_at;
    int64_t followed_up_at;
} hu_superhuman_commitment_t;

typedef struct hu_temporal_pattern {
    char *contact_id;
    int day_of_week;
    int hour;
    uint32_t message_count;
    int64_t avg_response_time_ms;
} hu_temporal_pattern_t;

typedef struct hu_delayed_followup {
    int64_t id;
    char *contact_id;
    char *topic;
    int64_t scheduled_at;
    bool sent;
} hu_delayed_followup_t;

typedef struct hu_important_date {
    char *date;      /* "07-15" */
    char *type;      /* "birthday", "holiday" */
    char *message;  /* "happy birthday min!" */
} hu_important_date_t;

hu_error_t hu_superhuman_inside_joke_store(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *context, size_t context_len,
    const char *punchline, size_t punchline_len);
hu_error_t hu_superhuman_inside_joke_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, size_t limit,
    hu_inside_joke_t **out, size_t *out_count);
hu_error_t hu_superhuman_inside_joke_reference(void *sqlite_ctx, int64_t id);
void hu_superhuman_inside_joke_free(hu_allocator_t *alloc, hu_inside_joke_t *arr, size_t count);

hu_error_t hu_superhuman_commitment_store(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *description, size_t desc_len,
    const char *who, size_t who_len, int64_t deadline);
hu_error_t hu_superhuman_commitment_list_due(void *sqlite_ctx, hu_allocator_t *alloc,
    int64_t now_ts, size_t limit, hu_superhuman_commitment_t **out, size_t *out_count);
hu_error_t hu_superhuman_commitment_mark_followed_up(void *sqlite_ctx, int64_t id);
void hu_superhuman_commitment_free(hu_allocator_t *alloc, hu_superhuman_commitment_t *arr, size_t count);

hu_error_t hu_superhuman_temporal_record(void *sqlite_ctx, const char *contact_id,
    size_t contact_id_len, int day_of_week, int hour, int64_t response_time_ms);
hu_error_t hu_superhuman_temporal_get_quiet_hours(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, int *out_day, int *out_hour_start, int *out_hour_end);

hu_error_t hu_superhuman_delayed_followup_schedule(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *topic, size_t topic_len,
    int64_t scheduled_at);
hu_error_t hu_superhuman_delayed_followup_list_due(void *sqlite_ctx, hu_allocator_t *alloc,
    int64_t now_ts, hu_delayed_followup_t **out, size_t *out_count);
void hu_superhuman_delayed_followup_free(hu_allocator_t *alloc, hu_delayed_followup_t *arr, size_t count);
hu_error_t hu_superhuman_delayed_followup_mark_sent(void *sqlite_ctx, int64_t id);

hu_error_t hu_superhuman_micro_moment_store(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *fact, size_t fact_len,
    const char *significance, size_t sig_len);
hu_error_t hu_superhuman_micro_moment_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, size_t limit, char **out_json, size_t *out_len);

hu_error_t hu_superhuman_avoidance_record(void *sqlite_ctx, const char *contact_id,
    size_t contact_id_len, const char *topic, size_t topic_len, bool topic_changed_quickly);
hu_error_t hu_superhuman_avoidance_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, char **out_json, size_t *out_len);

hu_error_t hu_superhuman_topic_baseline_record(void *sqlite_ctx, const char *contact_id,
    size_t contact_id_len, const char *topic, size_t topic_len);
hu_error_t hu_superhuman_topic_absence_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, int64_t now_ts, int64_t absence_days,
    char **out_json, size_t *out_len);

hu_error_t hu_superhuman_growth_store(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *topic, size_t topic_len,
    const char *before_state, size_t before_len, const char *after_state, size_t after_len);
hu_error_t hu_superhuman_growth_list_recent(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, size_t limit, char **out_json, size_t *out_len);

hu_error_t hu_superhuman_pattern_record(void *sqlite_ctx, const char *contact_id,
    size_t contact_id_len, const char *topic, size_t topic_len, const char *tone, size_t tone_len,
    int day_of_week, int hour);
hu_error_t hu_superhuman_pattern_list(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, size_t limit, char **out_json, size_t *out_len);

hu_error_t hu_superhuman_curiosity_topics(void *sqlite_ctx, hu_allocator_t *alloc,
    hu_memory_t *memory, const char *contact_id, size_t contact_id_len, size_t limit,
    char ***out_topics, size_t *out_count);
hu_error_t hu_superhuman_callback_topics(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, int64_t now_ts,
    char ***out_topics, size_t *out_count);

hu_error_t hu_superhuman_build_context(void *sqlite_ctx, hu_allocator_t *alloc,
    hu_memory_t *memory, const char *contact_id, size_t contact_id_len,
    const hu_contact_profile_t *contact, char **out, size_t *out_len);
hu_error_t hu_superhuman_extract_and_store(void *sqlite_ctx, hu_allocator_t *alloc,
    const char *contact_id, size_t contact_id_len, const char *user_msg, size_t user_len,
    const char *assistant_msg, size_t assistant_len, const char *history, size_t history_len);
#endif
```

2. **Implementation** — each function:
   - Takes `void *sqlite_ctx` (cast to `hu_sqlite_memory_t*` or `sqlite3*` depending on what we expose)
   - Use `hu_sqlite_memory_get_db` pattern: `sqlite3 *db = ...` from ctx
   - Prepare, bind, step, finalize; use `SQLITE_STATIC` (null) per AGENTS.md
   - Free all allocations in `*_free` helpers

3. **sqlite.c** — add accessor:

```c
sqlite3 *hu_sqlite_memory_get_db(hu_memory_t *mem);
```

Returns `self->db` when mem is sqlite-backed, else NULL.

**Tests:**

- `hu_superhuman_inside_joke_store` then `hu_superhuman_inside_joke_list` → 1 result
- `hu_superhuman_commitment_store` with deadline in past → `list_due` returns it
- `hu_superhuman_temporal_record` then `get_quiet_hours` (mock data)
- `hu_superhuman_delayed_followup_schedule` then `list_due` then `mark_sent`

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Task 3: F19 — Inside joke memory

**Description:** Store and retrieve inside jokes. Extraction: LLM or heuristic detects shared humor (e.g. callback phrase, meme reference). Storage: `inside_jokes` table. Retrieval: inject into prompt for conversation. Proactive: occasionally surface as callback opportunity.

**Files:**

- Modify: `src/memory/superhuman.c` (already has store/list)
- Modify: `src/agent/agent_turn.c` — inject inside jokes into prompt
- Modify: `src/agent/proactive.c` — add `HU_PROACTIVE_INSIDE_JOKE` action type
- Modify: `src/daemon.c` — query inside jokes in proactive cycle

**Steps:**

1. **Extraction trigger:** When processing a message, if `hu_conversation_classify_humor_callback` (new) or LLM extraction detects "inside joke" — e.g. "that's so [X] energy", "remember when...", shared meme reference — call `hu_superhuman_inside_joke_store`. For Phase 3, use simple heuristic: if message contains "remember when" or "that [X] energy" or similar, and we have context from history, store. Alternative: add to event extractor or a lightweight `hu_superhuman_extract_from_turn` that returns suggested stores (inside_joke, commitment, etc.) — caller decides. **Minimal path:** Add `hu_superhuman_inside_joke_suggest_store(const char *msg, size_t len, const char *history, size_t hist_len)` returning bool; when true, caller stores with `context=last_exchange`, `punchline=msg` or extracted phrase.

2. **Storage:** `hu_superhuman_inside_joke_store(ctx, alloc, contact_id, len, context, context_len, punchline, punchline_len)`. `created_at = time(NULL)`.

3. **Retrieval for prompt:** Before LLM call, `hu_superhuman_inside_joke_list(ctx, alloc, contact_id, len, 5, &jokes, &count)`. Build string: `"Inside jokes with this contact: [list]. Use naturally when relevant."` Inject under `### Memory` or `### Proactive Awareness`.

4. **Proactive:** In proactive cycle, 10–15% chance: `hu_superhuman_inside_joke_list`, pick one at random, add `HU_PROACTIVE_INSIDE_JOKE` action: `"CALLBACK: Reference this inside joke naturally: [context/punchline]"`.

5. **Reference tracking:** When we use an inside joke in a response, call `hu_superhuman_inside_joke_reference(ctx, id)` to bump `last_referenced` and `reference_count`. (Add this function.)

**Tests:**

- Store joke, list returns it
- Proactive with 1 joke → action contains joke
- Reference updates `last_referenced`

**Validation:**

- Manual: send "remember when you sent me that meme about X" → stored; next proactive or reply can reference

---

## Task 4: F20 — Commitment keeper (deadline + follow-up)

**Description:** Track commitments with deadlines. Extract from conversations (reuse `hu_commitment_detect` + parse deadline). Store in `commitments` table. Proactive: after deadline, one follow-up ("hey did you ever call the dentist?").

**Files:**

- Modify: `src/agent/commitment.h` / `commitment.c` — add `deadline` to `hu_commitment_t` if not present
- Modify: `src/memory/superhuman.c` — `hu_superhuman_commitment_store` with deadline
- Modify: `src/agent/agent_turn.c` — after `hu_commitment_detect`, store via superhuman when deadline parseable
- Modify: `src/agent/proactive.c` — query `hu_superhuman_commitment_list_due`, add `HU_PROACTIVE_COMMITMENT_FOLLOW_UP` (already exists; wire to superhuman table)
- Modify: `src/daemon.c` — pass superhuman commitments to proactive

**Steps:**

1. **Extraction:** `hu_commitment_detect` returns commitments. Extend or add `hu_commitment_parse_deadline(const char *statement, size_t len, int64_t *out_ts)` — heuristics for "tomorrow", "next week", "March 15", "in 2 days". Return 0 if no deadline.

2. **Storage:** When saving commitment, if deadline parsed: `hu_superhuman_commitment_store(ctx, alloc, contact_id, len, description, desc_len, "contact", 7, deadline)`. Also keep existing `hu_commitment_store_save` for backward compat (memory-backed).

3. **List due:** `hu_superhuman_commitment_list_due(ctx, alloc, time(NULL), 5, &out, &count)` — `WHERE contact_id=? AND status='pending' AND deadline IS NOT NULL AND deadline <= ?`.

4. **Proactive:** In daemon proactive cycle, call `hu_superhuman_commitment_list_due`. For each: add `HU_PROACTIVE_COMMITMENT_FOLLOW_UP` action. After sending follow-up, call `hu_superhuman_commitment_mark_followed_up(ctx, id)` to set `followed_up_at` and `status='followed_up'`.

5. **One follow-up only:** `status='followed_up'` excludes from future `list_due`.

**Tests:**

- Store commitment with deadline in past → list_due returns it
- After mark_followed_up → list_due excludes it
- Deadline "tomorrow" parses to correct timestamp

**Validation:**

- "I'll call the dentist tomorrow" → stored with deadline; next day proactive includes follow-up

---

## Task 5: F21 — Avoidance pattern detection

**Description:** Notice when someone steers away from topics. Track: topic mentioned → immediate topic change. Store in `avoidance_patterns`. Don't push — note in memory for context. Optionally surface gently for high-trust contacts.

**Files:**

- Modify: `src/memory/superhuman.c` — avoidance_record, avoidance_list
- Modify: `src/context/conversation.c` — add `hu_conversation_detect_topic_change`
- Modify: `src/agent/agent_turn.c` — after turn, if topic change detected within 1–2 messages, call avoidance_record
- Modify: `src/agent/agent_turn.c` — inject avoidance list into prompt (gentle context)

**Steps:**

1. **Detection:** `hu_conversation_detect_topic_change(history, count, &topic_before, &topic_after)` — simple: extract nouns/keywords from last 2 user messages; if different and change was rapid (same session), return true with topic_before. Topic = first few significant words.

2. **Storage:** `hu_superhuman_avoidance_record(ctx, contact_id, topic_before, topic_len, true)` — upsert, increment `change_count` when topic_changed_quickly.

3. **Retrieval:** `hu_superhuman_avoidance_list` → JSON or formatted string: `"Topics they may avoid: [list]. Don't push; use for context."`

4. **Prompt injection:** For contacts with `relationship_stage` friend+, inject avoidance context. Use `hu_persona_find_contact` to check stage.

**Tests:**

- Record 3 avoidance events for same topic → change_count >= 3
- List returns formatted output

**Validation:**

- Manual: rapid topic changes → stored; prompt includes "may avoid" note

---

## Task 6: F22 — Pattern mirror

**Description:** Surface recurring behavioral patterns ("You always seem more stressed on Sundays"). Track topic → emotional_tone over time. Surface only for friend+.

**Files:**

- Modify: `src/memory/superhuman.c` — add `hu_superhuman_pattern_record`, `hu_superhuman_pattern_list`
- Modify: `src/context/conversation.c` — add `hu_conversation_classify_emotional_tone`
- Modify: `src/agent/agent_turn.c` — record pattern after each message; inject pattern list into prompt
- Modify: `src/agent/proactive.c` — add `HU_PROACTIVE_PATTERN_INSIGHT` (exists; wire to superhuman pattern list)

**Steps:**

1. **Schema:** Add table `behavioral_patterns(contact_id, topic, tone, day_of_week, hour, count)` or reuse `temporal_patterns` with a `tone` column. Simpler: add `pattern_observations(contact_id, topic, tone, observed_at)`.

```sql
CREATE TABLE IF NOT EXISTS pattern_observations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contact_id TEXT NOT NULL,
    topic TEXT NOT NULL,
    tone TEXT NOT NULL,
    day_of_week INTEGER,
    hour INTEGER,
    observed_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_pattern_observations_contact ON pattern_observations(contact_id);
```

2. **Record:** After each user message, `hu_conversation_classify_emotional_tone(msg, len)` → "stressed", "excited", "neutral", etc. Extract topic (keyword). `hu_superhuman_pattern_record(ctx, contact_id, topic, tone, day_of_week, hour)`.

3. **Aggregate:** `hu_superhuman_pattern_list(ctx, alloc, contact_id, limit)` — GROUP BY topic, tone, day_of_week; return patterns with count >= 3. Format: "When discussing [topic] on [day], they often seem [tone]."

4. **Proactive:** Use in `HU_PROACTIVE_PATTERN_INSIGHT` — "PATTERN INSIGHT: [pattern]. Surface naturally."

**Tests:**

- Record 5 observations (topic=X, tone=stressed, day=0) → list returns pattern
- Count threshold 3 → fewer than 3 not returned

**Validation:**

- Manual: multiple stressed-Sunday messages → pattern surfaces in proactive

---

## Task 7: F23 — Topic absence detection

**Description:** Notice when someone hasn't mentioned a usual topic for >2 weeks. Track baselines in `topic_baselines`. Proactive: "hey you haven't mentioned work in a while, everything good?"

**Files:**

- Modify: `src/memory/superhuman.c` — topic_baseline_record, topic_absence_list
- Modify: `src/agent/agent_turn.c` — record topic mentions; inject absence list
- Modify: `src/agent/proactive.c` — add `HU_PROACTIVE_TOPIC_ABSENCE`

**Steps:**

1. **Baseline:** On each message, extract topics (keywords, or LLM). `hu_superhuman_topic_baseline_record(ctx, contact_id, topic)` — upsert, increment mention_count, set last_mentioned=now.

2. **Absence:** `hu_superhuman_topic_absence_list(ctx, alloc, contact_id, now_ts, 14)` — topics where mention_count >= 3 (frequent) AND last_mentioned < now - 14 days.

3. **Proactive:** Query absence list. If non-empty, 20% chance add `HU_PROACTIVE_TOPIC_ABSENCE`: "They haven't mentioned [topic] in 2+ weeks. Gentle check-in if appropriate."

4. **Prompt:** Inject absence list for context: "Topics they usually mention but haven't recently: [list]."

**Tests:**

- Record 5 mentions of "work", then advance time (or mock last_mentioned) → absence list includes "work"
- mention_count < 3 → not in absence list

**Validation:**

- Manual: frequent work mentions, then 3 weeks silence → proactive suggests check-in

---

## Task 8: F24 — Growth celebration

**Description:** Notice and celebrate progress. Compare current statements to past. "You were really worried about that presentation last month — sounds like you crushed it!"

**Files:**

- Modify: `src/memory/superhuman.c` — growth_store, growth_list_recent
- Modify: `src/agent/agent_turn.c` — detect progress (compare to past statements on same topic); store growth milestone; inject into prompt

**Steps:**

1. **Detection:** When user shares positive outcome ("it went great!", "I got the job"), check memory for past statements about same topic (e.g. "presentation", "interview"). If found with negative/worried tone → growth opportunity.

2. **Storage:** `hu_superhuman_growth_store(ctx, alloc, contact_id, topic, before_state, after_state)`.

3. **Retrieval:** `hu_superhuman_growth_list_recent` — last 30 days. Inject: "Recent growth to celebrate: [list]. Reference naturally when relevant."

4. **Proactive:** In proactive cycle, 15% chance: if growth milestones exist, add `HU_PROACTIVE_GROWTH_CELEBRATION`: "Celebrate their progress: [milestone]."

**Tests:**

- Store growth, list returns it
- Proactive includes growth action when milestones exist

**Validation:**

- Manual: "I was so worried about X" → later "X went great!" → growth stored; proactive can celebrate

---

## Task 9: F18 — Micro-moment recognition

**Description:** Catch small but significant details. "Wait, you said your dog's name is the same as your grandma's — is there a story there?" Store facts with `micro_moment` flag. Inject into prompt.

**Files:**

- Modify: `src/memory/superhuman.c` — micro_moment_store, micro_moment_list
- Modify: `src/agent/agent_turn.c` — extract micro-moments (LLM or heuristic); store; inject

**Steps:**

1. **Extraction:** Add to consolidation/summarization or post-turn: `hu_superhuman_extract_micro_moments(alloc, msg, history, &facts, &count)` — returns array of `{fact, significance}`. Use LLM: "From this conversation, extract 0–3 small but personally significant details that most people would miss. JSON array." Or heuristic: named entities (people, pets, places) + unusual connections ("same name as", "anniversary", "first time").

2. **Storage:** For each: `hu_superhuman_micro_moment_store(ctx, alloc, contact_id, fact, significance)`.

3. **Retrieval:** `hu_superhuman_micro_moment_list(ctx, alloc, contact_id, 10, &json, &len)` — return as formatted string for prompt.

4. **Prompt:** "Notable small details about this contact: [list]. Reference naturally when relevant — e.g. ask about the story behind a detail."

**Tests:**

- Store 2 micro-moments, list returns both
- Prompt injection includes them

**Validation:**

- Manual: "my dog's name is the same as my grandma's" → stored; next conversation can ask "is there a story there?"

---

## Task 10: F26 — Temporal pattern learning

**Description:** Learn when someone is chatty vs quiet. Track message frequency by day/hour. Use for proactive timing and interpreting silence.

**Files:**

- Modify: `src/memory/superhuman.c` — temporal_record, temporal_get_quiet_hours
- Modify: `src/daemon.c` — record temporal pattern on each message; before proactive, check quiet hours
- Modify: `src/agent/proactive.c` — skip or reduce proactive during quiet hours

**Steps:**

1. **Record:** On each incoming message: `hu_superhuman_temporal_record(ctx, contact_id, day_of_week, hour, response_time_ms)`. Upsert: increment message_count, update avg_response_time_ms.

2. **Quiet hours:** `hu_superhuman_temporal_get_quiet_hours(ctx, alloc, contact_id, &day, &hour_start, &hour_end)` — find (day, hour) with lowest message_count over last 4 weeks. Return that window.

3. **Proactive:** Before sending proactive to contact, call `get_quiet_hours`. If current (day, hour) is in quiet window, reduce probability (e.g. 50% skip) or delay.

4. **Silence interpretation:** When they're quiet, inject: "They're often quiet on [day] mornings. Don't worry if no reply."

**Tests:**

- Record 10 messages on Mon 9am, 2 on Sun 6am → quiet hours includes Sun 6am
- Proactive during quiet hour → reduced probability (mock)

**Validation:**

- Manual: send messages at consistent times → temporal patterns learned; proactive avoids quiet times

---

## Task 11: F30 — Spontaneous curiosity

**Description:** Random genuine questions from memory. "random question — do you still play the piano?" Triggered by memory surfacing interesting past topics. 10–15% per proactive cycle.

**Files:**

- Modify: `src/agent/proactive.c` — add `hu_proactive_check_curiosity`
- Modify: `src/daemon.c` — call curiosity check in proactive cycle
- Modify: `src/memory/superhuman.c` — add `hu_superhuman_curiosity_topics` (query memory for old topics)

**Steps:**

1. **Topic source:** `hu_superhuman_curiosity_topics(ctx, alloc, contact_id, limit)` — recall from memory with query "past interests hobbies activities" or similar; filter to topics not mentioned in last 14 days. Return 3–5 topics.

2. **Proactive:** `hu_proactive_check_curiosity(alloc, memory, contact_id, seed, &action)` — 10–15% chance; if yes, pick random topic, add action: "SPONTANEOUS CURIOSITY: Ask a genuine question about [topic]. E.g. 'random question — do you still [X]?'"

3. **Daemon:** In proactive cycle, call `hu_proactive_check_curiosity` before or after other checks. Merge into proactive result.

**Tests:**

- Mock memory with "piano" in old entries → curiosity returns piano
- Seed controls probability (deterministic test)

**Validation:**

- Manual: after 2 weeks, proactive includes "do you still play piano?" style message

---

## Task 12: F31 — Callback opportunities

**Description:** Reference previous conversations naturally. "how did that dinner thing go?" Memory scan for unresolved topics or mentioned future events. 25–35% per conversation start.

**Files:**

- Modify: `src/agent/proactive.c` — extend `hu_proactive_build_starter` or add `hu_proactive_check_callbacks`
- Modify: `src/memory/superhuman.c` — add `hu_superhuman_callback_topics` (query delayed_followups, commitments, events from last 2 weeks)

**Steps:**

1. **Topic source:** `hu_superhuman_callback_topics(ctx, alloc, contact_id, now_ts, &topics, &count)` — query: delayed_followups with topic, commitments with recent created_at, memory recall "upcoming event plan dinner meeting" from last 14 days. Return list of callback strings.

2. **Probability:** 25–35% when starting a conversation (conversation start = first message in new thread or after long gap). Use seed.

3. **Prompt:** When callback triggered, inject: "CALLBACK OPPORTUNITY: Consider asking about: [topics]. Only if natural."

4. **Integration:** In `hu_proactive_build_starter` or agent_turn when building starter, call `hu_superhuman_callback_topics`. Merge with existing starter logic.

**Tests:**

- Store delayed_followup "dinner" scheduled 2 days ago → callback_topics includes it
- Probability 30% → ~30% of runs have callback (statistical test or seed-based)

**Validation:**

- Manual: "we should get dinner next week" → later conversation start includes "how did that dinner go?" possibility

---

## Task 13: F50 — Calendar/schedule awareness

**Description:** Query macOS Calendar for context. "can't talk, in meetings all day" / "how was the dentist?" Opt-in via persona `context_awareness.calendar_enabled`.

**Files:**

- Create: `scripts/calendar_query.applescript`
- Create: `src/platform/calendar_macos.c` (or `src/context/calendar.c`)
- Modify: `include/human/persona.h` — add `context_awareness` to persona
- Modify: `src/persona/persona.c` — parse `context_awareness.calendar_enabled`
- Modify: `src/daemon.c` — when calendar_enabled, run calendar query; inject events into proactive prompt

**Steps:**

1. **AppleScript:** Create `scripts/calendar_query.applescript`:

```applescript
-- Usage: osascript calendar_query.applescript [hours_ahead]
-- Returns: JSON array of events
on run argv
    set hoursAhead to 24
    if (count of argv) > 0 then
        set hoursAhead to item 1 of argv as integer
    end if
    tell application "Calendar"
        set now to current date
        set endDate to now + (hoursAhead * hours)
        set eventList to {}
        repeat with cal in calendars
            set calEvents to (every event of cal whose start date ≥ now and start date ≤ endDate)
            repeat with ev in calEvents
                set end of eventList to {summary:(summary of ev), start:(start date of ev)}
            end repeat
        end repeat
        return eventList
    end tell
end run
```

2. **C wrapper:** `hu_calendar_macos_get_events(alloc, hours_ahead, &events_json, &len)` — run `osascript` with script path, parse stdout. Use `HU_IS_TEST` guard: in tests, return empty or mock.

3. **Persona:** Add to `hu_persona_t` or contact: `bool calendar_enabled`. Parse from `context_awareness.calendar_enabled`.

4. **Proactive:** When building proactive prompt for a contact, if calendar_enabled, call `hu_calendar_macos_get_events`. Inject: "Your calendar today: [events]. Use for context (e.g. 'in meetings', 'had dentist appointment')."

5. **Privacy:** Only when explicitly enabled. Document in persona schema.

**Tests:**

- HU_IS_TEST: `hu_calendar_macos_get_events` returns empty or mock
- Persona parse: `calendar_enabled: true` → field set

**Validation:**

- Manual: enable calendar, run daemon; proactive prompt includes today's events

---

## Task 14: F53 — Birthday/holiday awareness

**Description:** Occasion-specific messages. "happy birthday min!" + confetti effect. Storage: `important_dates` in persona JSON. Daily proactive check.

**Files:**

- Modify: `include/human/persona.h` — add `hu_important_date_t`, `hu_context_awareness_t`
- Modify: `src/persona/persona.c` — parse `important_dates`, `context_awareness`
- Modify: `src/agent/proactive.c` — add `hu_proactive_check_important_dates`
- Modify: `src/daemon.c` — call in proactive cycle
- Modify: `src/channels/imessage.c` — when sending birthday message, add effect (F42)

**Steps:**

1. **Persona struct:**

```c
typedef struct hu_important_date {
    char *date;      /* "07-15" MM-DD */
    char *type;      /* "birthday", "holiday", "anniversary" */
    char *message;   /* "happy birthday min!" */
} hu_important_date_t;

typedef struct hu_context_awareness {
    bool calendar_enabled;
} hu_context_awareness_t;
```

Add to `hu_contact_profile_t` or `hu_persona_t`:

- `hu_important_date_t *important_dates; size_t important_dates_count;`
- `hu_context_awareness_t context_awareness;`

2. **Parse:** In `hu_persona_load_json`, parse:

```json
"important_dates": [
  { "date": "07-15", "type": "birthday", "message": "happy birthday min!" }
],
"context_awareness": { "calendar_enabled": false }
```

3. **Check:** `hu_proactive_check_important_dates(alloc, persona, contact_id, now_tm, &action)` — iterate important_dates, compare `date` to `tm_mon+1` and `tm_mday`. If match, add `HU_PROACTIVE_IMPORTANT_DATE` action with message and type (for effect selection).

4. **Daemon:** In proactive cycle, before other checks, call `hu_proactive_check_important_dates`. If match, add to result with high priority (0.95).

5. **Effect:** When sending, if type=birthday and contact has `message_effects.enabled`, pass effect=confetti to channel send (F42).

**Tests:**

- Persona with important_dates, now = July 15 → action returned
- Persona with important_dates, now = July 16 → no action
- Parse empty important_dates → count 0

**Validation:**

- Manual: set birthday for today, run proactive → "happy birthday" + confetti

---

## Task 15: Persona JSON schema — important_dates, context_awareness

**Description:** Add parsing for `important_dates` and `context_awareness` in persona JSON. Wire to structs.

**Files:**

- Modify: `include/human/persona.h` — structs (Task 14)
- Modify: `src/persona/persona.c` — JSON parsing
- Modify: `src/persona/creator.c` — if it generates persona JSON
- Test: `tests/test_persona.c`

**Steps:**

1. Add `hu_important_date_t` and `hu_context_awareness_t` to persona.h.
2. Add fields to `hu_contact_profile_t` or `hu_persona_t` per design.
3. In `hu_persona_load_json`, parse `important_dates` array and `context_awareness` object.
4. Add `hu_important_date_deinit`, `hu_context_awareness_deinit` for cleanup.
5. Update `hu_persona_deinit` to free new fields.

**Tests:**

- Load persona JSON with important_dates → count correct, date/type/message populated
- Load with context_awareness.calendar_enabled=true → field true
- Load without these keys → defaults (empty, false)

**Validation:**

- `./build/human_tests` — 0 failures

---

## Task 16: Proactive action types — wire new types

**Description:** Add new `hu_proactive_action_type_t` values and wire them in `hu_proactive_build_context` and daemon.

**Files:**

- Modify: `include/human/agent/proactive.h` — add enum values
- Modify: `src/agent/proactive.c` — handle new types in build_context
- Modify: `src/daemon.c` — populate new actions from superhuman queries

**Steps:**

1. Add to enum:

```c
HU_PROACTIVE_INSIDE_JOKE,
HU_PROACTIVE_TOPIC_ABSENCE,
HU_PROACTIVE_GROWTH_CELEBRATION,
HU_PROACTIVE_IMPORTANT_DATE,
HU_PROACTIVE_CURIOSITY,
HU_PROACTIVE_CALLBACK,
```

2. In `hu_proactive_build_context`, all types produce the same format: "- {message}\n". No special handling.

3. In daemon proactive cycle, after existing checks:
   - Call `hu_superhuman_commitment_list_due`, `hu_superhuman_delayed_followup_list_due`
   - Call `hu_proactive_check_important_dates`
   - Call `hu_proactive_check_curiosity`
   - Call `hu_superhuman_inside_joke_list` (10% chance)
   - Call `hu_superhuman_callback_topics` (25% chance)
   - Call `hu_superhuman_topic_absence_list` (20% chance)
   - Call `hu_superhuman_growth_list_recent` (15% chance)
   - Merge all into `hu_proactive_result_t`, then `hu_proactive_build_context` and inject into prompt.

**Tests:**

- `hu_proactive_build_context` with each new type → output contains message
- Daemon with mock superhuman → actions populated

**Validation:**

- `./build/human_tests` — 0 failures

---

## Task 17: Agent turn — inject superhuman context

**Description:** Before LLM call in agent_turn, build and inject superhuman memory context: micro-moments, inside jokes, avoidance patterns, callbacks, growth celebration.

**Files:**

- Modify: `src/agent/agent_turn.c` — build superhuman context block
- Modify: `include/human/agent.h` — ensure agent has access to memory + sqlite ctx for superhuman

**Steps:**

1. **Context builder:** `hu_superhuman_build_context(ctx, alloc, contact_id, &out, &out_len)` — aggregates:
   - Micro-moments (if any)
   - Inside jokes (if any)
   - Avoidance patterns (if relationship friend+)
   - Topic absences (if any)
   - Growth milestones (if any)
   - Callback topics (if any)
     Format: "### Superhuman Memory\n\n[section for each non-empty]\n"

2. **Agent_turn:** When building system prompt, after proactive context, append `hu_superhuman_build_context` output if non-empty.

3. **Contact_id:** Use `agent->memory_session_id` as contact_id (session_id often maps to contact in iMessage).

**Tests:**

- Mock superhuman with all sections populated → context contains all
- Empty superhuman → context empty or minimal

**Validation:**

- Manual: with stored memories, prompt includes superhuman section

---

## Task 18: Extraction pipeline — post-turn storage

**Description:** After each agent turn, run extraction to populate superhuman tables: commitments (with deadlines), inside jokes, micro-moments, topic baselines, avoidance, patterns, growth.

**Files:**

- Modify: `src/agent/agent_turn.c` — post-turn extraction
- Create: `src/context/superhuman_extract.c` (optional) — extraction helpers
- Or: extend `hu_event_extract` for events; add `hu_superhuman_extract_turn` for the rest

**Steps:**

1. **Single entry point:** `hu_superhuman_extract_and_store(ctx, alloc, contact_id, user_msg, user_len, assistant_msg, assistant_len, history, history_len)` — internally:
   - Run commitment detect on user_msg
   - Parse deadlines, store in commitments table
   - Detect inside joke (heuristic or LLM)
   - Extract micro-moments (LLM or heuristic)
   - Extract topics, update topic_baselines
   - Detect topic change → avoidance_record
   - Classify tone → pattern_record
   - Detect growth (compare to past) → growth_store

2. **Agent_turn:** After sending response, call `hu_superhuman_extract_and_store` with user message, our response, and recent history.

3. **HU_IS_TEST:** In tests, extraction can be no-op or mock to avoid LLM calls.

**Tests:**

- `hu_superhuman_extract_and_store` with "I'll call the dentist tomorrow" → commitment stored with deadline
- With "remember when we laughed about X" → inside joke stored
- HU_IS_TEST path → no side effects

**Validation:**

- Manual: full conversation → tables populated

---

## Implementation Order

Recommended sequence (dependencies first):

1. **Task 1** — Schema (unblocks all)
2. **Task 2** — Superhuman module (unblocks Tasks 3–12)
3. **Task 15** — Persona schema (unblocks Task 14)
4. **Task 3** (F19 inside jokes)
5. **Task 4** (F20 commitment keeper)
6. **Task 9** (F18 micro-moments)
7. **Task 5** (F21 avoidance)
8. **Task 6** (F22 pattern mirror)
9. **Task 7** (F23 topic absence)
10. **Task 8** (F24 growth celebration)
11. **Task 10** (F26 temporal)
12. **Task 11** (F30 curiosity)
13. **Task 12** (F31 callbacks)
14. **Task 13** (F50 calendar)
15. **Task 14** (F53 birthday/holiday)
16. **Task 16** — Proactive types
17. **Task 17** — Agent turn injection
18. **Task 18** — Extraction pipeline

---

## Validation Matrix

Before considering Phase 3 complete:

| Check       | Command / Action                                                                                     |
| ----------- | ---------------------------------------------------------------------------------------------------- |
| Build       | `cmake -B build -DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_SQLITE=ON && cmake --build build -j$(nproc)` |
| Tests       | `./build/human_tests` — 0 failures, 0 ASan errors                                                  |
| Inside joke | Store → list → proactive action                                                                      |
| Commitment  | Store with deadline → list_due → follow-up → mark_sent                                               |
| Temporal    | Record → get_quiet_hours                                                                             |
| Birthday    | Persona with date=today → proactive action                                                           |
| Calendar    | calendar_enabled + AppleScript → events in prompt                                                    |
| Extraction  | Full turn → tables populated                                                                         |

---

## Risk Notes

- **Calendar:** AppleScript requires Calendar.app and permissions. HU_IS_TEST must bypass.
- **LLM extraction:** Micro-moments and growth detection may need LLM. Fallback to heuristics for binary size.
- **SQLite:** New tables add ~20KB to schema. Monitor DB size.
- **Persona:** important_dates is per-contact in design; persona schema may be per-contact or top-level. Verify design doc.
