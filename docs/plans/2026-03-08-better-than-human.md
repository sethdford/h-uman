# Better Than Human — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make human genuinely superhuman — perfect memory, pattern detection, proactive intelligence, adaptive persona, parallel reasoning, and commitment tracking — all in a single 511KB binary.

**Architecture:** Seven layers, each building on the last. Layer 1 (memory) is the foundation everything depends on. Each layer maps to human's existing vtable architecture: `hu_memory_t` for storage, `hu_tool_t` for actions, `hu_observer_t` for analysis, `hu_persona_t` for personality. New subsystems get their own headers in `include/human/` and implementations in `src/`.

**Tech Stack:** C11, SQLite (for persistent tiers), PCRE2-compatible regex (or simple pattern matching), pthreads (for parallel execution), existing `hu_arena_t`/`hu_allocator_t`/`hu_json_*` utilities.

---

## Layer 1: Temporal Memory Engine

The foundation. Without perfect recall, nothing else is superhuman. Four tiers of memory, from microseconds to permanent, with automatic capture and promotion.

### Task 1: STM Buffer — Data Structure

Short-term memory: an in-memory ring buffer holding the last N turns of the current session with extracted metadata.

**Files:**

- Create: `include/human/memory/stm.h`
- Create: `src/memory/stm.c`
- Test: `tests/test_stm.c`
- Modify: `tests/test_main.c` — add `run_stm_tests()`
- Modify: `CMakeLists.txt` — add both source files

**Step 1: Write the header**

```c
/* include/human/memory/stm.h */
#ifndef HU_STM_H
#define HU_STM_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/slice.h"

#define HU_STM_MAX_TURNS     20
#define HU_STM_MAX_ENTITIES  50
#define HU_STM_MAX_TOPICS    10
#define HU_STM_MAX_EMOTIONS   5

typedef enum hu_emotion_tag {
    HU_EMOTION_NEUTRAL,
    HU_EMOTION_JOY,
    HU_EMOTION_SADNESS,
    HU_EMOTION_ANGER,
    HU_EMOTION_FEAR,
    HU_EMOTION_SURPRISE,
    HU_EMOTION_FRUSTRATION,
    HU_EMOTION_EXCITEMENT,
    HU_EMOTION_ANXIETY,
} hu_emotion_tag_t;

typedef struct hu_stm_entity {
    char *name;            /* owned */
    size_t name_len;
    char *type;            /* "person", "place", "org", "date", "topic" — owned */
    size_t type_len;
    uint32_t mention_count;
    double importance;     /* 0.0–1.0, computed from frequency + recency + emotion */
} hu_stm_entity_t;

typedef struct hu_stm_emotion {
    hu_emotion_tag_t tag;
    double intensity;      /* 0.0–1.0 */
} hu_stm_emotion_t;

typedef struct hu_stm_turn {
    char *role;            /* "user" or "assistant" — owned */
    char *content;         /* owned */
    size_t content_len;
    hu_stm_entity_t entities[HU_STM_MAX_ENTITIES];
    size_t entity_count;
    hu_stm_emotion_t emotions[HU_STM_MAX_EMOTIONS];
    size_t emotion_count;
    char *primary_topic;   /* owned, NULL if none */
    uint64_t timestamp_ms;
} hu_stm_turn_t;

typedef struct hu_stm_buffer {
    hu_allocator_t alloc;
    hu_stm_turn_t turns[HU_STM_MAX_TURNS];
    size_t turn_count;       /* total turns added (wraps) */
    size_t head;             /* next write position */
    char *topics[HU_STM_MAX_TOPICS]; /* LRU topic history — owned */
    size_t topic_count;
    char *session_id;        /* owned */
    size_t session_id_len;
} hu_stm_buffer_t;

hu_error_t hu_stm_init(hu_stm_buffer_t *buf, hu_allocator_t alloc,
                        const char *session_id, size_t session_id_len);
hu_error_t hu_stm_record_turn(hu_stm_buffer_t *buf, const char *role, size_t role_len,
                               const char *content, size_t content_len, uint64_t timestamp_ms);
size_t hu_stm_count(const hu_stm_buffer_t *buf);
const hu_stm_turn_t *hu_stm_get(const hu_stm_buffer_t *buf, size_t idx);
hu_error_t hu_stm_build_context(const hu_stm_buffer_t *buf, hu_allocator_t *alloc,
                                 char **out, size_t *out_len);
void hu_stm_clear(hu_stm_buffer_t *buf);
void hu_stm_deinit(hu_stm_buffer_t *buf);

#endif
```

**Step 2: Write failing tests**

```c
/* tests/test_stm.c */
#include "test_framework.h"
#include "human/memory/stm.h"
#include "human/core/allocator.h"

static hu_allocator_t test_alloc(void) {
    return hu_allocator_libc();
}

static void stm_init_sets_session_id(void) {
    hu_stm_buffer_t buf;
    HU_ASSERT_EQ(hu_stm_init(&buf, test_alloc(), "sess-1", 6), HU_OK);
    HU_ASSERT_EQ(hu_stm_count(&buf), 0);
    HU_ASSERT_STR_EQ(buf.session_id, "sess-1");
    hu_stm_deinit(&buf);
}

static void stm_record_turn_stores_content(void) {
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, test_alloc(), "sess-1", 6);
    HU_ASSERT_EQ(hu_stm_record_turn(&buf, "user", 4, "hello world", 11, 1000), HU_OK);
    HU_ASSERT_EQ(hu_stm_count(&buf), 1);
    const hu_stm_turn_t *t = hu_stm_get(&buf, 0);
    HU_ASSERT_NOT_NULL(t);
    HU_ASSERT_STR_EQ(t->content, "hello world");
    HU_ASSERT_STR_EQ(t->role, "user");
    hu_stm_deinit(&buf);
}

static void stm_wraps_at_max_turns(void) {
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, test_alloc(), "sess-1", 6);
    for (size_t i = 0; i < HU_STM_MAX_TURNS + 5; i++) {
        char msg[32];
        int len = snprintf(msg, sizeof(msg), "turn-%zu", i);
        hu_stm_record_turn(&buf, "user", 4, msg, (size_t)len, 1000 + i);
    }
    HU_ASSERT_EQ(hu_stm_count(&buf), HU_STM_MAX_TURNS + 5);
    /* oldest should be turn-5 (first 5 were evicted) */
    const hu_stm_turn_t *oldest = hu_stm_get(&buf, 0);
    HU_ASSERT_NOT_NULL(oldest);
    HU_ASSERT_STR_EQ(oldest->content, "turn-5");
    hu_stm_deinit(&buf);
}

static void stm_build_context_formats_turns(void) {
    hu_stm_buffer_t buf;
    hu_allocator_t a = test_alloc();
    hu_stm_init(&buf, a, "sess-1", 6);
    hu_stm_record_turn(&buf, "user", 4, "What's the weather?", 19, 1000);
    hu_stm_record_turn(&buf, "assistant", 9, "It's sunny today.", 17, 2000);
    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_stm_build_context(&buf, &a, &ctx, &ctx_len), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(ctx_len > 0);
    /* Should contain both turns */
    HU_ASSERT_TRUE(strstr(ctx, "What's the weather?") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "It's sunny today.") != NULL);
    a.free(a.ctx, ctx, ctx_len + 1);
    hu_stm_deinit(&buf);
}

static void stm_clear_resets_buffer(void) {
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, test_alloc(), "sess-1", 6);
    hu_stm_record_turn(&buf, "user", 4, "hello", 5, 1000);
    HU_ASSERT_EQ(hu_stm_count(&buf), 1);
    hu_stm_clear(&buf);
    HU_ASSERT_EQ(hu_stm_count(&buf), 0);
    hu_stm_deinit(&buf);
}

void run_stm_tests(void) {
    HU_TEST_SUITE("stm_buffer");
    HU_RUN_TEST(stm_init_sets_session_id);
    HU_RUN_TEST(stm_record_turn_stores_content);
    HU_RUN_TEST(stm_wraps_at_max_turns);
    HU_RUN_TEST(stm_build_context_formats_turns);
    HU_RUN_TEST(stm_clear_resets_buffer);
}
```

**Step 3: Implement `src/memory/stm.c`**

Ring buffer with FIFO eviction. On `record_turn`: free oldest if full, copy content, advance head. `build_context` formats recent turns as markdown. `get(idx)` maps logical index to physical position in ring.

**Step 4:** Run: `cmake --build build && ./build/human_tests` — 5 new tests pass

**Step 5:** Commit: `feat(memory): add STM buffer for short-term session memory`

---

### Task 2: Fast-Capture Regex Patterns

Extract entities, emotions, topics, and dates from text in <1ms using pattern matching. No LLM needed.

**Files:**

- Create: `include/human/memory/fast_capture.h`
- Create: `src/memory/fast_capture.c`
- Test: `tests/test_fast_capture.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
/* include/human/memory/fast_capture.h */
#ifndef HU_FAST_CAPTURE_H
#define HU_FAST_CAPTURE_H

#include "human/memory/stm.h"

#define HU_FC_MAX_RESULTS 20

typedef struct hu_fc_entity_match {
    char *name;       /* owned */
    size_t name_len;
    char *type;       /* "person", "date", "topic", "emotion" — owned */
    size_t type_len;
    double confidence; /* 0.0–1.0 */
    size_t offset;     /* byte offset in source text */
} hu_fc_entity_match_t;

typedef struct hu_fc_result {
    hu_fc_entity_match_t entities[HU_FC_MAX_RESULTS];
    size_t entity_count;
    hu_stm_emotion_t emotions[HU_STM_MAX_EMOTIONS];
    size_t emotion_count;
    char *primary_topic;  /* owned, NULL if none */
    bool has_commitment;  /* "I will", "I'm going to", "I promise" detected */
    bool has_question;    /* ends with ? or contains question patterns */
} hu_fc_result_t;

hu_error_t hu_fast_capture(hu_allocator_t *alloc, const char *text, size_t text_len,
                            hu_fc_result_t *out);
void hu_fc_result_deinit(hu_fc_result_t *result, hu_allocator_t *alloc);

#endif
```

**Step 2: Write failing tests**

Tests for: relationship words detection ("my mom", "my friend"), emotion patterns ("I'm so frustrated", "I feel great"), topic classification ("at work", "the doctor"), commitment detection ("I'll do it", "I promise"), question detection. Each test provides a sentence and asserts the expected match.

**Step 3: Implement**

Pattern arrays: `RELATIONSHIP_PATTERNS[]` = `{"my mom", "my dad", "my wife", "my husband", "my friend", "my sister", "my brother", "my boss", "my coworker", ...}`, scanned with `strstr` or `hu_str_contains`. Emotion patterns: keyword → emotion + intensity. Topic patterns: keyword clusters → topic. Commitment: `"I will"`, `"I'm going to"`, `"I promise"`, `"remind me"`. All O(n) single pass.

**Step 4:** Tests pass

**Step 5:** Commit: `feat(memory): add fast-capture regex pattern extraction`

---

### Task 3: Fast-Capture Integration into Agent Turn

Wire fast-capture into the agent turn so every user message is analyzed and entities are recorded in STM.

**Files:**

- Modify: `src/agent/agent_turn.c` — after user message append, run fast-capture + STM record
- Modify: `include/human/agent.h` — add `hu_stm_buffer_t *stm` to `hu_agent_t`
- Modify: `src/agent/agent.c` — init/deinit STM buffer
- Test: `tests/test_agent.c` — verify STM populated after turn

**Step 1:** Add `hu_stm_buffer_t stm;` field to `hu_agent_t` in `include/human/agent.h`.

**Step 2:** In `hu_agent_init` (`src/agent/agent.c`), call `hu_stm_init(&agent->stm, ...)`.

**Step 3:** In `hu_agent_turn` (`src/agent/agent_turn.c`), after appending the user message to history (around line 176), add:

```c
hu_fc_result_t fc;
memset(&fc, 0, sizeof(fc));
if (hu_fast_capture(agent->alloc, msg, msg_len, &fc) == HU_OK) {
    hu_stm_turn_t *turn = /* current turn from stm */;
    /* Copy extracted entities and emotions into the STM turn */
    memcpy(turn->entities, fc.entities, sizeof(hu_stm_entity_t) * fc.entity_count);
    turn->entity_count = fc.entity_count;
    memcpy(turn->emotions, fc.emotions, sizeof(hu_stm_emotion_t) * fc.emotion_count);
    turn->emotion_count = fc.emotion_count;
    turn->primary_topic = fc.primary_topic;
    fc.primary_topic = NULL; /* transfer ownership */
}
hu_fc_result_deinit(&fc, agent->alloc);
```

**Step 4:** In `hu_agent_deinit`, call `hu_stm_deinit(&agent->stm)`.

**Step 5:** Test: verify that after `hu_agent_turn`, the STM buffer contains the turn with extracted entities.

**Step 6:** Commit: `feat(agent): integrate fast-capture + STM into agent turn loop`

---

### Task 4: STM Context Injection into System Prompt

Inject the STM buffer's context into the system prompt so the LLM has awareness of recent conversation signals.

**Files:**

- Modify: `src/agent/agent_turn.c` — build STM context, pass to prompt config
- Modify: `include/human/agent/prompt.h` — add `stm_context` field to `hu_prompt_config_t`
- Modify: `src/agent/prompt.c` — include STM context in system prompt

**Step 1:** Add `const char *stm_context; size_t stm_context_len;` to `hu_prompt_config_t`.

**Step 2:** In `hu_prompt_build_system`, after memory context, append STM context:

```c
if (config->stm_context && config->stm_context_len > 0) {
    hu_json_buf_append_str(buf, "\n\n### Session Context\n");
    hu_json_buf_append(buf, config->stm_context, config->stm_context_len);
}
```

**Step 3:** In `agent_turn.c`, before building system prompt, call `hu_stm_build_context` and set `prompt_config.stm_context`.

**Step 4:** Tests pass

**Step 5:** Commit: `feat(agent): inject STM session context into system prompt`

---

### Task 5: Deep Extraction Job (Async LLM)

After the agent turn completes, dispatch an async LLM call to extract structured facts, relationships, and insights from the conversation. Store results in memory (L2).

**Files:**

- Create: `include/human/memory/deep_extract.h`
- Create: `src/memory/deep_extract.c`
- Test: `tests/test_deep_extract.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef HU_DEEP_EXTRACT_H
#define HU_DEEP_EXTRACT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "human/memory.h"

#define HU_DE_MAX_FACTS      20
#define HU_DE_MAX_ENTITIES   10
#define HU_DE_MAX_RELATIONS  10

typedef struct hu_extracted_fact {
    char *subject;    /* owned */
    char *predicate;  /* owned */
    char *object;     /* owned */
    double confidence;
} hu_extracted_fact_t;

typedef struct hu_extracted_relation {
    char *entity_a;   /* owned */
    char *relation;   /* owned — "parent_of", "friend_of", "works_at", etc. */
    char *entity_b;   /* owned */
    double confidence;
} hu_extracted_relation_t;

typedef struct hu_deep_extract_result {
    hu_extracted_fact_t facts[HU_DE_MAX_FACTS];
    size_t fact_count;
    hu_extracted_relation_t relations[HU_DE_MAX_RELATIONS];
    size_t relation_count;
    char *summary;     /* owned — 1-2 sentence session summary */
    size_t summary_len;
} hu_deep_extract_result_t;

/* Builds the extraction prompt from conversation turns */
hu_error_t hu_deep_extract_build_prompt(hu_allocator_t *alloc,
                                         const char *conversation, size_t conversation_len,
                                         char **out, size_t *out_len);

/* Parses the LLM response JSON into structured results */
hu_error_t hu_deep_extract_parse(hu_allocator_t *alloc, const char *response, size_t response_len,
                                  hu_deep_extract_result_t *out);

/* Stores extracted results into memory backend */
hu_error_t hu_deep_extract_store(hu_allocator_t *alloc, hu_memory_t *memory,
                                  const char *session_id, size_t session_id_len,
                                  const hu_deep_extract_result_t *result);

void hu_deep_extract_result_deinit(hu_deep_extract_result_t *result, hu_allocator_t *alloc);

#endif
```

**Step 2:** Write tests: `deep_extract_build_prompt_includes_conversation`, `deep_extract_parse_extracts_facts`, `deep_extract_parse_extracts_relations`, `deep_extract_parse_handles_empty`.

**Step 3:** Implement. The prompt instructs the LLM to return JSON with `facts[]`, `relations[]`, and `summary`. The parser uses `hu_json_parse`. The store function calls `memory->vtable->store()` for each fact with category `HU_MEMORY_CATEGORY_CONVERSATION`.

**Step 4:** Tests pass

**Step 5:** Commit: `feat(memory): add deep extraction for structured fact/relation extraction`

---

### Task 6: Session Promotion (L1 → L2)

When a session ends, promote important entities and patterns from STM to persistent memory.

**Files:**

- Create: `include/human/memory/promotion.h`
- Create: `src/memory/promotion.c`
- Test: `tests/test_promotion.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef HU_MEMORY_PROMOTION_H
#define HU_MEMORY_PROMOTION_H

#include "human/memory/stm.h"
#include "human/memory.h"

typedef struct hu_promotion_config {
    uint32_t min_mention_count;   /* entities must be mentioned >= this many times */
    double min_importance;         /* minimum importance score (0.0–1.0) */
    uint32_t max_entities;         /* max entities to promote per session */
} hu_promotion_config_t;

#define HU_PROMOTION_DEFAULTS { .min_mention_count = 2, .min_importance = 0.3, .max_entities = 15 }

/* Calculate importance score for an entity */
double hu_promotion_entity_importance(const hu_stm_entity_t *entity,
                                       const hu_stm_buffer_t *buf);

/* Promote qualifying entities from STM to persistent memory */
hu_error_t hu_promotion_run(hu_allocator_t *alloc, const hu_stm_buffer_t *buf,
                             hu_memory_t *memory, const hu_promotion_config_t *config);

#endif
```

**Step 2:** Tests: `promotion_skips_low_importance`, `promotion_promotes_frequent_entities`, `promotion_respects_max_cap`.

**Step 3:** Implement. Sort entities by importance (mention count _ recency weight _ emotion boost). Promote top N above threshold. Store as memory entries with category CORE and key `entity:{name}`.

**Step 4:** Tests pass. **Step 5:** Commit: `feat(memory): add session promotion for STM entities to persistent memory`

---

### Task 7: Memory Consolidation Job

Periodically merge duplicate memories, decay old entries, and promote patterns.

**Files:**

- Create: `include/human/memory/consolidation.h`
- Create: `src/memory/consolidation.c`
- Test: `tests/test_consolidation.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef HU_MEMORY_CONSOLIDATION_H
#define HU_MEMORY_CONSOLIDATION_H

#include "human/memory.h"

typedef struct hu_consolidation_config {
    uint32_t decay_days;        /* memories older than this lose importance */
    double decay_factor;         /* multiplier per decay period (e.g. 0.9) */
    uint32_t dedup_threshold;    /* similarity score for merging (0–100) */
    uint32_t max_entries;        /* cap total entries, prune lowest importance */
} hu_consolidation_config_t;

#define HU_CONSOLIDATION_DEFAULTS { .decay_days = 30, .decay_factor = 0.9, \
                                     .dedup_threshold = 85, .max_entries = 10000 }

/* Run consolidation pass over the memory backend */
hu_error_t hu_memory_consolidate(hu_allocator_t *alloc, hu_memory_t *memory,
                                  const hu_consolidation_config_t *config);

/* Compute string similarity score (0–100) using token overlap */
uint32_t hu_similarity_score(const char *a, size_t a_len, const char *b, size_t b_len);

#endif
```

**Step 2:** Tests: `similarity_identical_strings_100`, `similarity_different_strings_low`, `consolidation_merges_duplicates`.

**Step 3:** Implement. `hu_similarity_score` uses token overlap (split by space, count shared / total). `hu_memory_consolidate` lists all entries, groups by key prefix, merges entries above threshold, removes oldest beyond max_entries.

**Step 4:** Tests pass. **Step 5:** Commit: `feat(memory): add memory consolidation with dedup and decay`

---

## Layer 2: Promise Ledger

### Task 8: Commitment Detection

Detect promises, intentions, and reminders from text using pattern matching.

**Files:**

- Create: `include/human/agent/commitment.h`
- Create: `src/agent/commitment.c`
- Test: `tests/test_commitment.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef HU_COMMITMENT_H
#define HU_COMMITMENT_H

#include "human/core/allocator.h"
#include "human/core/error.h"

typedef enum hu_commitment_type {
    HU_COMMIT_PROMISE,     /* "I will", "I'll" */
    HU_COMMIT_INTENTION,   /* "I'm going to", "I plan to" */
    HU_COMMIT_REMINDER,    /* "remind me", "don't let me forget" */
    HU_COMMIT_GOAL,        /* "I want to", "my goal is" */
} hu_commitment_type_t;

typedef enum hu_commitment_status {
    HU_COMMIT_STATUS_ACTIVE,
    HU_COMMIT_STATUS_COMPLETED,
    HU_COMMIT_STATUS_EXPIRED,
    HU_COMMIT_STATUS_CANCELLED,
} hu_commitment_status_t;

typedef struct hu_commitment {
    char *id;               /* owned — UUID */
    char *statement;         /* owned — the original text */
    size_t statement_len;
    char *summary;           /* owned — extracted core commitment */
    size_t summary_len;
    hu_commitment_type_t type;
    hu_commitment_status_t status;
    char *target_date;       /* owned, NULL if no deadline — ISO 8601 */
    char *follow_up_after;   /* owned, NULL if no follow-up — ISO 8601 */
    char *created_at;        /* owned — ISO 8601 */
    double emotional_weight; /* 0.0–1.0 */
    char *owner;             /* "user" or "assistant" — owned */
} hu_commitment_t;

#define HU_COMMITMENT_MAX_DETECT 5

typedef struct hu_commitment_detect_result {
    hu_commitment_t commitments[HU_COMMITMENT_MAX_DETECT];
    size_t count;
} hu_commitment_detect_result_t;

/* Detect commitments in text */
hu_error_t hu_commitment_detect(hu_allocator_t *alloc, const char *text, size_t text_len,
                                 const char *role, size_t role_len,
                                 hu_commitment_detect_result_t *out);

/* Free a commitment's owned fields */
void hu_commitment_deinit(hu_commitment_t *c, hu_allocator_t *alloc);
void hu_commitment_detect_result_deinit(hu_commitment_detect_result_t *r, hu_allocator_t *alloc);

#endif
```

**Step 2:** Tests: `detect_promise_i_will`, `detect_intention_going_to`, `detect_reminder_remind_me`, `detect_goal_i_want_to`, `detect_no_commitment_in_casual`, `detect_ignores_negation` ("I will not" should not match).

**Step 3:** Implement. Pattern arrays: `PROMISE_PATTERNS[] = {"I will ", "I'll ", "I promise "}`, `INTENTION_PATTERNS[] = {"I'm going to ", "I am going to ", "I plan to "}`, etc. Scan text for each pattern. Extract the clause following the pattern (up to period/comma/newline). Set owner based on role. Generate UUID for id. Set `created_at` to current ISO 8601. Negation filter: skip if preceded by "not", "n't", "never".

**Step 4:** Tests pass. **Step 5:** Commit: `feat(agent): add commitment detection from conversation text`

---

### Task 9: Commitment Storage (SQLite)

Store and query commitments in the SQLite memory backend.

**Files:**

- Create: `include/human/agent/commitment_store.h`
- Create: `src/agent/commitment_store.c`
- Test: `tests/test_commitment_store.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef HU_COMMITMENT_STORE_H
#define HU_COMMITMENT_STORE_H

#include "human/agent/commitment.h"
#include "human/memory.h"

typedef struct hu_commitment_store hu_commitment_store_t;

hu_error_t hu_commitment_store_create(hu_allocator_t *alloc, hu_memory_t *memory,
                                       hu_commitment_store_t **out);
hu_error_t hu_commitment_store_save(hu_commitment_store_t *store, const hu_commitment_t *c);
hu_error_t hu_commitment_store_list_active(hu_commitment_store_t *store, hu_allocator_t *alloc,
                                            hu_commitment_t **out, size_t *out_count);
hu_error_t hu_commitment_store_list_due(hu_commitment_store_t *store, hu_allocator_t *alloc,
                                         const char *before_date, size_t before_date_len,
                                         hu_commitment_t **out, size_t *out_count);
hu_error_t hu_commitment_store_update_status(hu_commitment_store_t *store,
                                              const char *id, size_t id_len,
                                              hu_commitment_status_t status);
hu_error_t hu_commitment_store_build_context(hu_commitment_store_t *store, hu_allocator_t *alloc,
                                              char **out, size_t *out_len);
void hu_commitment_store_destroy(hu_commitment_store_t *store);

#endif
```

**Step 2:** Tests: `store_save_and_list_active`, `store_update_status_to_completed`, `store_list_due_returns_past_deadlines`, `store_build_context_formats_commitments`.

**Step 3:** Implement. Store commitments as memory entries with category CUSTOM("commitment") and key `commitment:{id}`. Content is JSON: `{"statement","summary","type","status","target_date","follow_up_after","created_at","emotional_weight","owner"}`. List active: `memory->vtable->list()` with category filter, parse JSON, filter status=ACTIVE. Build context: format active commitments as markdown list.

**Step 4:** Tests pass. **Step 5:** Commit: `feat(agent): add commitment store with SQLite persistence`

---

### Task 10: Commitment Integration into Agent Loop

Wire commitment detection and context injection into the agent turn.

**Files:**

- Modify: `include/human/agent.h` — add `hu_commitment_store_t *commitments` to `hu_agent_t`
- Modify: `src/agent/agent.c` — init/deinit commitment store
- Modify: `src/agent/agent_turn.c` — detect commitments after user message, inject context
- Modify: `include/human/agent/prompt.h` — add `commitment_context` to prompt config
- Modify: `src/agent/prompt.c` — include commitment context

**Step 1:** Add `hu_commitment_store_t *commitment_store;` to `hu_agent_t`.

**Step 2:** After fast-capture in agent_turn.c, run `hu_commitment_detect` on user message. If commitments found, store each via `hu_commitment_store_save`.

**Step 3:** Before building system prompt, call `hu_commitment_store_build_context` and set `prompt_config.commitment_context`.

**Step 4:** In `hu_prompt_build_system`, after STM context, append commitment context under `### Active Commitments`.

**Step 5:** Tests: verify commitment detected and context injected.

**Step 6:** Commit: `feat(agent): integrate commitment detection and context into agent turn`

---

## Layer 3: Pattern Radar

### Task 11: Pattern Observation Recorder

Record and track recurring patterns across conversations — topics, emotions, behaviors.

**Files:**

- Create: `include/human/agent/pattern_radar.h`
- Create: `src/agent/pattern_radar.c`
- Test: `tests/test_pattern_radar.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef HU_PATTERN_RADAR_H
#define HU_PATTERN_RADAR_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"

#define HU_PATTERN_MAX_OBSERVATIONS 100

typedef enum hu_pattern_type {
    HU_PATTERN_TOPIC_RECURRENCE,    /* same topic appearing repeatedly */
    HU_PATTERN_EMOTIONAL_TREND,     /* emotion trending up or down */
    HU_PATTERN_BEHAVIORAL_CYCLE,    /* recurring at time-of-day or day-of-week */
    HU_PATTERN_RELATIONSHIP_SHIFT,  /* mentions of a person changing in tone */
    HU_PATTERN_GROWTH,              /* topic dropping off = improvement */
} hu_pattern_type_t;

typedef struct hu_pattern_observation {
    hu_pattern_type_t type;
    char *subject;          /* owned — what the pattern is about */
    size_t subject_len;
    char *detail;           /* owned — description */
    size_t detail_len;
    uint32_t occurrence_count;
    double confidence;       /* 0.0–1.0 */
    char *first_seen;       /* ISO 8601 — owned */
    char *last_seen;        /* ISO 8601 — owned */
} hu_pattern_observation_t;

typedef struct hu_pattern_radar {
    hu_allocator_t alloc;
    hu_memory_t *memory;
    hu_pattern_observation_t observations[HU_PATTERN_MAX_OBSERVATIONS];
    size_t observation_count;
} hu_pattern_radar_t;

hu_error_t hu_pattern_radar_init(hu_pattern_radar_t *radar, hu_allocator_t alloc,
                                  hu_memory_t *memory);

/* Record a new observation from fast-capture results */
hu_error_t hu_pattern_radar_observe(hu_pattern_radar_t *radar,
                                     const char *topic, size_t topic_len,
                                     hu_pattern_type_t type, const char *detail, size_t detail_len,
                                     const char *timestamp, size_t timestamp_len);

/* Analyze accumulated observations and generate insights */
hu_error_t hu_pattern_radar_analyze(hu_pattern_radar_t *radar, hu_allocator_t *alloc,
                                     char **insights, size_t *insights_len);

/* Build context string for prompt injection */
hu_error_t hu_pattern_radar_build_context(hu_pattern_radar_t *radar, hu_allocator_t *alloc,
                                           char **out, size_t *out_len);

void hu_pattern_radar_deinit(hu_pattern_radar_t *radar);

#endif
```

**Step 2:** Tests: `radar_tracks_topic_recurrence`, `radar_detects_emotional_trend`, `radar_generates_insight_after_threshold`, `radar_build_context_formats_patterns`.

**Step 3:** Implement. `observe`: find existing observation for subject+type, increment count, update last_seen; or insert new. `analyze`: find observations with count >= 3, generate insight text ("You've mentioned {subject} {count} times since {first_seen}"). `build_context`: format top insights as `### Pattern Insights\n- ...`.

**Step 4:** Tests pass. **Step 5:** Commit: `feat(agent): add pattern radar for tracking recurring conversation patterns`

---

### Task 12: Pattern Radar Integration

Wire pattern radar into the agent turn, fed by fast-capture results.

**Files:**

- Modify: `include/human/agent.h` — add `hu_pattern_radar_t radar` to `hu_agent_t`
- Modify: `src/agent/agent.c` — init/deinit
- Modify: `src/agent/agent_turn.c` — feed fast-capture results to radar, inject context
- Modify: `include/human/agent/prompt.h` — add `pattern_context`
- Modify: `src/agent/prompt.c` — include pattern context

**Step 1–5:** Same integration pattern as STM and commitments. After fast-capture, call `hu_pattern_radar_observe` for each entity/topic/emotion. Before prompt build, call `hu_pattern_radar_build_context`. Inject into system prompt.

**Step 6:** Commit: `feat(agent): integrate pattern radar into agent turn loop`

---

## Layer 4: Adaptive Persona

### Task 13: Circadian Persona Overlay

Adjust persona behavior based on time of day — calmer at night, energetic in morning.

**Files:**

- Create: `include/human/persona/circadian.h`
- Create: `src/persona/circadian.c`
- Test: `tests/test_circadian.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef HU_CIRCADIAN_H
#define HU_CIRCADIAN_H

#include "human/core/allocator.h"
#include "human/core/error.h"

typedef enum hu_time_phase {
    HU_PHASE_EARLY_MORNING,  /* 5:00–8:00 — gentle, warming up */
    HU_PHASE_MORNING,        /* 8:00–12:00 — energetic, productive */
    HU_PHASE_AFTERNOON,      /* 12:00–17:00 — steady, focused */
    HU_PHASE_EVENING,        /* 17:00–21:00 — winding down, reflective */
    HU_PHASE_NIGHT,          /* 21:00–0:00 — calm, intimate, deeper */
    HU_PHASE_LATE_NIGHT,     /* 0:00–5:00 — quiet, present, unhurried */
} hu_time_phase_t;

typedef struct hu_circadian_overlay {
    hu_time_phase_t phase;
    const char *tone_guidance;       /* injected into persona prompt */
    size_t tone_guidance_len;
    const char *pacing_guidance;     /* speech tempo/length guidance */
    size_t pacing_guidance_len;
    double energy_level;             /* 0.0–1.0 */
} hu_circadian_overlay_t;

/* Get current phase from hour (0–23) */
hu_time_phase_t hu_circadian_phase(uint8_t hour);

/* Get overlay for current phase */
const hu_circadian_overlay_t *hu_circadian_get_overlay(hu_time_phase_t phase);

/* Build persona prompt addition for current time */
hu_error_t hu_circadian_build_prompt(hu_allocator_t *alloc, uint8_t hour,
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

- Create: `include/human/persona/relationship.h`
- Create: `src/persona/relationship.c`
- Test: `tests/test_relationship.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef HU_RELATIONSHIP_H
#define HU_RELATIONSHIP_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"

typedef enum hu_relationship_stage {
    HU_REL_NEW,           /* 0–5 sessions: formal, helpful, establishing trust */
    HU_REL_FAMILIAR,      /* 5–20 sessions: warmer, recalls preferences */
    HU_REL_TRUSTED,       /* 20–50 sessions: candid, proactive, personal */
    HU_REL_DEEP,          /* 50+ sessions: intimate, anticipatory, growth-oriented */
} hu_relationship_stage_t;

typedef struct hu_relationship_state {
    hu_relationship_stage_t stage;
    uint32_t session_count;
    uint32_t total_turns;
    uint32_t vulnerability_moments;  /* times user shared something personal */
    uint32_t humor_moments;          /* times user used humor */
    double trust_score;              /* 0.0–1.0, composite */
    char *first_interaction;         /* ISO 8601 — owned */
    char *last_interaction;          /* ISO 8601 — owned */
} hu_relationship_state_t;

hu_error_t hu_relationship_load(hu_allocator_t *alloc, hu_memory_t *memory,
                                 hu_relationship_state_t *out);
hu_error_t hu_relationship_save(hu_allocator_t *alloc, hu_memory_t *memory,
                                 const hu_relationship_state_t *state);
hu_error_t hu_relationship_update(hu_relationship_state_t *state,
                                   bool had_vulnerability, bool had_humor,
                                   uint32_t turn_count);
hu_error_t hu_relationship_build_prompt(hu_allocator_t *alloc,
                                         const hu_relationship_state_t *state,
                                         char **out, size_t *out_len);
void hu_relationship_state_deinit(hu_relationship_state_t *state, hu_allocator_t *alloc);

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
- Modify: `include/human/agent.h` — add relationship state to agent
- Modify: `src/agent/agent.c` — load/save relationship state on init/session-end
- Modify: `include/human/agent/prompt.h` — add adaptive persona fields
- Modify: `src/agent/prompt.c` — include adaptive persona context

**Steps:** Same integration pattern. Add `hu_relationship_state_t relationship;` to agent. Load on init. After each turn, call `hu_relationship_update`. Before prompt build, call `hu_circadian_build_prompt` and `hu_relationship_build_prompt`. Inject both into system prompt.

**Commit:** `feat(agent): integrate adaptive persona (circadian + relationship depth)`

---

## Layer 5: LLMCompiler (Parallel Execution)

### Task 16: DAG Data Structure and Validation

Define the task DAG (directed acyclic graph) with dependency tracking and cycle detection.

**Files:**

- Create: `include/human/agent/dag.h`
- Create: `src/agent/dag.c`
- Test: `tests/test_dag.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef HU_DAG_H
#define HU_DAG_H

#include "human/core/allocator.h"
#include "human/core/error.h"

#define HU_DAG_MAX_NODES 32
#define HU_DAG_MAX_DEPS   8

typedef enum hu_dag_node_status {
    HU_DAG_PENDING,
    HU_DAG_READY,     /* all deps resolved */
    HU_DAG_RUNNING,
    HU_DAG_DONE,
    HU_DAG_FAILED,
} hu_dag_node_status_t;

typedef struct hu_dag_node {
    char *id;            /* owned — "t1", "t2", etc. */
    char *tool_name;     /* owned */
    char *args_json;     /* owned — may contain $t1 refs */
    char *deps[HU_DAG_MAX_DEPS]; /* owned dep IDs */
    size_t dep_count;
    hu_dag_node_status_t status;
    char *result;        /* owned — output after execution */
    size_t result_len;
} hu_dag_node_t;

typedef struct hu_dag {
    hu_allocator_t alloc;
    hu_dag_node_t nodes[HU_DAG_MAX_NODES];
    size_t node_count;
} hu_dag_t;

hu_error_t hu_dag_init(hu_dag_t *dag, hu_allocator_t alloc);
hu_error_t hu_dag_add_node(hu_dag_t *dag, const char *id, const char *tool_name,
                            const char *args_json, const char **deps, size_t dep_count);
hu_error_t hu_dag_validate(const hu_dag_t *dag); /* checks: no cycles, no missing deps, no dup IDs */
hu_error_t hu_dag_parse_json(hu_dag_t *dag, hu_allocator_t *alloc,
                              const char *json, size_t json_len);
void hu_dag_deinit(hu_dag_t *dag);

/* Query */
bool hu_dag_is_complete(const hu_dag_t *dag);
hu_dag_node_t *hu_dag_find_node(hu_dag_t *dag, const char *id, size_t id_len);

#endif
```

**Step 2:** Tests: `dag_add_node_and_find`, `dag_validate_detects_cycle`, `dag_validate_detects_missing_dep`, `dag_validate_detects_duplicate_id`, `dag_validate_accepts_valid_dag`, `dag_parse_json_creates_nodes`.

**Step 3:** Implement. Cycle detection: DFS with `visiting[]` and `visited[]` flags per node. Missing dep: for each dep ID, verify it exists in the node list. Parse JSON: `{"tasks":[{"id":"t1","tool":"web_search","args":{},"deps":["t0"]},...]}`.

**Step 4:** Tests pass. **Step 5:** Commit: `feat(agent): add DAG data structure with cycle detection for LLMCompiler`

---

### Task 17: Topological Sort and Batch Builder

Group DAG nodes into execution batches — each batch contains nodes whose dependencies are all resolved.

**Files:**

- Create: `include/human/agent/dag_executor.h`
- Create: `src/agent/dag_executor.c`
- Test: `tests/test_dag_executor.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef HU_DAG_EXECUTOR_H
#define HU_DAG_EXECUTOR_H

#include "human/agent/dag.h"
#include "human/tool.h"

#define HU_DAG_MAX_BATCH_SIZE 8

typedef struct hu_dag_batch {
    hu_dag_node_t *nodes[HU_DAG_MAX_BATCH_SIZE];
    size_t count;
} hu_dag_batch_t;

/* Get next batch of ready-to-execute nodes */
hu_error_t hu_dag_next_batch(hu_dag_t *dag, hu_dag_batch_t *batch);

/* Resolve $tN variable references in args using completed node results */
hu_error_t hu_dag_resolve_vars(hu_allocator_t *alloc, const hu_dag_t *dag,
                                const char *args, size_t args_len,
                                char **resolved, size_t *resolved_len);

/* Execute a full DAG using tool dispatch */
hu_error_t hu_dag_execute(hu_dag_t *dag, hu_allocator_t *alloc,
                           hu_tool_t *tools, size_t tools_count,
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

- Create: `include/human/agent/llm_compiler.h`
- Create: `src/agent/llm_compiler.c`
- Test: `tests/test_llm_compiler.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef HU_LLM_COMPILER_H
#define HU_LLM_COMPILER_H

#include "human/agent/dag.h"
#include "human/provider.h"
#include "human/tool.h"

/* Build the planning prompt for LLM DAG generation */
hu_error_t hu_llm_compiler_build_prompt(hu_allocator_t *alloc,
                                         const char *goal, size_t goal_len,
                                         hu_tool_t *tools, size_t tools_count,
                                         char **out, size_t *out_len);

/* Parse LLM response into a DAG */
hu_error_t hu_llm_compiler_parse_plan(hu_allocator_t *alloc,
                                       const char *response, size_t response_len,
                                       hu_dag_t *dag);

/* Full pipeline: generate plan via LLM, validate, execute */
hu_error_t hu_llm_compiler_run(hu_allocator_t *alloc,
                                hu_provider_t *provider, const char *model,
                                const char *goal, size_t goal_len,
                                hu_tool_t *tools, size_t tools_count,
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
- Modify: `include/human/agent.h` — add config flag for LLMCompiler
- Modify: `include/human/config.h` — add `llm_compiler_enabled` to agent config

**Steps:** Add `bool llm_compiler_enabled;` to `hu_agent_config_t`. In the agent turn loop, when the provider returns 3+ tool calls, check if `llm_compiler_enabled`. If so, instead of dispatching sequentially, build a DAG from the tool calls (treating each as an independent node unless the LLM specified dependencies via `$tN` in args), and use `hu_dag_execute` with `max_parallel=4`.

**Commit:** `feat(agent): integrate LLMCompiler parallel execution into agent turn`

---

## Layer 6: Proactive Scheduler

### Task 20: Time-Triggered Action System

A scheduler that fires actions based on time — check-ins, reminders, milestone awareness.

**Files:**

- Create: `include/human/agent/proactive.h`
- Create: `src/agent/proactive.c`
- Test: `tests/test_proactive.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef HU_PROACTIVE_H
#define HU_PROACTIVE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/agent/commitment.h"

#define HU_PROACTIVE_MAX_ACTIONS 32

typedef enum hu_proactive_action_type {
    HU_PROACTIVE_COMMITMENT_FOLLOW_UP,
    HU_PROACTIVE_MILESTONE,
    HU_PROACTIVE_CHECK_IN,
    HU_PROACTIVE_MORNING_BRIEFING,
    HU_PROACTIVE_PATTERN_INSIGHT,
} hu_proactive_action_type_t;

typedef struct hu_proactive_action {
    hu_proactive_action_type_t type;
    char *message;       /* owned — the proactive message/question to inject */
    size_t message_len;
    char *context;       /* owned — supporting context for the LLM */
    size_t context_len;
    double priority;     /* 0.0–1.0 */
} hu_proactive_action_t;

typedef struct hu_proactive_result {
    hu_proactive_action_t actions[HU_PROACTIVE_MAX_ACTIONS];
    size_t count;
} hu_proactive_result_t;

/* Check for due actions given current time and memory state */
hu_error_t hu_proactive_check(hu_allocator_t *alloc, hu_memory_t *memory,
                               const char *current_time, size_t current_time_len,
                               hu_proactive_result_t *out);

/* Build context string for prompt injection (top N by priority) */
hu_error_t hu_proactive_build_context(const hu_proactive_result_t *result,
                                       hu_allocator_t *alloc, size_t max_actions,
                                       char **out, size_t *out_len);

void hu_proactive_result_deinit(hu_proactive_result_t *result, hu_allocator_t *alloc);

#endif
```

**Step 2:** Tests: `proactive_detects_due_commitment`, `proactive_generates_milestone_at_anniversary`, `proactive_morning_briefing_includes_commitments`, `proactive_build_context_sorts_by_priority`.

**Step 3:** Implement. `hu_proactive_check`: query commitment store for due follow-ups, check memory for milestones (first_interaction anniversary, session count milestones at 10/25/50/100), check time for morning briefing window (8-10am). Generate action for each.

**Step 4:** Tests pass. **Step 5:** Commit: `feat(agent): add proactive scheduler for time-triggered actions`

---

### Task 21: Proactive Context Integration

Wire proactive actions into the agent turn.

**Files:**

- Modify: `src/agent/agent_turn.c` — run proactive check at turn start, inject context
- Modify: `include/human/agent/prompt.h` — add `proactive_context`
- Modify: `src/agent/prompt.c` — include proactive context

**Steps:** At the start of the agent turn (before the main loop), call `hu_proactive_check` with current time. If actions exist, call `hu_proactive_build_context` and inject into system prompt under `### Proactive Awareness`. The LLM decides whether and how to surface these naturally.

**Commit:** `feat(agent): integrate proactive awareness into agent turn`

---

## Layer 7: Superhuman Services

### Task 22: Superhuman Service Registry

A registry for superhuman services that build specialized context from memory.

**Files:**

- Create: `include/human/agent/superhuman.h`
- Create: `src/agent/superhuman.c`
- Test: `tests/test_superhuman.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef HU_SUPERHUMAN_H
#define HU_SUPERHUMAN_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/memory/stm.h"

#define HU_SUPERHUMAN_MAX_SERVICES 16

typedef struct hu_superhuman_service {
    const char *name;
    /* Build context string for this service given memory + STM state */
    hu_error_t (*build_context)(void *ctx, hu_allocator_t *alloc, hu_memory_t *memory,
                                 const hu_stm_buffer_t *stm,
                                 char **out, size_t *out_len);
    /* Record an observation from the current turn */
    hu_error_t (*observe)(void *ctx, hu_allocator_t *alloc, hu_memory_t *memory,
                           const char *text, size_t text_len,
                           const char *role, size_t role_len);
    void *ctx;
} hu_superhuman_service_t;

typedef struct hu_superhuman_registry {
    hu_superhuman_service_t services[HU_SUPERHUMAN_MAX_SERVICES];
    size_t count;
} hu_superhuman_registry_t;

hu_error_t hu_superhuman_registry_init(hu_superhuman_registry_t *reg);
hu_error_t hu_superhuman_register(hu_superhuman_registry_t *reg,
                                   hu_superhuman_service_t service);

/* Build unified context from all registered services */
hu_error_t hu_superhuman_build_context(hu_superhuman_registry_t *reg,
                                        hu_allocator_t *alloc, hu_memory_t *memory,
                                        const hu_stm_buffer_t *stm,
                                        char **out, size_t *out_len);

/* Run all observe hooks for a turn */
hu_error_t hu_superhuman_observe_all(hu_superhuman_registry_t *reg,
                                      hu_allocator_t *alloc, hu_memory_t *memory,
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

**Step 1:** Implement `hu_superhuman_service_t` for commitment keeping. `build_context`: query active commitments, format as: "You made {count} active commitments. [list]. {follow_up_due} are due for follow-up." `observe`: run `hu_commitment_detect` on text, store results.

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

- Modify: `include/human/agent.h` — add `hu_superhuman_registry_t superhuman` to agent
- Modify: `src/agent/agent.c` — init registry, register services
- Modify: `src/agent/agent_turn.c` — call observe + build_context
- Modify: `include/human/agent/prompt.h` — add `superhuman_context`
- Modify: `src/agent/prompt.c` — include superhuman context

**Steps:** Init registry in `hu_agent_init`. Register commitment keeper, predictive coaching, emotional first aid, silence interpreter. In agent_turn: after processing user message, call `hu_superhuman_observe_all`. Before prompt build, call `hu_superhuman_build_context`. Inject into system prompt under `### Superhuman Insights`.

**Commit:** `feat(agent): integrate superhuman service registry into agent turn`

---

## Layer 8: Semantic Tool Routing (Bonus)

### Task 28: Tool Relevance Scorer

Score tool relevance to the current conversation context, so only the most relevant tools are sent to the LLM.

**Files:**

- Create: `include/human/agent/tool_router.h`
- Create: `src/agent/tool_router.c`
- Test: `tests/test_tool_router.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

**Step 1: Write the header**

```c
#ifndef HU_TOOL_ROUTER_H
#define HU_TOOL_ROUTER_H

#include "human/core/allocator.h"
#include "human/tool.h"

#define HU_TOOL_ROUTER_MAX_SELECTED 15
#define HU_TOOL_ROUTER_ALWAYS_MAX    8

typedef struct hu_tool_router {
    hu_allocator_t alloc;
    const char **always_tools;      /* tool names always included */
    size_t always_tools_count;
} hu_tool_router_t;

typedef struct hu_tool_selection {
    hu_tool_t *tools[HU_TOOL_ROUTER_MAX_SELECTED];
    size_t count;
} hu_tool_selection_t;

hu_error_t hu_tool_router_init(hu_tool_router_t *router, hu_allocator_t alloc);

/* Select most relevant tools for the given message */
hu_error_t hu_tool_router_select(hu_tool_router_t *router,
                                  const char *message, size_t message_len,
                                  hu_tool_t *all_tools, size_t all_tools_count,
                                  hu_tool_selection_t *out);

void hu_tool_router_deinit(hu_tool_router_t *router);

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
- Modify: `include/human/agent.h` — add tool_router to agent

**Steps:** Before the provider `chat()` call in the main loop, run `hu_tool_router_select` to get a filtered tool set. Pass the filtered tools to the provider instead of all tools. This reduces token usage and improves tool selection quality.

**Commit:** `feat(agent): integrate semantic tool routing into agent turn`

---

## Validation

After each layer:

```bash
cmake --build build -j$(sysctl -n hw.ncpu) && ./build/human_tests
```

All tests must pass with 0 ASan errors.

After all layers:

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON \
  -DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_SQLITE=ON -DSC_ENABLE_PERSONA=ON
cmake --build build-release -j$(sysctl -n hw.ncpu)
ls -la build-release/human  # verify binary size still reasonable
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
