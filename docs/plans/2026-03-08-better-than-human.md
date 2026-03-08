# Better Than Human — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make seaclaw genuinely superhuman — perfect memory, pattern detection, proactive intelligence, adaptive persona, parallel reasoning, and commitment tracking — all in a single 511KB binary.

**Architecture:** Seven layers, each building on the last. Layer 1 (memory) is the foundation everything depends on. Each layer maps to seaclaw's existing vtable architecture: `sc_memory_t` for storage, `sc_tool_t` for actions, `sc_observer_t` for analysis, `sc_persona_t` for personality. New subsystems get their own headers in `include/seaclaw/` and implementations in `src/`.

**Tech Stack:** C11, SQLite (for persistent tiers), PCRE2-compatible regex (or simple pattern matching), pthreads (for parallel execution), existing `sc_arena_t`/`sc_allocator_t`/`sc_json_*` utilities.

---

## Layer 1: Temporal Memory Engine

The foundation. Without perfect recall, nothing else is superhuman. Four tiers of memory, from microseconds to permanent, with automatic capture and promotion.

### Task 1: STM Buffer — Data Structure

Short-term memory: an in-memory ring buffer holding the last N turns of the current session with extracted metadata.

**Files:**

- Create: `include/seaclaw/memory/stm.h`
- Create: `src/memory/stm.c`
- Test: `tests/test_stm.c`
- Modify: `tests/test_main.c` — add `run_stm_tests()`
- Modify: `CMakeLists.txt` — add both source files

**Step 1: Write the header**

```c
/* include/seaclaw/memory/stm.h */
#ifndef SC_STM_H
#define SC_STM_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/slice.h"

#define SC_STM_MAX_TURNS     20
#define SC_STM_MAX_ENTITIES  50
#define SC_STM_MAX_TOPICS    10
#define SC_STM_MAX_EMOTIONS   5

typedef enum sc_emotion_tag {
    SC_EMOTION_NEUTRAL,
    SC_EMOTION_JOY,
    SC_EMOTION_SADNESS,
    SC_EMOTION_ANGER,
    SC_EMOTION_FEAR,
    SC_EMOTION_SURPRISE,
    SC_EMOTION_FRUSTRATION,
    SC_EMOTION_EXCITEMENT,
    SC_EMOTION_ANXIETY,
} sc_emotion_tag_t;

typedef struct sc_stm_entity {
    char *name;            /* owned */
    size_t name_len;
    char *type;            /* "person", "place", "org", "date", "topic" — owned */
    size_t type_len;
    uint32_t mention_count;
    double importance;     /* 0.0–1.0, computed from frequency + recency + emotion */
} sc_stm_entity_t;

typedef struct sc_stm_emotion {
    sc_emotion_tag_t tag;
    double intensity;      /* 0.0–1.0 */
} sc_stm_emotion_t;

typedef struct sc_stm_turn {
    char *role;            /* "user" or "assistant" — owned */
    char *content;         /* owned */
    size_t content_len;
    sc_stm_entity_t entities[SC_STM_MAX_ENTITIES];
    size_t entity_count;
    sc_stm_emotion_t emotions[SC_STM_MAX_EMOTIONS];
    size_t emotion_count;
    char *primary_topic;   /* owned, NULL if none */
    uint64_t timestamp_ms;
} sc_stm_turn_t;

typedef struct sc_stm_buffer {
    sc_allocator_t alloc;
    sc_stm_turn_t turns[SC_STM_MAX_TURNS];
    size_t turn_count;       /* total turns added (wraps) */
    size_t head;             /* next write position */
    char *topics[SC_STM_MAX_TOPICS]; /* LRU topic history — owned */
    size_t topic_count;
    char *session_id;        /* owned */
    size_t session_id_len;
} sc_stm_buffer_t;

sc_error_t sc_stm_init(sc_stm_buffer_t *buf, sc_allocator_t alloc,
                        const char *session_id, size_t session_id_len);
sc_error_t sc_stm_record_turn(sc_stm_buffer_t *buf, const char *role, size_t role_len,
                               const char *content, size_t content_len, uint64_t timestamp_ms);
size_t sc_stm_count(const sc_stm_buffer_t *buf);
const sc_stm_turn_t *sc_stm_get(const sc_stm_buffer_t *buf, size_t idx);
sc_error_t sc_stm_build_context(const sc_stm_buffer_t *buf, sc_allocator_t *alloc,
                                 char **out, size_t *out_len);
void sc_stm_clear(sc_stm_buffer_t *buf);
void sc_stm_deinit(sc_stm_buffer_t *buf);

#endif
```

**Step 2: Write failing tests**

```c
/* tests/test_stm.c */
#include "test_framework.h"
#include "seaclaw/memory/stm.h"
#include "seaclaw/core/allocator.h"

static sc_allocator_t test_alloc(void) {
    return sc_allocator_libc();
}

static void stm_init_sets_session_id(void) {
    sc_stm_buffer_t buf;
    SC_ASSERT_EQ(sc_stm_init(&buf, test_alloc(), "sess-1", 6), SC_OK);
    SC_ASSERT_EQ(sc_stm_count(&buf), 0);
    SC_ASSERT_STR_EQ(buf.session_id, "sess-1");
    sc_stm_deinit(&buf);
}

static void stm_record_turn_stores_content(void) {
    sc_stm_buffer_t buf;
    sc_stm_init(&buf, test_alloc(), "sess-1", 6);
    SC_ASSERT_EQ(sc_stm_record_turn(&buf, "user", 4, "hello world", 11, 1000), SC_OK);
    SC_ASSERT_EQ(sc_stm_count(&buf), 1);
    const sc_stm_turn_t *t = sc_stm_get(&buf, 0);
    SC_ASSERT_NOT_NULL(t);
    SC_ASSERT_STR_EQ(t->content, "hello world");
    SC_ASSERT_STR_EQ(t->role, "user");
    sc_stm_deinit(&buf);
}

static void stm_wraps_at_max_turns(void) {
    sc_stm_buffer_t buf;
    sc_stm_init(&buf, test_alloc(), "sess-1", 6);
    for (size_t i = 0; i < SC_STM_MAX_TURNS + 5; i++) {
        char msg[32];
        int len = snprintf(msg, sizeof(msg), "turn-%zu", i);
        sc_stm_record_turn(&buf, "user", 4, msg, (size_t)len, 1000 + i);
    }
    SC_ASSERT_EQ(sc_stm_count(&buf), SC_STM_MAX_TURNS + 5);
    /* oldest should be turn-5 (first 5 were evicted) */
    const sc_stm_turn_t *oldest = sc_stm_get(&buf, 0);
    SC_ASSERT_NOT_NULL(oldest);
    SC_ASSERT_STR_EQ(oldest->content, "turn-5");
    sc_stm_deinit(&buf);
}

static void stm_build_context_formats_turns(void) {
    sc_stm_buffer_t buf;
    sc_allocator_t a = test_alloc();
    sc_stm_init(&buf, a, "sess-1", 6);
    sc_stm_record_turn(&buf, "user", 4, "What's the weather?", 19, 1000);
    sc_stm_record_turn(&buf, "assistant", 9, "It's sunny today.", 17, 2000);
    char *ctx = NULL;
    size_t ctx_len = 0;
    SC_ASSERT_EQ(sc_stm_build_context(&buf, &a, &ctx, &ctx_len), SC_OK);
    SC_ASSERT_NOT_NULL(ctx);
    SC_ASSERT_TRUE(ctx_len > 0);
    /* Should contain both turns */
    SC_ASSERT_TRUE(strstr(ctx, "What's the weather?") != NULL);
    SC_ASSERT_TRUE(strstr(ctx, "It's sunny today.") != NULL);
    a.free(a.ctx, ctx, ctx_len + 1);
    sc_stm_deinit(&buf);
}

static void stm_clear_resets_buffer(void) {
    sc_stm_buffer_t buf;
    sc_stm_init(&buf, test_alloc(), "sess-1", 6);
    sc_stm_record_turn(&buf, "user", 4, "hello", 5, 1000);
    SC_ASSERT_EQ(sc_stm_count(&buf), 1);
    sc_stm_clear(&buf);
    SC_ASSERT_EQ(sc_stm_count(&buf), 0);
    sc_stm_deinit(&buf);
}

void run_stm_tests(void) {
    SC_TEST_SUITE("stm_buffer");
    SC_RUN_TEST(stm_init_sets_session_id);
    SC_RUN_TEST(stm_record_turn_stores_content);
    SC_RUN_TEST(stm_wraps_at_max_turns);
    SC_RUN_TEST(stm_build_context_formats_turns);
    SC_RUN_TEST(stm_clear_resets_buffer);
}
```

**Step 3: Implement `src/memory/stm.c`**

Ring buffer with FIFO eviction. On `record_turn`: free oldest if full, copy content, advance head. `build_context` formats recent turns as markdown. `get(idx)` maps logical index to physical position in ring.

**Step 4:** Run: `cmake --build build && ./build/seaclaw_tests` — 5 new tests pass

**Step 5:** Commit: `feat(memory): add STM buffer for short-term session memory`

---

### Task 2: Fast-Capture Regex Patterns

Extract entities, emotions, topics, and dates from text in <1ms using pattern matching. No LLM needed.

**Files:**

- Create: `include/seaclaw/memory/fast_capture.h`
- Create: `src/memory/fast_capture.c`
- Test: `tests/test_fast_capture.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
/* include/seaclaw/memory/fast_capture.h */
#ifndef SC_FAST_CAPTURE_H
#define SC_FAST_CAPTURE_H

#include "seaclaw/memory/stm.h"

#define SC_FC_MAX_RESULTS 20

typedef struct sc_fc_entity_match {
    char *name;       /* owned */
    size_t name_len;
    char *type;       /* "person", "date", "topic", "emotion" — owned */
    size_t type_len;
    double confidence; /* 0.0–1.0 */
    size_t offset;     /* byte offset in source text */
} sc_fc_entity_match_t;

typedef struct sc_fc_result {
    sc_fc_entity_match_t entities[SC_FC_MAX_RESULTS];
    size_t entity_count;
    sc_stm_emotion_t emotions[SC_STM_MAX_EMOTIONS];
    size_t emotion_count;
    char *primary_topic;  /* owned, NULL if none */
    bool has_commitment;  /* "I will", "I'm going to", "I promise" detected */
    bool has_question;    /* ends with ? or contains question patterns */
} sc_fc_result_t;

sc_error_t sc_fast_capture(sc_allocator_t *alloc, const char *text, size_t text_len,
                            sc_fc_result_t *out);
void sc_fc_result_deinit(sc_fc_result_t *result, sc_allocator_t *alloc);

#endif
```

**Step 2: Write failing tests**

Tests for: relationship words detection ("my mom", "my friend"), emotion patterns ("I'm so frustrated", "I feel great"), topic classification ("at work", "the doctor"), commitment detection ("I'll do it", "I promise"), question detection. Each test provides a sentence and asserts the expected match.

**Step 3: Implement**

Pattern arrays: `RELATIONSHIP_PATTERNS[]` = `{"my mom", "my dad", "my wife", "my husband", "my friend", "my sister", "my brother", "my boss", "my coworker", ...}`, scanned with `strstr` or `sc_str_contains`. Emotion patterns: keyword → emotion + intensity. Topic patterns: keyword clusters → topic. Commitment: `"I will"`, `"I'm going to"`, `"I promise"`, `"remind me"`. All O(n) single pass.

**Step 4:** Tests pass

**Step 5:** Commit: `feat(memory): add fast-capture regex pattern extraction`

---

### Task 3: Fast-Capture Integration into Agent Turn

Wire fast-capture into the agent turn so every user message is analyzed and entities are recorded in STM.

**Files:**

- Modify: `src/agent/agent_turn.c` — after user message append, run fast-capture + STM record
- Modify: `include/seaclaw/agent.h` — add `sc_stm_buffer_t *stm` to `sc_agent_t`
- Modify: `src/agent/agent.c` — init/deinit STM buffer
- Test: `tests/test_agent.c` — verify STM populated after turn

**Step 1:** Add `sc_stm_buffer_t stm;` field to `sc_agent_t` in `include/seaclaw/agent.h`.

**Step 2:** In `sc_agent_init` (`src/agent/agent.c`), call `sc_stm_init(&agent->stm, ...)`.

**Step 3:** In `sc_agent_turn` (`src/agent/agent_turn.c`), after appending the user message to history (around line 176), add:

```c
sc_fc_result_t fc;
memset(&fc, 0, sizeof(fc));
if (sc_fast_capture(agent->alloc, msg, msg_len, &fc) == SC_OK) {
    sc_stm_turn_t *turn = /* current turn from stm */;
    /* Copy extracted entities and emotions into the STM turn */
    memcpy(turn->entities, fc.entities, sizeof(sc_stm_entity_t) * fc.entity_count);
    turn->entity_count = fc.entity_count;
    memcpy(turn->emotions, fc.emotions, sizeof(sc_stm_emotion_t) * fc.emotion_count);
    turn->emotion_count = fc.emotion_count;
    turn->primary_topic = fc.primary_topic;
    fc.primary_topic = NULL; /* transfer ownership */
}
sc_fc_result_deinit(&fc, agent->alloc);
```

**Step 4:** In `sc_agent_deinit`, call `sc_stm_deinit(&agent->stm)`.

**Step 5:** Test: verify that after `sc_agent_turn`, the STM buffer contains the turn with extracted entities.

**Step 6:** Commit: `feat(agent): integrate fast-capture + STM into agent turn loop`

---

### Task 4: STM Context Injection into System Prompt

Inject the STM buffer's context into the system prompt so the LLM has awareness of recent conversation signals.

**Files:**

- Modify: `src/agent/agent_turn.c` — build STM context, pass to prompt config
- Modify: `include/seaclaw/agent/prompt.h` — add `stm_context` field to `sc_prompt_config_t`
- Modify: `src/agent/prompt.c` — include STM context in system prompt

**Step 1:** Add `const char *stm_context; size_t stm_context_len;` to `sc_prompt_config_t`.

**Step 2:** In `sc_prompt_build_system`, after memory context, append STM context:

```c
if (config->stm_context && config->stm_context_len > 0) {
    sc_json_buf_append_str(buf, "\n\n### Session Context\n");
    sc_json_buf_append(buf, config->stm_context, config->stm_context_len);
}
```

**Step 3:** In `agent_turn.c`, before building system prompt, call `sc_stm_build_context` and set `prompt_config.stm_context`.

**Step 4:** Tests pass

**Step 5:** Commit: `feat(agent): inject STM session context into system prompt`

---

### Task 5: Deep Extraction Job (Async LLM)

After the agent turn completes, dispatch an async LLM call to extract structured facts, relationships, and insights from the conversation. Store results in memory (L2).

**Files:**

- Create: `include/seaclaw/memory/deep_extract.h`
- Create: `src/memory/deep_extract.c`
- Test: `tests/test_deep_extract.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef SC_DEEP_EXTRACT_H
#define SC_DEEP_EXTRACT_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/provider.h"
#include "seaclaw/memory.h"

#define SC_DE_MAX_FACTS      20
#define SC_DE_MAX_ENTITIES   10
#define SC_DE_MAX_RELATIONS  10

typedef struct sc_extracted_fact {
    char *subject;    /* owned */
    char *predicate;  /* owned */
    char *object;     /* owned */
    double confidence;
} sc_extracted_fact_t;

typedef struct sc_extracted_relation {
    char *entity_a;   /* owned */
    char *relation;   /* owned — "parent_of", "friend_of", "works_at", etc. */
    char *entity_b;   /* owned */
    double confidence;
} sc_extracted_relation_t;

typedef struct sc_deep_extract_result {
    sc_extracted_fact_t facts[SC_DE_MAX_FACTS];
    size_t fact_count;
    sc_extracted_relation_t relations[SC_DE_MAX_RELATIONS];
    size_t relation_count;
    char *summary;     /* owned — 1-2 sentence session summary */
    size_t summary_len;
} sc_deep_extract_result_t;

/* Builds the extraction prompt from conversation turns */
sc_error_t sc_deep_extract_build_prompt(sc_allocator_t *alloc,
                                         const char *conversation, size_t conversation_len,
                                         char **out, size_t *out_len);

/* Parses the LLM response JSON into structured results */
sc_error_t sc_deep_extract_parse(sc_allocator_t *alloc, const char *response, size_t response_len,
                                  sc_deep_extract_result_t *out);

/* Stores extracted results into memory backend */
sc_error_t sc_deep_extract_store(sc_allocator_t *alloc, sc_memory_t *memory,
                                  const char *session_id, size_t session_id_len,
                                  const sc_deep_extract_result_t *result);

void sc_deep_extract_result_deinit(sc_deep_extract_result_t *result, sc_allocator_t *alloc);

#endif
```

**Step 2:** Write tests: `deep_extract_build_prompt_includes_conversation`, `deep_extract_parse_extracts_facts`, `deep_extract_parse_extracts_relations`, `deep_extract_parse_handles_empty`.

**Step 3:** Implement. The prompt instructs the LLM to return JSON with `facts[]`, `relations[]`, and `summary`. The parser uses `sc_json_parse`. The store function calls `memory->vtable->store()` for each fact with category `SC_MEMORY_CATEGORY_CONVERSATION`.

**Step 4:** Tests pass

**Step 5:** Commit: `feat(memory): add deep extraction for structured fact/relation extraction`

---

### Task 6: Session Promotion (L1 → L2)

When a session ends, promote important entities and patterns from STM to persistent memory.

**Files:**

- Create: `include/seaclaw/memory/promotion.h`
- Create: `src/memory/promotion.c`
- Test: `tests/test_promotion.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef SC_MEMORY_PROMOTION_H
#define SC_MEMORY_PROMOTION_H

#include "seaclaw/memory/stm.h"
#include "seaclaw/memory.h"

typedef struct sc_promotion_config {
    uint32_t min_mention_count;   /* entities must be mentioned >= this many times */
    double min_importance;         /* minimum importance score (0.0–1.0) */
    uint32_t max_entities;         /* max entities to promote per session */
} sc_promotion_config_t;

#define SC_PROMOTION_DEFAULTS { .min_mention_count = 2, .min_importance = 0.3, .max_entities = 15 }

/* Calculate importance score for an entity */
double sc_promotion_entity_importance(const sc_stm_entity_t *entity,
                                       const sc_stm_buffer_t *buf);

/* Promote qualifying entities from STM to persistent memory */
sc_error_t sc_promotion_run(sc_allocator_t *alloc, const sc_stm_buffer_t *buf,
                             sc_memory_t *memory, const sc_promotion_config_t *config);

#endif
```

**Step 2:** Tests: `promotion_skips_low_importance`, `promotion_promotes_frequent_entities`, `promotion_respects_max_cap`.

**Step 3:** Implement. Sort entities by importance (mention count _ recency weight _ emotion boost). Promote top N above threshold. Store as memory entries with category CORE and key `entity:{name}`.

**Step 4:** Tests pass. **Step 5:** Commit: `feat(memory): add session promotion for STM entities to persistent memory`

---

### Task 7: Memory Consolidation Job

Periodically merge duplicate memories, decay old entries, and promote patterns.

**Files:**

- Create: `include/seaclaw/memory/consolidation.h`
- Create: `src/memory/consolidation.c`
- Test: `tests/test_consolidation.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef SC_MEMORY_CONSOLIDATION_H
#define SC_MEMORY_CONSOLIDATION_H

#include "seaclaw/memory.h"

typedef struct sc_consolidation_config {
    uint32_t decay_days;        /* memories older than this lose importance */
    double decay_factor;         /* multiplier per decay period (e.g. 0.9) */
    uint32_t dedup_threshold;    /* similarity score for merging (0–100) */
    uint32_t max_entries;        /* cap total entries, prune lowest importance */
} sc_consolidation_config_t;

#define SC_CONSOLIDATION_DEFAULTS { .decay_days = 30, .decay_factor = 0.9, \
                                     .dedup_threshold = 85, .max_entries = 10000 }

/* Run consolidation pass over the memory backend */
sc_error_t sc_memory_consolidate(sc_allocator_t *alloc, sc_memory_t *memory,
                                  const sc_consolidation_config_t *config);

/* Compute string similarity score (0–100) using token overlap */
uint32_t sc_similarity_score(const char *a, size_t a_len, const char *b, size_t b_len);

#endif
```

**Step 2:** Tests: `similarity_identical_strings_100`, `similarity_different_strings_low`, `consolidation_merges_duplicates`.

**Step 3:** Implement. `sc_similarity_score` uses token overlap (split by space, count shared / total). `sc_memory_consolidate` lists all entries, groups by key prefix, merges entries above threshold, removes oldest beyond max_entries.

**Step 4:** Tests pass. **Step 5:** Commit: `feat(memory): add memory consolidation with dedup and decay`

---

## Layer 2: Promise Ledger

### Task 8: Commitment Detection

Detect promises, intentions, and reminders from text using pattern matching.

**Files:**

- Create: `include/seaclaw/agent/commitment.h`
- Create: `src/agent/commitment.c`
- Test: `tests/test_commitment.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef SC_COMMITMENT_H
#define SC_COMMITMENT_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"

typedef enum sc_commitment_type {
    SC_COMMIT_PROMISE,     /* "I will", "I'll" */
    SC_COMMIT_INTENTION,   /* "I'm going to", "I plan to" */
    SC_COMMIT_REMINDER,    /* "remind me", "don't let me forget" */
    SC_COMMIT_GOAL,        /* "I want to", "my goal is" */
} sc_commitment_type_t;

typedef enum sc_commitment_status {
    SC_COMMIT_STATUS_ACTIVE,
    SC_COMMIT_STATUS_COMPLETED,
    SC_COMMIT_STATUS_EXPIRED,
    SC_COMMIT_STATUS_CANCELLED,
} sc_commitment_status_t;

typedef struct sc_commitment {
    char *id;               /* owned — UUID */
    char *statement;         /* owned — the original text */
    size_t statement_len;
    char *summary;           /* owned — extracted core commitment */
    size_t summary_len;
    sc_commitment_type_t type;
    sc_commitment_status_t status;
    char *target_date;       /* owned, NULL if no deadline — ISO 8601 */
    char *follow_up_after;   /* owned, NULL if no follow-up — ISO 8601 */
    char *created_at;        /* owned — ISO 8601 */
    double emotional_weight; /* 0.0–1.0 */
    char *owner;             /* "user" or "assistant" — owned */
} sc_commitment_t;

#define SC_COMMITMENT_MAX_DETECT 5

typedef struct sc_commitment_detect_result {
    sc_commitment_t commitments[SC_COMMITMENT_MAX_DETECT];
    size_t count;
} sc_commitment_detect_result_t;

/* Detect commitments in text */
sc_error_t sc_commitment_detect(sc_allocator_t *alloc, const char *text, size_t text_len,
                                 const char *role, size_t role_len,
                                 sc_commitment_detect_result_t *out);

/* Free a commitment's owned fields */
void sc_commitment_deinit(sc_commitment_t *c, sc_allocator_t *alloc);
void sc_commitment_detect_result_deinit(sc_commitment_detect_result_t *r, sc_allocator_t *alloc);

#endif
```

**Step 2:** Tests: `detect_promise_i_will`, `detect_intention_going_to`, `detect_reminder_remind_me`, `detect_goal_i_want_to`, `detect_no_commitment_in_casual`, `detect_ignores_negation` ("I will not" should not match).

**Step 3:** Implement. Pattern arrays: `PROMISE_PATTERNS[] = {"I will ", "I'll ", "I promise "}`, `INTENTION_PATTERNS[] = {"I'm going to ", "I am going to ", "I plan to "}`, etc. Scan text for each pattern. Extract the clause following the pattern (up to period/comma/newline). Set owner based on role. Generate UUID for id. Set `created_at` to current ISO 8601. Negation filter: skip if preceded by "not", "n't", "never".

**Step 4:** Tests pass. **Step 5:** Commit: `feat(agent): add commitment detection from conversation text`

---

### Task 9: Commitment Storage (SQLite)

Store and query commitments in the SQLite memory backend.

**Files:**

- Create: `include/seaclaw/agent/commitment_store.h`
- Create: `src/agent/commitment_store.c`
- Test: `tests/test_commitment_store.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef SC_COMMITMENT_STORE_H
#define SC_COMMITMENT_STORE_H

#include "seaclaw/agent/commitment.h"
#include "seaclaw/memory.h"

typedef struct sc_commitment_store sc_commitment_store_t;

sc_error_t sc_commitment_store_create(sc_allocator_t *alloc, sc_memory_t *memory,
                                       sc_commitment_store_t **out);
sc_error_t sc_commitment_store_save(sc_commitment_store_t *store, const sc_commitment_t *c);
sc_error_t sc_commitment_store_list_active(sc_commitment_store_t *store, sc_allocator_t *alloc,
                                            sc_commitment_t **out, size_t *out_count);
sc_error_t sc_commitment_store_list_due(sc_commitment_store_t *store, sc_allocator_t *alloc,
                                         const char *before_date, size_t before_date_len,
                                         sc_commitment_t **out, size_t *out_count);
sc_error_t sc_commitment_store_update_status(sc_commitment_store_t *store,
                                              const char *id, size_t id_len,
                                              sc_commitment_status_t status);
sc_error_t sc_commitment_store_build_context(sc_commitment_store_t *store, sc_allocator_t *alloc,
                                              char **out, size_t *out_len);
void sc_commitment_store_destroy(sc_commitment_store_t *store);

#endif
```

**Step 2:** Tests: `store_save_and_list_active`, `store_update_status_to_completed`, `store_list_due_returns_past_deadlines`, `store_build_context_formats_commitments`.

**Step 3:** Implement. Store commitments as memory entries with category CUSTOM("commitment") and key `commitment:{id}`. Content is JSON: `{"statement","summary","type","status","target_date","follow_up_after","created_at","emotional_weight","owner"}`. List active: `memory->vtable->list()` with category filter, parse JSON, filter status=ACTIVE. Build context: format active commitments as markdown list.

**Step 4:** Tests pass. **Step 5:** Commit: `feat(agent): add commitment store with SQLite persistence`

---

### Task 10: Commitment Integration into Agent Loop

Wire commitment detection and context injection into the agent turn.

**Files:**

- Modify: `include/seaclaw/agent.h` — add `sc_commitment_store_t *commitments` to `sc_agent_t`
- Modify: `src/agent/agent.c` — init/deinit commitment store
- Modify: `src/agent/agent_turn.c` — detect commitments after user message, inject context
- Modify: `include/seaclaw/agent/prompt.h` — add `commitment_context` to prompt config
- Modify: `src/agent/prompt.c` — include commitment context

**Step 1:** Add `sc_commitment_store_t *commitment_store;` to `sc_agent_t`.

**Step 2:** After fast-capture in agent_turn.c, run `sc_commitment_detect` on user message. If commitments found, store each via `sc_commitment_store_save`.

**Step 3:** Before building system prompt, call `sc_commitment_store_build_context` and set `prompt_config.commitment_context`.

**Step 4:** In `sc_prompt_build_system`, after STM context, append commitment context under `### Active Commitments`.

**Step 5:** Tests: verify commitment detected and context injected.

**Step 6:** Commit: `feat(agent): integrate commitment detection and context into agent turn`

---

## Layer 3: Pattern Radar

### Task 11: Pattern Observation Recorder

Record and track recurring patterns across conversations — topics, emotions, behaviors.

**Files:**

- Create: `include/seaclaw/agent/pattern_radar.h`
- Create: `src/agent/pattern_radar.c`
- Test: `tests/test_pattern_radar.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef SC_PATTERN_RADAR_H
#define SC_PATTERN_RADAR_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"

#define SC_PATTERN_MAX_OBSERVATIONS 100

typedef enum sc_pattern_type {
    SC_PATTERN_TOPIC_RECURRENCE,    /* same topic appearing repeatedly */
    SC_PATTERN_EMOTIONAL_TREND,     /* emotion trending up or down */
    SC_PATTERN_BEHAVIORAL_CYCLE,    /* recurring at time-of-day or day-of-week */
    SC_PATTERN_RELATIONSHIP_SHIFT,  /* mentions of a person changing in tone */
    SC_PATTERN_GROWTH,              /* topic dropping off = improvement */
} sc_pattern_type_t;

typedef struct sc_pattern_observation {
    sc_pattern_type_t type;
    char *subject;          /* owned — what the pattern is about */
    size_t subject_len;
    char *detail;           /* owned — description */
    size_t detail_len;
    uint32_t occurrence_count;
    double confidence;       /* 0.0–1.0 */
    char *first_seen;       /* ISO 8601 — owned */
    char *last_seen;        /* ISO 8601 — owned */
} sc_pattern_observation_t;

typedef struct sc_pattern_radar {
    sc_allocator_t alloc;
    sc_memory_t *memory;
    sc_pattern_observation_t observations[SC_PATTERN_MAX_OBSERVATIONS];
    size_t observation_count;
} sc_pattern_radar_t;

sc_error_t sc_pattern_radar_init(sc_pattern_radar_t *radar, sc_allocator_t alloc,
                                  sc_memory_t *memory);

/* Record a new observation from fast-capture results */
sc_error_t sc_pattern_radar_observe(sc_pattern_radar_t *radar,
                                     const char *topic, size_t topic_len,
                                     sc_pattern_type_t type, const char *detail, size_t detail_len,
                                     const char *timestamp, size_t timestamp_len);

/* Analyze accumulated observations and generate insights */
sc_error_t sc_pattern_radar_analyze(sc_pattern_radar_t *radar, sc_allocator_t *alloc,
                                     char **insights, size_t *insights_len);

/* Build context string for prompt injection */
sc_error_t sc_pattern_radar_build_context(sc_pattern_radar_t *radar, sc_allocator_t *alloc,
                                           char **out, size_t *out_len);

void sc_pattern_radar_deinit(sc_pattern_radar_t *radar);

#endif
```

**Step 2:** Tests: `radar_tracks_topic_recurrence`, `radar_detects_emotional_trend`, `radar_generates_insight_after_threshold`, `radar_build_context_formats_patterns`.

**Step 3:** Implement. `observe`: find existing observation for subject+type, increment count, update last_seen; or insert new. `analyze`: find observations with count >= 3, generate insight text ("You've mentioned {subject} {count} times since {first_seen}"). `build_context`: format top insights as `### Pattern Insights\n- ...`.

**Step 4:** Tests pass. **Step 5:** Commit: `feat(agent): add pattern radar for tracking recurring conversation patterns`

---

### Task 12: Pattern Radar Integration

Wire pattern radar into the agent turn, fed by fast-capture results.

**Files:**

- Modify: `include/seaclaw/agent.h` — add `sc_pattern_radar_t radar` to `sc_agent_t`
- Modify: `src/agent/agent.c` — init/deinit
- Modify: `src/agent/agent_turn.c` — feed fast-capture results to radar, inject context
- Modify: `include/seaclaw/agent/prompt.h` — add `pattern_context`
- Modify: `src/agent/prompt.c` — include pattern context

**Step 1–5:** Same integration pattern as STM and commitments. After fast-capture, call `sc_pattern_radar_observe` for each entity/topic/emotion. Before prompt build, call `sc_pattern_radar_build_context`. Inject into system prompt.

**Step 6:** Commit: `feat(agent): integrate pattern radar into agent turn loop`

---

## Layer 4: Adaptive Persona

### Task 13: Circadian Persona Overlay

Adjust persona behavior based on time of day — calmer at night, energetic in morning.

**Files:**

- Create: `include/seaclaw/persona/circadian.h`
- Create: `src/persona/circadian.c`
- Test: `tests/test_circadian.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef SC_CIRCADIAN_H
#define SC_CIRCADIAN_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"

typedef enum sc_time_phase {
    SC_PHASE_EARLY_MORNING,  /* 5:00–8:00 — gentle, warming up */
    SC_PHASE_MORNING,        /* 8:00–12:00 — energetic, productive */
    SC_PHASE_AFTERNOON,      /* 12:00–17:00 — steady, focused */
    SC_PHASE_EVENING,        /* 17:00–21:00 — winding down, reflective */
    SC_PHASE_NIGHT,          /* 21:00–0:00 — calm, intimate, deeper */
    SC_PHASE_LATE_NIGHT,     /* 0:00–5:00 — quiet, present, unhurried */
} sc_time_phase_t;

typedef struct sc_circadian_overlay {
    sc_time_phase_t phase;
    const char *tone_guidance;       /* injected into persona prompt */
    size_t tone_guidance_len;
    const char *pacing_guidance;     /* speech tempo/length guidance */
    size_t pacing_guidance_len;
    double energy_level;             /* 0.0–1.0 */
} sc_circadian_overlay_t;

/* Get current phase from hour (0–23) */
sc_time_phase_t sc_circadian_phase(uint8_t hour);

/* Get overlay for current phase */
const sc_circadian_overlay_t *sc_circadian_get_overlay(sc_time_phase_t phase);

/* Build persona prompt addition for current time */
sc_error_t sc_circadian_build_prompt(sc_allocator_t *alloc, uint8_t hour,
                                      char **out, size_t *out_len);

#endif
```

**Step 2:** Tests: `phase_5am_is_early_morning`, `phase_10am_is_morning`, `phase_22pm_is_night`, `build_prompt_night_mentions_calm`, `build_prompt_morning_mentions_energy`.

**Step 3:** Implement. Static array of 6 overlays with tone/pacing text. `build_prompt` formats as: `\n\n### Time Awareness\nIt's currently {phase}. {tone_guidance} {pacing_guidance}`.

**Step 4:** Tests pass. **Step 5:** Commit: `feat(persona): add circadian time-of-day persona overlay`

---

### Task 14: Relationship Depth Tracker

Track how the relationship with the user deepens over time, adjusting warmth and formality.

**Files:**

- Create: `include/seaclaw/persona/relationship.h`
- Create: `src/persona/relationship.c`
- Test: `tests/test_relationship.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef SC_RELATIONSHIP_H
#define SC_RELATIONSHIP_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"

typedef enum sc_relationship_stage {
    SC_REL_NEW,           /* 0–5 sessions: formal, helpful, establishing trust */
    SC_REL_FAMILIAR,      /* 5–20 sessions: warmer, recalls preferences */
    SC_REL_TRUSTED,       /* 20–50 sessions: candid, proactive, personal */
    SC_REL_DEEP,          /* 50+ sessions: intimate, anticipatory, growth-oriented */
} sc_relationship_stage_t;

typedef struct sc_relationship_state {
    sc_relationship_stage_t stage;
    uint32_t session_count;
    uint32_t total_turns;
    uint32_t vulnerability_moments;  /* times user shared something personal */
    uint32_t humor_moments;          /* times user used humor */
    double trust_score;              /* 0.0–1.0, composite */
    char *first_interaction;         /* ISO 8601 — owned */
    char *last_interaction;          /* ISO 8601 — owned */
} sc_relationship_state_t;

sc_error_t sc_relationship_load(sc_allocator_t *alloc, sc_memory_t *memory,
                                 sc_relationship_state_t *out);
sc_error_t sc_relationship_save(sc_allocator_t *alloc, sc_memory_t *memory,
                                 const sc_relationship_state_t *state);
sc_error_t sc_relationship_update(sc_relationship_state_t *state,
                                   bool had_vulnerability, bool had_humor,
                                   uint32_t turn_count);
sc_error_t sc_relationship_build_prompt(sc_allocator_t *alloc,
                                         const sc_relationship_state_t *state,
                                         char **out, size_t *out_len);
void sc_relationship_state_deinit(sc_relationship_state_t *state, sc_allocator_t *alloc);

#endif
```

**Step 2:** Tests: `new_relationship_is_formal`, `familiar_after_5_sessions`, `trusted_after_20_sessions`, `deep_after_50_sessions`, `vulnerability_boosts_trust`.

**Step 3:** Implement. Stage transitions based on `session_count` + `trust_score`. Prompt additions per stage: NEW = "Be helpful and clear. Use their name occasionally.", FAMILIAR = "You can reference past conversations. Be warmer.", TRUSTED = "Be candid. Offer proactive insights. Share observations.", DEEP = "Be genuinely present. Anticipate needs. Celebrate growth."

**Step 4:** Tests pass. **Step 5:** Commit: `feat(persona): add relationship depth tracker with stage-based prompt adaptation`

---

### Task 15: Adaptive Persona Integration

Wire circadian overlay and relationship depth into the persona prompt builder.

**Files:**

- Modify: `src/agent/agent_turn.c` — compute circadian + relationship, inject into prompt
- Modify: `include/seaclaw/agent.h` — add relationship state to agent
- Modify: `src/agent/agent.c` — load/save relationship state on init/session-end
- Modify: `include/seaclaw/agent/prompt.h` — add adaptive persona fields
- Modify: `src/agent/prompt.c` — include adaptive persona context

**Steps:** Same integration pattern. Add `sc_relationship_state_t relationship;` to agent. Load on init. After each turn, call `sc_relationship_update`. Before prompt build, call `sc_circadian_build_prompt` and `sc_relationship_build_prompt`. Inject both into system prompt.

**Commit:** `feat(agent): integrate adaptive persona (circadian + relationship depth)`

---

## Layer 5: LLMCompiler (Parallel Execution)

### Task 16: DAG Data Structure and Validation

Define the task DAG (directed acyclic graph) with dependency tracking and cycle detection.

**Files:**

- Create: `include/seaclaw/agent/dag.h`
- Create: `src/agent/dag.c`
- Test: `tests/test_dag.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef SC_DAG_H
#define SC_DAG_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"

#define SC_DAG_MAX_NODES 32
#define SC_DAG_MAX_DEPS   8

typedef enum sc_dag_node_status {
    SC_DAG_PENDING,
    SC_DAG_READY,     /* all deps resolved */
    SC_DAG_RUNNING,
    SC_DAG_DONE,
    SC_DAG_FAILED,
} sc_dag_node_status_t;

typedef struct sc_dag_node {
    char *id;            /* owned — "t1", "t2", etc. */
    char *tool_name;     /* owned */
    char *args_json;     /* owned — may contain $t1 refs */
    char *deps[SC_DAG_MAX_DEPS]; /* owned dep IDs */
    size_t dep_count;
    sc_dag_node_status_t status;
    char *result;        /* owned — output after execution */
    size_t result_len;
} sc_dag_node_t;

typedef struct sc_dag {
    sc_allocator_t alloc;
    sc_dag_node_t nodes[SC_DAG_MAX_NODES];
    size_t node_count;
} sc_dag_t;

sc_error_t sc_dag_init(sc_dag_t *dag, sc_allocator_t alloc);
sc_error_t sc_dag_add_node(sc_dag_t *dag, const char *id, const char *tool_name,
                            const char *args_json, const char **deps, size_t dep_count);
sc_error_t sc_dag_validate(const sc_dag_t *dag); /* checks: no cycles, no missing deps, no dup IDs */
sc_error_t sc_dag_parse_json(sc_dag_t *dag, sc_allocator_t *alloc,
                              const char *json, size_t json_len);
void sc_dag_deinit(sc_dag_t *dag);

/* Query */
bool sc_dag_is_complete(const sc_dag_t *dag);
sc_dag_node_t *sc_dag_find_node(sc_dag_t *dag, const char *id, size_t id_len);

#endif
```

**Step 2:** Tests: `dag_add_node_and_find`, `dag_validate_detects_cycle`, `dag_validate_detects_missing_dep`, `dag_validate_detects_duplicate_id`, `dag_validate_accepts_valid_dag`, `dag_parse_json_creates_nodes`.

**Step 3:** Implement. Cycle detection: DFS with `visiting[]` and `visited[]` flags per node. Missing dep: for each dep ID, verify it exists in the node list. Parse JSON: `{"tasks":[{"id":"t1","tool":"web_search","args":{},"deps":["t0"]},...]}`.

**Step 4:** Tests pass. **Step 5:** Commit: `feat(agent): add DAG data structure with cycle detection for LLMCompiler`

---

### Task 17: Topological Sort and Batch Builder

Group DAG nodes into execution batches — each batch contains nodes whose dependencies are all resolved.

**Files:**

- Create: `include/seaclaw/agent/dag_executor.h`
- Create: `src/agent/dag_executor.c`
- Test: `tests/test_dag_executor.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef SC_DAG_EXECUTOR_H
#define SC_DAG_EXECUTOR_H

#include "seaclaw/agent/dag.h"
#include "seaclaw/tool.h"

#define SC_DAG_MAX_BATCH_SIZE 8

typedef struct sc_dag_batch {
    sc_dag_node_t *nodes[SC_DAG_MAX_BATCH_SIZE];
    size_t count;
} sc_dag_batch_t;

/* Get next batch of ready-to-execute nodes */
sc_error_t sc_dag_next_batch(sc_dag_t *dag, sc_dag_batch_t *batch);

/* Resolve $tN variable references in args using completed node results */
sc_error_t sc_dag_resolve_vars(sc_allocator_t *alloc, const sc_dag_t *dag,
                                const char *args, size_t args_len,
                                char **resolved, size_t *resolved_len);

/* Execute a full DAG using tool dispatch */
sc_error_t sc_dag_execute(sc_dag_t *dag, sc_allocator_t *alloc,
                           sc_tool_t *tools, size_t tools_count,
                           uint32_t max_parallel);

#endif
```

**Step 2:** Tests: `next_batch_returns_roots_first`, `next_batch_returns_dependents_after_roots_done`, `resolve_vars_substitutes_t1`, `resolve_vars_handles_no_refs`, `execute_runs_linear_dag`, `execute_runs_parallel_dag`.

**Step 3:** Implement. `next_batch`: scan nodes for PENDING where all deps are DONE → mark READY, add to batch. `resolve_vars`: regex-like scan for `$t` followed by alphanumeric; look up node by ID, replace with `node->result`. `execute`: loop — get batch, resolve vars for each, dispatch batch (sequential or parallel via pthreads), mark results.

**Step 4:** Tests pass. **Step 5:** Commit: `feat(agent): add DAG batch builder and variable resolution for LLMCompiler`

---

### Task 18: LLMCompiler Plan Generation

Generate a DAG plan from a user goal using the LLM.

**Files:**

- Create: `include/seaclaw/agent/llm_compiler.h`
- Create: `src/agent/llm_compiler.c`
- Test: `tests/test_llm_compiler.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef SC_LLM_COMPILER_H
#define SC_LLM_COMPILER_H

#include "seaclaw/agent/dag.h"
#include "seaclaw/provider.h"
#include "seaclaw/tool.h"

/* Build the planning prompt for LLM DAG generation */
sc_error_t sc_llm_compiler_build_prompt(sc_allocator_t *alloc,
                                         const char *goal, size_t goal_len,
                                         sc_tool_t *tools, size_t tools_count,
                                         char **out, size_t *out_len);

/* Parse LLM response into a DAG */
sc_error_t sc_llm_compiler_parse_plan(sc_allocator_t *alloc,
                                       const char *response, size_t response_len,
                                       sc_dag_t *dag);

/* Full pipeline: generate plan via LLM, validate, execute */
sc_error_t sc_llm_compiler_run(sc_allocator_t *alloc,
                                sc_provider_t *provider, const char *model,
                                const char *goal, size_t goal_len,
                                sc_tool_t *tools, size_t tools_count,
                                uint32_t max_parallel,
                                char **final_result, size_t *final_result_len);

#endif
```

**Step 2:** Tests: `build_prompt_includes_tools_and_goal`, `parse_plan_creates_valid_dag`, `parse_plan_rejects_invalid_json`.

**Step 3:** Implement. The prompt instructs the LLM: "Given this goal and these tools, produce a JSON DAG of tasks to execute. Format: `{"tasks":[{"id":"t1","tool":"name","args":{},"deps":[]},...]}`. Use `$tN` to reference outputs of previous tasks. Minimize sequential dependencies — parallelize where possible."

**Step 4:** Tests pass. **Step 5:** Commit: `feat(agent): add LLMCompiler plan generation and execution pipeline`

---

### Task 19: LLMCompiler Integration into Agent Turn

When the planner detects a multi-step goal, use LLMCompiler instead of sequential tool calls.

**Files:**

- Modify: `src/agent/agent_turn.c` — detect multi-tool scenarios, route to LLMCompiler
- Modify: `include/seaclaw/agent.h` — add config flag for LLMCompiler
- Modify: `include/seaclaw/config.h` — add `llm_compiler_enabled` to agent config

**Steps:** Add `bool llm_compiler_enabled;` to `sc_agent_config_t`. In the agent turn loop, when the provider returns 3+ tool calls, check if `llm_compiler_enabled`. If so, instead of dispatching sequentially, build a DAG from the tool calls (treating each as an independent node unless the LLM specified dependencies via `$tN` in args), and use `sc_dag_execute` with `max_parallel=4`.

**Commit:** `feat(agent): integrate LLMCompiler parallel execution into agent turn`

---

## Layer 6: Proactive Scheduler

### Task 20: Time-Triggered Action System

A scheduler that fires actions based on time — check-ins, reminders, milestone awareness.

**Files:**

- Create: `include/seaclaw/agent/proactive.h`
- Create: `src/agent/proactive.c`
- Test: `tests/test_proactive.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef SC_PROACTIVE_H
#define SC_PROACTIVE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"
#include "seaclaw/agent/commitment.h"

#define SC_PROACTIVE_MAX_ACTIONS 32

typedef enum sc_proactive_action_type {
    SC_PROACTIVE_COMMITMENT_FOLLOW_UP,
    SC_PROACTIVE_MILESTONE,
    SC_PROACTIVE_CHECK_IN,
    SC_PROACTIVE_MORNING_BRIEFING,
    SC_PROACTIVE_PATTERN_INSIGHT,
} sc_proactive_action_type_t;

typedef struct sc_proactive_action {
    sc_proactive_action_type_t type;
    char *message;       /* owned — the proactive message/question to inject */
    size_t message_len;
    char *context;       /* owned — supporting context for the LLM */
    size_t context_len;
    double priority;     /* 0.0–1.0 */
} sc_proactive_action_t;

typedef struct sc_proactive_result {
    sc_proactive_action_t actions[SC_PROACTIVE_MAX_ACTIONS];
    size_t count;
} sc_proactive_result_t;

/* Check for due actions given current time and memory state */
sc_error_t sc_proactive_check(sc_allocator_t *alloc, sc_memory_t *memory,
                               const char *current_time, size_t current_time_len,
                               sc_proactive_result_t *out);

/* Build context string for prompt injection (top N by priority) */
sc_error_t sc_proactive_build_context(const sc_proactive_result_t *result,
                                       sc_allocator_t *alloc, size_t max_actions,
                                       char **out, size_t *out_len);

void sc_proactive_result_deinit(sc_proactive_result_t *result, sc_allocator_t *alloc);

#endif
```

**Step 2:** Tests: `proactive_detects_due_commitment`, `proactive_generates_milestone_at_anniversary`, `proactive_morning_briefing_includes_commitments`, `proactive_build_context_sorts_by_priority`.

**Step 3:** Implement. `sc_proactive_check`: query commitment store for due follow-ups, check memory for milestones (first_interaction anniversary, session count milestones at 10/25/50/100), check time for morning briefing window (8-10am). Generate action for each.

**Step 4:** Tests pass. **Step 5:** Commit: `feat(agent): add proactive scheduler for time-triggered actions`

---

### Task 21: Proactive Context Integration

Wire proactive actions into the agent turn.

**Files:**

- Modify: `src/agent/agent_turn.c` — run proactive check at turn start, inject context
- Modify: `include/seaclaw/agent/prompt.h` — add `proactive_context`
- Modify: `src/agent/prompt.c` — include proactive context

**Steps:** At the start of the agent turn (before the main loop), call `sc_proactive_check` with current time. If actions exist, call `sc_proactive_build_context` and inject into system prompt under `### Proactive Awareness`. The LLM decides whether and how to surface these naturally.

**Commit:** `feat(agent): integrate proactive awareness into agent turn`

---

## Layer 7: Superhuman Services

### Task 22: Superhuman Service Registry

A registry for superhuman services that build specialized context from memory.

**Files:**

- Create: `include/seaclaw/agent/superhuman.h`
- Create: `src/agent/superhuman.c`
- Test: `tests/test_superhuman.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef SC_SUPERHUMAN_H
#define SC_SUPERHUMAN_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"
#include "seaclaw/memory/stm.h"

#define SC_SUPERHUMAN_MAX_SERVICES 16

typedef struct sc_superhuman_service {
    const char *name;
    /* Build context string for this service given memory + STM state */
    sc_error_t (*build_context)(void *ctx, sc_allocator_t *alloc, sc_memory_t *memory,
                                 const sc_stm_buffer_t *stm,
                                 char **out, size_t *out_len);
    /* Record an observation from the current turn */
    sc_error_t (*observe)(void *ctx, sc_allocator_t *alloc, sc_memory_t *memory,
                           const char *text, size_t text_len,
                           const char *role, size_t role_len);
    void *ctx;
} sc_superhuman_service_t;

typedef struct sc_superhuman_registry {
    sc_superhuman_service_t services[SC_SUPERHUMAN_MAX_SERVICES];
    size_t count;
} sc_superhuman_registry_t;

sc_error_t sc_superhuman_registry_init(sc_superhuman_registry_t *reg);
sc_error_t sc_superhuman_register(sc_superhuman_registry_t *reg,
                                   sc_superhuman_service_t service);

/* Build unified context from all registered services */
sc_error_t sc_superhuman_build_context(sc_superhuman_registry_t *reg,
                                        sc_allocator_t *alloc, sc_memory_t *memory,
                                        const sc_stm_buffer_t *stm,
                                        char **out, size_t *out_len);

/* Run all observe hooks for a turn */
sc_error_t sc_superhuman_observe_all(sc_superhuman_registry_t *reg,
                                      sc_allocator_t *alloc, sc_memory_t *memory,
                                      const char *text, size_t text_len,
                                      const char *role, size_t role_len);

#endif
```

**Step 2:** Tests: `registry_register_and_count`, `registry_build_context_calls_all`, `registry_observe_calls_all`.

**Step 3:** Implement. `build_context` iterates services, calls each `build_context`, concatenates results under `### Superhuman Insights\n#### {service_name}\n{context}\n\n`.

**Step 4:** Tests pass. **Step 5:** Commit: `feat(agent): add superhuman service registry`

---

### Task 23: Commitment Keeper Service

First superhuman service — wraps the commitment store with richer context building.

**Files:**

- Create: `src/agent/superhuman_commitment.c`
- Modify: `tests/test_superhuman.c` — add tests

**Step 1:** Implement `sc_superhuman_service_t` for commitment keeping. `build_context`: query active commitments, format as: "You made {count} active commitments. [list]. {follow_up_due} are due for follow-up." `observe`: run `sc_commitment_detect` on text, store results.

**Step 2:** Tests: `commitment_service_builds_context_with_active`, `commitment_service_observes_new_commitments`.

**Step 3:** Commit: `feat(agent): add commitment keeper superhuman service`

---

### Task 24: Predictive Coaching Service

Detects recurring patterns and generates coaching insights.

**Files:**

- Create: `src/agent/superhuman_predictive.c`
- Modify: `tests/test_superhuman.c`

**Step 1:** Implement. `build_context`: query pattern radar for high-confidence observations, format as coaching insights: "Pattern detected: {subject} comes up frequently when {trigger}. Consider: {coaching_prompt}." `observe`: feed topics to pattern radar.

**Step 2:** Tests: `predictive_service_surfaces_patterns`, `predictive_service_generates_coaching`.

**Step 3:** Commit: `feat(agent): add predictive coaching superhuman service`

---

### Task 25: Emotional First Aid Service

Detect emotional distress and provide appropriate grounding/support context.

**Files:**

- Create: `src/agent/superhuman_emotional.c`
- Modify: `tests/test_superhuman.c`

**Step 1: Write the implementation**

Static arrays of crisis patterns (keyword + level) and grounding scripts per level. `observe`: check STM emotions for high-intensity negative emotions. `build_context`: if distress detected, inject crisis-appropriate guidance: "The user may be experiencing {emotion}. Priority: {level}. Approach: {protocol}."

Crisis levels:

- **SAFETY**: immediate danger language → "Focus on safety. Ask directly. Provide hotline."
- **CONTAINING**: high distress → "Validate feelings. Don't try to fix. Be present."
- **STABILIZING**: moderate distress → "Help ground. Breathing exercise. Name emotions."
- **CALMING**: mild distress → "Gentle support. Normalize feelings. Offer perspective."
- **GROUNDING**: general unease → "Acknowledge. Light touch. Continue naturally."

**Step 2:** Tests: `emotional_detects_crisis_language`, `emotional_provides_grounding_for_anxiety`, `emotional_no_context_when_neutral`.

**Step 3:** Commit: `feat(agent): add emotional first aid superhuman service`

---

### Task 26: Silence Interpreter Service

Detect and interpret conversational pauses/silences meaningfully.

**Files:**

- Create: `src/agent/superhuman_silence.c`
- Modify: `tests/test_superhuman.c`

**Step 1:** Implement. `observe`: track timestamps between turns. If gap > threshold (configured, default 5s), record silence observation with context (what was being discussed). `build_context`: "A meaningful silence occurred after discussing {topic}. The user may be: processing, reflecting, or uncertain. Don't rush to fill it."

**Step 2:** Tests: `silence_detects_long_pause`, `silence_context_includes_topic`.

**Step 3:** Commit: `feat(agent): add silence interpreter superhuman service`

---

### Task 27: Superhuman Integration into Agent Turn

Wire the superhuman registry into the agent, register all services, and inject unified context.

**Files:**

- Modify: `include/seaclaw/agent.h` — add `sc_superhuman_registry_t superhuman` to agent
- Modify: `src/agent/agent.c` — init registry, register services
- Modify: `src/agent/agent_turn.c` — call observe + build_context
- Modify: `include/seaclaw/agent/prompt.h` — add `superhuman_context`
- Modify: `src/agent/prompt.c` — include superhuman context

**Steps:** Init registry in `sc_agent_init`. Register commitment keeper, predictive coaching, emotional first aid, silence interpreter. In agent_turn: after processing user message, call `sc_superhuman_observe_all`. Before prompt build, call `sc_superhuman_build_context`. Inject into system prompt under `### Superhuman Insights`.

**Commit:** `feat(agent): integrate superhuman service registry into agent turn`

---

## Layer 8: Semantic Tool Routing (Bonus)

### Task 28: Tool Relevance Scorer

Score tool relevance to the current conversation context, so only the most relevant tools are sent to the LLM.

**Files:**

- Create: `include/seaclaw/agent/tool_router.h`
- Create: `src/agent/tool_router.c`
- Test: `tests/test_tool_router.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef SC_TOOL_ROUTER_H
#define SC_TOOL_ROUTER_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/tool.h"

#define SC_TOOL_ROUTER_MAX_SELECTED 15
#define SC_TOOL_ROUTER_ALWAYS_MAX    8

typedef struct sc_tool_router {
    sc_allocator_t alloc;
    const char **always_tools;      /* tool names always included */
    size_t always_tools_count;
} sc_tool_router_t;

typedef struct sc_tool_selection {
    sc_tool_t *tools[SC_TOOL_ROUTER_MAX_SELECTED];
    size_t count;
} sc_tool_selection_t;

sc_error_t sc_tool_router_init(sc_tool_router_t *router, sc_allocator_t alloc);

/* Select most relevant tools for the given message */
sc_error_t sc_tool_router_select(sc_tool_router_t *router,
                                  const char *message, size_t message_len,
                                  sc_tool_t *all_tools, size_t all_tools_count,
                                  sc_tool_selection_t *out);

void sc_tool_router_deinit(sc_tool_router_t *router);

#endif
```

**Step 2:** Tests: `router_always_includes_core_tools`, `router_selects_relevant_by_keyword`, `router_limits_to_max_selected`.

**Step 3:** Implement. Always include: memory_store, memory_recall, message, shell. Then score remaining tools by keyword overlap between message text and tool name + description. Select top N by score up to `MAX_SELECTED`.

**Step 4:** Tests pass. **Step 5:** Commit: `feat(agent): add semantic tool routing for per-turn tool selection`

---

### Task 29: Tool Router Integration

Wire tool router into agent turn so only relevant tools are sent to the provider.

**Files:**

- Modify: `src/agent/agent_turn.c` — use tool router before provider chat call
- Modify: `include/seaclaw/agent.h` — add tool_router to agent

**Steps:** Before the provider `chat()` call in the main loop, run `sc_tool_router_select` to get a filtered tool set. Pass the filtered tools to the provider instead of all tools. This reduces token usage and improves tool selection quality.

**Commit:** `feat(agent): integrate semantic tool routing into agent turn`

---

## Validation

After each layer:

```bash
cmake --build build -j$(sysctl -n hw.ncpu) && ./build/seaclaw_tests
```

All tests must pass with 0 ASan errors.

After all layers:

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON \
  -DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_SQLITE=ON -DSC_ENABLE_PERSONA=ON
cmake --build build-release -j$(sysctl -n hw.ncpu)
ls -la build-release/seaclaw  # verify binary size still reasonable
```

---

## Summary

| Layer                  | Tasks  | New Files | What It Gives You                                                              |
| ---------------------- | ------ | --------- | ------------------------------------------------------------------------------ |
| 1. Temporal Memory     | 7      | 8         | Perfect recall — STM, fast-capture, deep extraction, promotion, consolidation  |
| 2. Promise Ledger      | 3      | 4         | Never forgets a commitment — detection, storage, context                       |
| 3. Pattern Radar       | 2      | 2         | Sees patterns humans can't — recurring topics, emotional trends                |
| 4. Adaptive Persona    | 3      | 4         | Feels alive — circadian, relationship depth, adaptive warmth                   |
| 5. LLMCompiler         | 4      | 6         | Thinks in parallel — DAG execution, variable substitution                      |
| 6. Proactive Scheduler | 2      | 2         | Cares proactively — follow-ups, milestones, briefings                          |
| 7. Superhuman Services | 6      | 5         | Better than human — commitment keeping, coaching, emotional first aid, silence |
| 8. Tool Routing        | 2      | 2         | Smarter tool selection — keyword scoring, per-turn filtering                   |
| **Total**              | **29** | **33**    | **The AI that never forgets, never sleeps, never judges**                      |
