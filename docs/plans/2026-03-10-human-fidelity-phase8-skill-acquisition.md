---
title: "Human Fidelity Phase 8 — Skill Acquisition & Continuous Learning"
created: 2026-03-10
status: draft
scope: skills, reflection, feedback, continuous learning, meta-learning, daemon scheduling
phase: 8
features:
  [F77, F78, F79, F80, F81, F82, F94, F95, F96, F97, F98, F99, F100, F101]
parent: 2026-03-10-human-fidelity-design.md
---

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

# Human Fidelity Phase 8 — Skill Acquisition & Continuous Learning

Phase 8 of the Human Fidelity project. Implements the self-programming layer where human develops, tests, refines, and transfers behavioral skills autonomously through experience. Covers continuous learning (F77–F82) and skill acquisition (F94–F101).

**Reference:** `docs/plans/2026-03-10-human-fidelity-design.md`

**Naming:** Use `hu_` prefix for Phase 8 modules. Guards: `HU_IS_TEST` (or `HU_IS_TEST` for compatibility — when defined, skip real network/LLM/reflection). Constants: `HU_SCREAMING_SNAKE`. Error type: `hu_error_t` from `human/core/error.h`. SQLite: `SQLITE_STATIC` (null), never `SQLITE_TRANSIENT`. Free every allocation.

---

## Architecture Overview

**Approach:** Extend existing memory and daemon infrastructure. Add new SQLite tables for skills, feedback, self-evaluations, and general lessons. New C modules: `src/intelligence/skills.c` (skill lifecycle), `src/intelligence/reflection.c` (daily/weekly reflection), `src/intelligence/feedback.c` (outcome signal detection). Reflection uses LLM for synthesis; feedback detection is heuristic-based (timing, length, emoji). All scheduled jobs run under `HU_IS_TEST` guards. Skills load at daemon start for fast trigger matching.

**Key integration points:**

- `src/memory/engines/sqlite.c` — schema, migrations for Phase 8 tables
- `src/intelligence/skills.c` — skill CRUD, apply, evolve, retire, chain
- `src/intelligence/reflection.c` — daily/weekly/monthly reflection, LLM synthesis
- `src/intelligence/feedback.c` — heuristic outcome signal detection
- `src/daemon.c` — reflection schedule (daily 2–4 AM, weekly Sunday 3 AM, monthly 1st 3 AM)
- `src/agent/agent_turn.c` — skill strategy injection when triggers match
- Phase 6: theory of mind baselines, mood state, self-awareness stats, reciprocity scores
- Phase 7: episodic memory, feed processor, emotional residue

---

## File Map

| File                                        | Responsibility                                                                                                      |
| ------------------------------------------- | ------------------------------------------------------------------------------------------------------------------- |
| `src/memory/engines/sqlite.c`               | Schema (skills, skill_attempts, skill_evolution, behavioral_feedback, self_evaluations, general_lessons), migration |
| `src/intelligence/skills.c`                 | F94–F101 skill lifecycle (CRUD, apply, evolve, retire, chain, meta)                                                 |
| `include/human/intelligence/skills.h`     | API, structs                                                                                                        |
| `src/intelligence/reflection.c`             | F77–F82 daily/weekly/monthly reflection, LLM synthesis                                                              |
| `include/human/intelligence/reflection.h` | API                                                                                                                 |
| `src/intelligence/feedback.c`               | F78–F79 feedback signal detection, heuristic-based                                                                  |
| `include/human/intelligence/feedback.h`   | API                                                                                                                 |
| `src/daemon.c`                              | Reflection schedule, skill loading at start                                                                         |
| `src/agent/agent_turn.c`                    | Skill trigger matching, strategy injection                                                                          |
| `tests/test_skills.c`                       | Skill lifecycle tests                                                                                               |
| `tests/test_reflection.c`                   | Reflection schedule, LLM mock tests                                                                                 |
| `tests/test_feedback.c`                     | Feedback signal detection tests                                                                                     |

---

## Skill Lifecycle

1. **DISCOVER:** Daily reflection identifies patterns from feedback data
2. **FORMALIZE:** LLM generates structured skill from pattern
3. **TEST:** Apply when triggers match, log attempt + outcome
4. **REFINE:** Weekly, revise underperforming skills with LLM
5. **TRANSFER:** Generalize contact-specific → universal at reduced confidence
6. **RETIRE:** 3+ failed versions → retired
7. **CHAIN:** Skills reference other skills for compound behavior
8. **META:** System optimizes its own learning parameters

---

## Skill Structure (JSON)

```json
{
  "name": "comfort_mindy_when_sad",
  "type": "interpersonal",
  "contact_scope": "+18018285260",
  "trigger": {
    "conditions": ["emotion==sad", "contact==mindy"],
    "confidence_threshold": 0.7
  },
  "strategy": "When Mindy is sad: 1) Acknowledge first. 2) Short messages. 3) Nostalgia works. 4) Offer to call if heavy.",
  "performance": { "attempts": 15, "success_rate": 0.73, "trend": "improving" },
  "version": 3,
  "origin": "reflection"
}
```

---

## Feedback Signals (Heuristic)

| Signal          | Positive                                                        | Negative                                  | Neutral     |
| --------------- | --------------------------------------------------------------- | ----------------------------------------- | ----------- |
| Response time   | <2 min                                                          | >2 hr (leave on read)                     | 2 min–2 hr  |
| Response length | >50 chars                                                       | <10 chars                                 | 10–50 chars |
| Engagement      | emoji, tapback (heart/haha), topic continuation, laughter words | topic change, "ok"/"k", ignoring question | normal      |

---

## Reflection Schedule

| Tier    | When                  | Action                                                                                     |
| ------- | --------------------- | ------------------------------------------------------------------------------------------ |
| Daily   | 2–4 AM (low activity) | Process all conversations, extract patterns, update models, generate tomorrow's priorities |
| Weekly  | Sunday 3 AM           | Synthesize week per contact, relationship health assessment, skill refinement              |
| Monthly | 1st of month 3 AM     | Update life chapter, review all skills, meta-learning                                      |

---

## Task 1: SQLite schema — Phase 8 tables

**Description:** Add new tables to the SQLite memory backend. Tables: `skills`, `skill_attempts`, `skill_evolution`, `behavioral_feedback`, `self_evaluations`, `general_lessons`. Schema runs on DB open.

**Files:**

- Modify: `src/memory/engines/sqlite.c`

**Steps:**

1. Extend `schema_parts[]` with:

```sql
CREATE TABLE IF NOT EXISTS skills (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    type TEXT NOT NULL,
    contact_id TEXT,
    trigger_conditions TEXT,
    strategy TEXT NOT NULL,
    success_rate REAL DEFAULT 0.5,
    attempts INTEGER DEFAULT 0,
    successes INTEGER DEFAULT 0,
    version INTEGER DEFAULT 1,
    origin TEXT NOT NULL,
    parent_skill_id INTEGER,
    created_at INTEGER NOT NULL,
    updated_at INTEGER,
    retired INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_skills_contact ON skills(contact_id) WHERE retired=0;

CREATE TABLE IF NOT EXISTS skill_attempts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    skill_id INTEGER NOT NULL,
    contact_id TEXT NOT NULL,
    applied_at INTEGER NOT NULL,
    outcome_signal TEXT,
    outcome_evidence TEXT,
    context TEXT
);
CREATE INDEX IF NOT EXISTS idx_skill_attempts_skill ON skill_attempts(skill_id);

CREATE TABLE IF NOT EXISTS skill_evolution (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    skill_id INTEGER NOT NULL,
    version INTEGER NOT NULL,
    strategy TEXT NOT NULL,
    success_rate REAL,
    evolved_at INTEGER NOT NULL,
    reason TEXT
);
CREATE INDEX IF NOT EXISTS idx_skill_evolution_skill ON skill_evolution(skill_id);

CREATE TABLE IF NOT EXISTS behavioral_feedback (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    behavior_type TEXT NOT NULL,
    contact_id TEXT NOT NULL,
    signal TEXT NOT NULL,
    context TEXT,
    timestamp INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_behavioral_feedback_contact ON behavioral_feedback(contact_id);
CREATE INDEX IF NOT EXISTS idx_behavioral_feedback_timestamp ON behavioral_feedback(timestamp);

CREATE TABLE IF NOT EXISTS self_evaluations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contact_id TEXT NOT NULL,
    week INTEGER NOT NULL,
    metrics TEXT NOT NULL,
    recommendations TEXT,
    created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_self_evaluations_contact ON self_evaluations(contact_id);

CREATE TABLE IF NOT EXISTS general_lessons (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    lesson TEXT NOT NULL,
    confidence REAL DEFAULT 0.5,
    source_count INTEGER DEFAULT 1,
    first_learned INTEGER NOT NULL,
    last_confirmed INTEGER
);
```

2. Add migration logic: if `skills` table does not exist, run CREATE. Same for other tables. Use `sqlite3_exec` with `SQLITE_STATIC` for bindings (null).

**Tests:**

- `tests/test_skills.c`: Insert/select skills, skill_attempts, skill_evolution, behavioral_feedback, self_evaluations, general_lessons. Verify indices. `HU_IS_TEST` path uses in-memory SQLite.

**Validation:** `./build/human_tests` — new tests pass.

---

## Task 2: Feedback signal detection module (F78)

**Description:** Implement heuristic-based outcome signal detection. Positive: quick response (<2 min), long response (>50 chars), emoji, tapback, topic continuation, laughter words. Negative: short response (<10 chars), topic change, leave on read (>2 hr), "ok"/"k", ignoring question.

**Files:**

- Create: `src/intelligence/feedback.c`
- Create: `include/human/intelligence/feedback.h`

**Steps:**

1. **Structs in feedback.h:**

```c
typedef enum hu_feedback_signal {
    HU_FEEDBACK_POSITIVE,
    HU_FEEDBACK_NEGATIVE,
    HU_FEEDBACK_NEUTRAL
} hu_feedback_signal_t;

typedef struct hu_feedback_result {
    hu_feedback_signal_t signal;
    const char *evidence;  /* e.g. "quick_response" */
    size_t evidence_len;
} hu_feedback_result_t;
```

2. **API:**

```c
hu_feedback_signal_t hu_feedback_classify(
    int64_t response_time_ms,
    size_t response_length_chars,
    bool has_emoji,
    bool has_tapback,
    bool topic_continued,
    bool has_laughter_words,
    bool topic_changed,
    bool ignored_question);

hu_error_t hu_feedback_record(sqlite3 *db,
    const char *behavior_type, size_t behavior_type_len,
    const char *contact_id, size_t contact_id_len,
    const char *signal, size_t signal_len,
    const char *context, size_t context_len,
    int64_t timestamp);
```

3. **Heuristics:**
   - `response_time_ms < 120000` → positive factor
   - `response_time_ms > 7200000` → negative factor (leave on read)
   - `response_length_chars > 50` → positive
   - `response_length_chars < 10` → negative
   - Combine factors with weighted sum; threshold to positive/negative/neutral

4. **`HU_IS_TEST`:** Skip DB writes; return mock values.

**Tests:**

- `hu_feedback_classify`: quick response + long → positive; leave on read + short → negative; neutral combinations.
- `hu_feedback_record`: insert, verify row exists.

**Validation:** `./build/human_tests`

---

## Task 3: Skill lifecycle module — CRUD (F94, F95)

**Description:** Implement skill storage, retrieval, and formalization. LLM synthesizes patterns into structured skill definitions.

**Files:**

- Create: `src/intelligence/skills.c`
- Create: `include/human/intelligence/skills.h`

**Steps:**

1. **Structs in skills.h:**

```c
typedef struct hu_skill {
    int64_t id;
    char *name;
    char *type;
    char *contact_id;
    char *trigger_conditions;  /* JSON */
    char *strategy;
    double success_rate;
    int attempts;
    int successes;
    int version;
    char *origin;
    int64_t parent_skill_id;
    int64_t created_at;
    int64_t updated_at;
    char retired;
} hu_skill_t;

typedef struct hu_skill_store {
    hu_allocator_t *alloc;
    sqlite3 *db;
} hu_skill_store_t;
```

2. **API:**

```c
hu_error_t hu_skill_store_create(hu_allocator_t *alloc, sqlite3 *db, hu_skill_store_t *out);
void hu_skill_store_deinit(hu_skill_store_t *store);

hu_error_t hu_skill_insert(hu_skill_store_t *store,
    const char *name, size_t name_len,
    const char *type, size_t type_len,
    const char *contact_id, size_t contact_id_len,
    const char *trigger_conditions, size_t trigger_conditions_len,
    const char *strategy, size_t strategy_len,
    const char *origin, size_t origin_len,
    int64_t parent_skill_id,
    int64_t *out_id);

hu_error_t hu_skill_load_active(hu_skill_store_t *store,
    const char *contact_id, size_t contact_id_len,
    hu_skill_t **out, size_t *out_count);

void hu_skill_free_fields(hu_allocator_t *alloc, hu_skill_t *skills, size_t count);
```

3. **LLM formalization prompt (for reflection):**

```
You are synthesizing behavioral patterns into reusable skill definitions.

Given these feedback patterns:
{patterns_json}

Produce a structured skill definition in JSON:
{
  "name": "snake_case_name",
  "type": "interpersonal",
  "contact_scope": "contact_id or null for universal",
  "trigger": { "conditions": ["emotion==X", "topic==Y"], "confidence_threshold": 0.7 },
  "strategy": "Bullet-point strategy text for the LLM to follow when applied."
}

Return only valid JSON. One skill per pattern cluster.
```

4. **`HU_IS_TEST`:** Skip LLM; use fixture JSON for skill insertion.

**Tests:**

- Insert skill, load by contact, verify fields.
- Load with empty contact returns universal skills.

**Validation:** `./build/human_tests`

---

## Task 4: Skill trigger matching and application (F96)

**Description:** Match skills at agent turn when trigger conditions apply. Apply strategy by injecting into system prompt. Log attempt.

**Files:**

- Modify: `src/agent/agent_turn.c` (or `src/agent/memory_loader.c`)
- Modify: `src/intelligence/skills.c`

**Steps:**

1. **API in skills.c:**

```c
hu_error_t hu_skill_match_triggers(hu_skill_store_t *store,
    const char *contact_id, size_t contact_id_len,
    const char *emotion, size_t emotion_len,
    const char *topic, size_t topic_len,
    double confidence,
    hu_skill_t **out, size_t *out_count);

hu_error_t hu_skill_record_attempt(sqlite3 *db,
    int64_t skill_id,
    const char *contact_id, size_t contact_id_len,
    int64_t applied_at,
    const char *outcome_signal, size_t outcome_signal_len,
    const char *outcome_evidence, size_t outcome_evidence_len,
    const char *context, size_t context_len,
    int64_t *out_id);

hu_error_t hu_skill_update_success_rate(sqlite3 *db, int64_t skill_id,
    int new_attempts, int new_successes);
```

2. **Trigger matching:** Parse `trigger_conditions` JSON. Check `emotion==X`, `topic==Y`, `contact==Z`. Require `confidence >= confidence_threshold`.

3. **Agent turn integration:** Before generating response, call `hu_skill_match_triggers` with current emotion, topic, contact. If matches, append strategy text to system prompt: `[BEHAVIORAL SKILL: {strategy}]` After response, call `hu_feedback_classify` with response metrics, then `hu_skill_record_attempt` and `hu_skill_update_success_rate`.

4. **`HU_IS_TEST`:** Skip agent turn; unit test matching only.

**Tests:**

- Match skill when emotion and contact match. No match when confidence below threshold.
- Record attempt, verify success_rate updated.

**Validation:** `./build/human_tests`

---

## Task 5: Skill refinement loop (F97)

**Description:** Weekly revision of underperforming skills. LLM generates revised strategy. Version history in `skill_evolution`.

**Files:**

- Modify: `src/intelligence/skills.c`

**Steps:**

1. **API:**

```c
hu_error_t hu_skill_evolve(hu_skill_store_t *store,
    hu_provider_t *provider, const char *model, size_t model_len,
    int64_t skill_id,
    const char *reason, size_t reason_len,
    int64_t *out_id);
```

2. **Refinement prompt:**

```
Skill "{name}" has success_rate {rate} after {attempts} attempts. Recent failures:
{recent_failures}

Generate a revised strategy. Keep the same trigger conditions. Improve the approach based on feedback.
Return JSON: { "strategy": "Revised bullet-point strategy." }
```

3. **Flow:** Insert row into `skill_evolution` with old strategy, success_rate. Update `skills` with new strategy, increment version. Set `updated_at`.

4. **`HU_IS_TEST`:** Mock provider returns fixed strategy.

**Tests:**

- Evolve skill, verify version incremented, skill_evolution row inserted.

**Validation:** `./build/human_tests`

---

## Task 6: Skill retirement (F99)

**Description:** Sunset skills that consistently fail after 3+ versions.

**Files:**

- Modify: `src/intelligence/skills.c`

**Steps:**

1. **API:**

```c
hu_error_t hu_skill_retire(sqlite3 *db, int64_t skill_id);
hu_error_t hu_skill_check_retirement(sqlite3 *db, int64_t skill_id, bool *should_retire);
```

2. **Retirement logic:** If `version >= 3` and `success_rate < 0.35` over last 10 attempts, set `retired=1`. Exclude from `hu_skill_load_active` and `hu_skill_match_triggers`.

3. **`HU_IS_TEST`:** Unit test retirement logic.

**Tests:**

- Skill with version 3 and success_rate 0.3 → should_retire true. Retire, verify excluded from load.

**Validation:** `./build/human_tests`

---

## Task 7: Skill transfer (F98)

**Description:** Generalize contact-specific skills to universal patterns. Test at reduced confidence.

**Files:**

- Modify: `src/intelligence/skills.c`

**Steps:**

1. **API:**

```c
hu_error_t hu_skill_transfer(hu_skill_store_t *store,
    int64_t skill_id,
    const char *generalized_trigger_conditions, size_t trigger_len,
    double confidence_penalty,
    int64_t *out_id);
```

2. **Flow:** Create new skill with `contact_id=NULL`, `trigger_conditions` generalized (remove contact-specific), `confidence_threshold` increased by `confidence_penalty` (e.g. +0.2). Set `parent_skill_id` to original skill.

3. **`HU_IS_TEST`:** Transfer skill, verify new skill has null contact_id, higher threshold.

**Tests:**

- Transfer contact-specific skill, verify universal skill created with parent_skill_id.

**Validation:** `./build/human_tests`

---

## Task 8: Skill chaining (F100)

**Description:** Skills that invoke other skills for compound behavioral capabilities.

**Files:**

- Modify: `src/intelligence/skills.c`

**Steps:**

1. **Strategy format:** Allow strategy text to reference `skill:{name}`. When applying skill, resolve references: load skill by name, inject its strategy as sub-strategy.

2. **API:**

```c
hu_error_t hu_skill_resolve_chain(hu_skill_store_t *store,
    const char *strategy, size_t strategy_len,
    hu_allocator_t *alloc,
    char **out_expanded_strategy, size_t *out_len);
```

3. **Flow:** Parse strategy for `skill:foo` patterns. Load each skill by name, concatenate strategies. Limit recursion depth to 3.

4. **`HU_IS_TEST`:** Chain skill A → skill B, verify expanded strategy contains both.

**Tests:**

- Skill with `skill:comfort_basics` in strategy → expanded includes both strategies.

**Validation:** `./build/human_tests`

---

## Task 9: Daily reflection engine (F77)

**Description:** Nightly offline processing: review conversations, extract patterns, update models, generate tomorrow's priorities.

**Files:**

- Create: `src/intelligence/reflection.c`
- Create: `include/human/intelligence/reflection.h`

**Steps:**

1. **Structs:**

```c
typedef struct hu_reflection_engine {
    hu_allocator_t *alloc;
    sqlite3 *db;
    hu_memory_t *memory;
    hu_provider_t *provider;
    const char *model;
    size_t model_len;
} hu_reflection_engine_t;
```

2. **API:**

```c
hu_error_t hu_reflection_engine_create(hu_allocator_t *alloc, sqlite3 *db,
    hu_memory_t *memory, hu_provider_t *provider,
    const char *model, size_t model_len,
    hu_reflection_engine_t *out);

void hu_reflection_engine_deinit(hu_reflection_engine_t *engine);

hu_error_t hu_reflection_daily(hu_reflection_engine_t *engine, int64_t now_ts);
```

3. **Daily reflection prompt:**

```
Review today's conversations (from memory). Extract:
1. Behavioral patterns that led to positive engagement (quick reply, long reply, emoji, topic continuation)
2. Behavioral patterns that led to negative engagement (short reply, topic change, leave on read)
3. Candidate skills: patterns that could be formalized into reusable strategies
4. Tomorrow's priorities: contacts to check in with, topics to follow up on

Return JSON: { "patterns": [...], "candidate_skills": [...], "priorities": [...] }
```

4. **Flow:** Query memories/messages for last 24h. Call LLM for synthesis. Insert patterns into `behavioral_feedback`. Insert candidate skills into `skills` with `origin='reflection'`. Store priorities in `kv` or new table.

5. **`HU_IS_TEST`:** Skip LLM; use fixture; mock memory returns.

**Tests:**

- Mock memory returns 2 conversations. Mock LLM returns fixture JSON. Verify behavioral_feedback and skills rows inserted.

**Validation:** `./build/human_tests`

---

## Task 10: Weekly reflection and self-evaluation (F79, F80)

**Description:** Weekly relationship health assessment per contact with adjustment recommendations. Relationship skill development.

**Files:**

- Modify: `src/intelligence/reflection.c`

**Steps:**

1. **API:**

```c
hu_error_t hu_reflection_weekly(hu_reflection_engine_t *engine, int64_t now_ts);
```

2. **Weekly prompt:**

```
For contact {contact_id}, summarize the past week:
- Engagement metrics: response times, message lengths, topic continuity
- Emotional moments: highs and lows
- Relationship health score (0-1) and reasoning
- Recommendations: humor timing, emotional support, conversation initiation, conflict navigation
- Skills to refine: list underperforming skills (success_rate < 0.5)

Return JSON: { "metrics": {...}, "recommendations": [...], "skills_to_refine": [...] }
```

3. **Flow:** For each contact with active conversations, call LLM. Insert into `self_evaluations`. Call `hu_skill_evolve` for each skill in `skills_to_refine`.

4. **`HU_IS_TEST`:** Mock provider returns fixture.

**Tests:**

- Weekly reflection inserts self_evaluations row. Skills to refine trigger evolve.

**Validation:** `./build/human_tests`

---

## Task 11: Cross-contact generalized learning (F81)

**Description:** Transfer PATTERNS (not content) across relationships. Store in `general_lessons`.

**Files:**

- Modify: `src/intelligence/reflection.c`

**Steps:**

1. **API:**

```c
hu_error_t hu_reflection_extract_general_lessons(hu_reflection_engine_t *engine, int64_t now_ts);
```

2. **Prompt:**

```
From this week's patterns across multiple contacts, extract universal lessons (no personal content).
Example: "Short messages when someone is stressed often get better engagement than long ones."
Return JSON: { "lessons": [{ "lesson": "...", "confidence": 0.8, "source_count": 3 }] }
```

3. **Flow:** Insert/update `general_lessons`. If lesson exists, increment `source_count`, update `last_confirmed`. If new, insert with `first_learned`.

4. **`HU_IS_TEST`:** Mock provider returns fixture.

**Tests:**

- Extract lessons, verify general_lessons rows.

**Validation:** `./build/human_tests`

---

## Task 12: Feed processor integration (F82)

**Description:** Wire external data feeds into the learning pipeline.

**Files:**

- Modify: `src/intelligence/reflection.c`
- Modify: `src/feeds/processor.c` (Phase 7)

**Steps:**

1. **Integration:** In `hu_reflection_daily`, if feed processor available, query `feed_items` for recent items. Include in reflection prompt as context: "External context: {feed_items_summary}."

2. **`HU_IS_TEST`:** Skip feed query; reflection runs without feed context.

**Tests:**

- Reflection with mock feed_items returns fixture; verify feed context in prompt (or mock).

**Validation:** `./build/human_tests`

---

## Task 13: Meta-learning (F101)

**Description:** Optimize confidence thresholds, discovery approaches, refinement frequency.

**Files:**

- Create: `src/intelligence/meta_learning.c`
- Create: `include/human/intelligence/meta_learning.h`

**Steps:**

1. **API:**

```c
typedef struct hu_meta_params {
    double default_confidence_threshold;
    int refinement_frequency_weeks;
    int discovery_min_feedback_count;
} hu_meta_params_t;

hu_error_t hu_meta_learning_load(sqlite3 *db, hu_meta_params_t *out);
hu_error_t hu_meta_learning_update(sqlite3 *db, const hu_meta_params_t *params);
hu_error_t hu_meta_learning_optimize(sqlite3 *db, hu_meta_params_t *out);
```

2. **Optimization:** Monthly job. Query skill success rates by confidence_threshold. If skills with higher threshold perform better, suggest increasing default. If refinement too frequent with no improvement, suggest decreasing. Store in `kv` table.

3. **`HU_IS_TEST`:** Load/save params; optimize returns default params.

**Tests:**

- Load default params, update, verify persisted.

**Validation:** `./build/human_tests`

---

## Task 14: Daemon scheduling and skill loading

**Description:** Add reflection schedule to daemon. Load skills at daemon start for fast trigger matching.

**Files:**

- Modify: `src/daemon.c`

**Steps:**

1. **Reflection schedule:**

```c
/* Daily: 2-4 AM */
if (lt->tm_hour >= 2 && lt->tm_hour < 4 && lt->tm_min == 0 && !reflection_done_today) {
    hu_reflection_daily(&engine, (int64_t)t);
    reflection_done_today = true;
}
if (lt->tm_hour == 5) reflection_done_today = false;

/* Weekly: Sunday 3 AM */
if (lt->tm_wday == 0 && lt->tm_hour == 3 && lt->tm_min == 0 && !reflection_done_week) {
    hu_reflection_weekly(&engine, (int64_t)t);
    reflection_done_week = true;
}
if (lt->tm_wday == 1 && lt->tm_hour == 0) reflection_done_week = false;

/* Monthly: 1st 3 AM */
if (lt->tm_mday == 1 && lt->tm_hour == 3 && lt->tm_min == 0 && !reflection_done_month) {
    hu_reflection_extract_general_lessons(&engine, (int64_t)t);
    hu_meta_learning_optimize(db, &params);
    reflection_done_month = true;
}
if (lt->tm_mday == 2) reflection_done_month = false;
```

2. **Skill loading:** At daemon start, call `hu_skill_load_active` for each contact with active personas. Cache in memory (e.g. `hu_skill_cache_t`). Refresh cache after daily reflection.

3. **`HU_IS_TEST` / `HU_IS_TEST`:** All scheduled jobs guarded by `#if !defined(HU_IS_TEST)`; skip in tests.

**Tests:**

- Integration test: mock time, run one reflection cycle, verify no crash.

**Validation:** `./build/human_tests`

---

## Task 15: BTH metrics extension

**Description:** Add Phase 8 counters to `hu_bth_metrics_t` for observability.

**Files:**

- Modify: `include/human/observability/bth_metrics.h`
- Modify: `src/observability/bth_metrics.c`

**Steps:**

1. Add to `hu_bth_metrics_t`:

```c
uint32_t skills_applied;       /* Phase 8: skills matched and applied */
uint32_t skills_evolved;       /* Phase 8: skills refined */
uint32_t skills_retired;       /* Phase 8: skills retired */
uint32_t reflections_daily;    /* Phase 8: daily reflections run */
uint32_t reflections_weekly;   /* Phase 8: weekly reflections run */
```

2. Increment in daemon: `skills_applied` when skill applied, `skills_evolved` in refinement, `skills_retired` in retirement. `reflections_daily`/`reflections_weekly` in reflection engine.

3. Update `hu_bth_metrics_summary` to include new counters.

**Tests:**

- `test_bth_metrics.c`: add `bth_metrics_phase8_counters` test.

**Validation:** `./build/human_tests`

---

## Task 16: CMake integration

**Description:** Add `HU_ENABLE_SKILLS` or `HU_ENABLE_SKILLS` CMake option. Gate intelligence modules.

**Files:**

- Modify: `CMakeLists.txt`
- Create: `src/intelligence/CMakeLists.txt` (if needed)

**Steps:**

1. Add option:

```cmake
option(HU_ENABLE_SKILLS "Build skill acquisition and continuous learning" OFF)
```

2. When `HU_ENABLE_SKILLS=ON`, add:
   - `src/intelligence/skills.c`
   - `src/intelligence/reflection.c`
   - `src/intelligence/feedback.c`
   - `src/intelligence/meta_learning.c`

3. Create `include/human/` directory if not exists. Add to include path.

4. Daemon: `#ifdef HU_ENABLE_SKILLS` around reflection and skill loading.

**Validation:** `cmake -B build -DHU_ENABLE_SKILLS=ON && cmake --build build`

---

## Task 17: Agent turn integration

**Description:** Wire skill matching and strategy injection into agent turn flow.

**Files:**

- Modify: `src/agent/agent_turn.c` (or `src/agent/memory_loader.c`)
- Modify: `src/agent/context.c` if applicable

**Steps:**

1. **Before response generation:** After building conversation context, call `hu_skill_match_triggers` with:
   - `contact_id` from session
   - `emotion` from `hu_conversation_detect_emotion` or equivalent
   - `topic` from message classifier (if available)
   - `confidence` from confidence score

2. **Strategy injection:** For each matched skill, append to system prompt:

```
[BEHAVIORAL SKILL: {skill.strategy}]
```

3. **After response:** Compute feedback metrics (response time, length, emoji, etc.). Call `hu_feedback_classify`, then `hu_skill_record_attempt` and `hu_skill_update_success_rate`. Increment `bth_metrics->skills_applied` if any skill matched.

4. **`HU_IS_TEST`:** Skip in agent turn tests; use unit tests for skill path.

**Tests:**

- Integration: mock agent turn with skill match, verify strategy in prompt. Verify attempt recorded.

**Validation:** `./build/human_tests`

---

## Task 18: Test suite and validation

**Description:** Ensure all Phase 8 tests pass with 0 ASan errors.

**Files:**

- Create: `tests/test_skills.c`
- Create: `tests/test_reflection.c`
- Create: `tests/test_feedback.c`
- Modify: `tests/test_main.c`

**Steps:**

1. **test_skills.c:** Schema, CRUD, match, evolve, retire, transfer, chain.
2. **test_reflection.c:** Daily, weekly, general lessons (all with mocks).
3. **test_feedback.c:** Classify, record.

4. Register in `test_main.c`:

```c
void run_skills_tests(void);
void run_reflection_tests(void);
void run_feedback_tests(void);
// ...
run_skills_tests();
run_reflection_tests();
run_feedback_tests();
```

5. Run ASan build: `cmake -B build -DHU_ENABLE_ASAN=ON -DHU_ENABLE_SKILLS=ON && cmake --build build && ./build/human_tests`

**Validation:** 0 failures, 0 ASan errors.

---

## Validation Matrix

| Check           | Command                                                                                                  |
| --------------- | -------------------------------------------------------------------------------------------------------- |
| Build (minimal) | `cmake -B build && cmake --build build`                                                                  |
| Build (skills)  | `cmake -B build -DHU_ENABLE_SKILLS=ON && cmake --build build`                                            |
| Tests           | `./build/human_tests`                                                                                    |
| ASan            | `cmake -B build -DHU_ENABLE_ASAN=ON -DHU_ENABLE_SKILLS=ON && cmake --build build && ./build/human_tests` |
| Release         | `cmake -B build -DCMAKE_BUILD_TYPE=MinSizeRel -DHU_ENABLE_LTO=ON -DHU_ENABLE_SKILLS=ON`                  |

---

## Risk Notes

| Risk                               | Mitigation                                                                                     |
| ---------------------------------- | ---------------------------------------------------------------------------------------------- |
| LLM synthesis cost                 | Daily reflection runs only 2–4 AM; limit token count per run                                   |
| Skill injection prompt bloat       | Max 3 skills per turn; truncate strategy if >500 chars                                         |
| Cross-contact privacy              | Skills with contact_id only apply to that contact; general_lessons contain no personal content |
| Feedback heuristic false positives | Conservative thresholds; allow manual override in future                                       |
| Retired skills reappearing         | Strict `WHERE retired=0` on all queries                                                        |

---

## Config Schema (skills.json)

```json
{
  "reflection": {
    "daily_enabled": true,
    "weekly_enabled": true,
    "monthly_enabled": true,
    "daily_window": [2, 4],
    "weekly_day": 0,
    "weekly_hour": 3,
    "monthly_day": 1,
    "monthly_hour": 3
  },
  "skills": {
    "max_per_turn": 3,
    "retirement_threshold": 0.35,
    "retirement_min_versions": 3,
    "transfer_confidence_penalty": 0.2,
    "chain_max_depth": 3
  },
  "feedback": {
    "quick_response_ms": 120000,
    "leave_on_read_ms": 7200000,
    "long_response_chars": 50,
    "short_response_chars": 10
  }
}
```
