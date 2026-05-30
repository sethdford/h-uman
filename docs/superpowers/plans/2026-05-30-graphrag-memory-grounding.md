---
title: GraphRAG Memory-Grounding — Increment 1 Implementation Plan
date: 2026-05-30
status: ready
spec: docs/superpowers/specs/2026-05-30-graphrag-memory-grounding-design.md
branch: feat/graphrag-grounding
---

# GraphRAG Memory-Grounding — Increment 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Inject each contact's pre-computed `community_summaries` (the "who they are" graph signal) into the local-model system prompt, behind a 3-state `HU_GRAPH_GROUNDING` flag that defaults to a no-op.

**Architecture:** A new `hu_graph_ground_load()` reads the top-N community summaries for the contact from SQLite and returns a markdown blob. That blob is carried in a new `hu_prompt_config_t.graph_context` field, appended by `prompt.c` under a "Relationship Context" header, and registered as a first-class budgeted prompt field. The call sites in `agent_stream.c` / `agent_turn.c` invoke it only when the flag is `shadow` or `on`; `off` (default) skips it so the prompt is byte-identical to today.

**Tech Stack:** C11, CMake (`--preset dev`, ASan), SQLite (`HU_ENABLE_SQLITE`), the in-repo test harness (`HU_TEST_SUITE` / `HU_RUN_TEST` / `HU_ASSERT_*`, runner `./build/human_tests`).

**Scope note:** This plan is Increment 1 only. Increment 2 (promote + fuse the W12 graph traversal into the hot retrieval path) gets its own plan *after* Increment 1's prompt-cache and latency impact is measured against the live server — per spec risks R2/R3.

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `include/human/agent/graph_grounding.h` | Create | Public API: `hu_graph_grounding_mode_t`, `hu_graph_grounding_mode()`, `hu_graph_ground_load()`. |
| `src/agent/graph_grounding.c` | Create | Flag parsing; community-summary SELECT; markdown assembly; fail-open. |
| `include/human/agent/prompt.h` | Modify | Add `graph_context` / `graph_context_len` to `hu_prompt_config_t`. |
| `include/human/agent/prompt_budget.h` | Modify | Add `HU_PROMPT_FIELD_GRAPH_CONTEXT`; bump `HU_PROMPT_FIELD_COUNT` 27→28. |
| `src/agent/prompt_budget.c` | Modify | Add `[HU_PROMPT_FIELD_GRAPH_CONTEXT] = "graph_context"` to `s_field_names`. |
| `src/agent/prompt.c` | Modify | Append `graph_context` under a "Relationship Context" header (mirror `contact_context` at prompt.c:337). |
| `src/agent/agent_stream.c` | Modify | After memory load (~:1187), set `.graph_context` when flag ≠ off. |
| `src/agent/agent_turn.c` | Modify | After memory load (~:1465), set `cfg.graph_context` when flag ≠ off. |
| `tests/test_prompt.c` | Modify | New prompt-field tests; register in `run_prompt_tests()`. |
| `tests/test_graph_grounding.c` | Create | Unit tests for flag parsing + `hu_graph_ground_load` (seeded in-memory SQLite). |
| `tests/test_main.c` | Modify | Declare + call `run_graph_grounding_tests()`. |
| `CMakeLists.txt` | Modify | Add `src/agent/graph_grounding.c` to core sources; add `tests/test_graph_grounding.c` to the test target. |

---

## Task 1: Prompt-field plumbing (`graph_context`)

Add the field, register it in the budget system, and append it in the prompt under a header. No retrieval yet — driven directly by a config field in tests.

**Files:**
- Modify: `include/human/agent/prompt.h` (after `memory_context_len`, ~line 27)
- Modify: `include/human/agent/prompt_budget.h:27,31` (enum + count)
- Modify: `src/agent/prompt_budget.c:40` (names array)
- Modify: `src/agent/prompt.c:337` (append block, mirror `contact_context`)
- Test: `tests/test_prompt.c`

- [ ] **Step 1: Write the failing tests** — add to `tests/test_prompt.c` (before `run_prompt_tests`):

```c
static void test_prompt_graph_context_present(void) {
    hu_allocator_t alloc = hu_default_allocator();
    char *out = NULL; size_t out_len = 0;
    hu_prompt_config_t cfg = {
        .provider_name = "ollama", .provider_name_len = 6,
        .model_name = "llama3", .model_name_len = 6,
        .autonomy_level = 1,
        .graph_context = "Climbing partner since 2019; talks in short bursts.",
        .graph_context_len = 50,
    };
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Relationship Context") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Climbing partner since 2019") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_prompt_graph_context_absent_is_noop(void) {
    hu_allocator_t alloc = hu_default_allocator();
    char *out = NULL; size_t out_len = 0;
    hu_prompt_config_t cfg = {
        .provider_name = "ollama", .provider_name_len = 6,
        .model_name = "llama3", .model_name_len = 6,
        .autonomy_level = 1,
        /* graph_context deliberately unset */
    };
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(out, "Relationship Context") == NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}
```

Register them inside `run_prompt_tests()` (after the existing `HU_RUN_TEST(test_prompt_build_with_memory);` line):

```c
    HU_RUN_TEST(test_prompt_graph_context_present);
    HU_RUN_TEST(test_prompt_graph_context_absent_is_noop);
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build --preset dev 2>&1 | tail -20`
Expected: compile error — `hu_prompt_config_t has no member named 'graph_context'`.

- [ ] **Step 3: Implement the field + budget registration + append**

In `include/human/agent/prompt.h`, immediately after `size_t memory_context_len;`:

```c
    const char *graph_context; /* GraphRAG: per-contact community summaries */
    size_t graph_context_len;
```

In `include/human/agent/prompt_budget.h`: add the enum entry after `HU_PROMPT_FIELD_VOICE_MATURITY_DIRECTIVE,` and bump the count:

```c
    HU_PROMPT_FIELD_GRAPH_CONTEXT,
```
```c
#define HU_PROMPT_FIELD_COUNT 28   /* was 27 */
```

In `src/agent/prompt_budget.c`, inside the `s_field_names` initializer (after the last entry):

```c
    [HU_PROMPT_FIELD_GRAPH_CONTEXT] = "graph_context",
```

In `src/agent/prompt.c`, mirror the `contact_context` block at line 337 (place the graph block immediately before the contact block so relationship context sits near identity):

```c
    if (config->graph_context && config->graph_context_len > 0) {
        err = append(alloc, &buf, &len, &cap, "\n## Relationship Context\n", 25);
        if (err != HU_OK)
            goto fail;
        err = append(alloc, &buf, &len, &cap, config->graph_context,
                     config->graph_context_len);
        if (err != HU_OK)
            goto fail;
        err = append(alloc, &buf, &len, &cap, "\n\n", 2);
        if (err != HU_OK)
            goto fail;
    }
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build --preset dev && ./build/human_tests --filter=graph_context -v`
Expected: PASS for `test_prompt_graph_context_present` and `test_prompt_graph_context_absent_is_noop`.

- [ ] **Step 5: Commit**

```bash
git -C /Users/sethford/Projects/h-uman-graphrag add include/human/agent/prompt.h include/human/agent/prompt_budget.h src/agent/prompt_budget.c src/agent/prompt.c tests/test_prompt.c
git -C /Users/sethford/Projects/h-uman-graphrag commit -m "feat(prompt): add graph_context field for GraphRAG grounding"
```

---

## Task 2: `hu_graph_ground_load` + community-summary read

Read the contact's top-N community summaries from SQLite and assemble a markdown blob. Fail-open on every error.

**Files:**
- Create: `include/human/agent/graph_grounding.h`
- Create: `src/agent/graph_grounding.c`
- Create: `tests/test_graph_grounding.c`
- Modify: `tests/test_main.c` (declare + call runner)
- Modify: `CMakeLists.txt` (add both new .c files)

- [ ] **Step 1: Write the header**

`include/human/agent/graph_grounding.h`:

```c
#ifndef HU_AGENT_GRAPH_GROUNDING_H
#define HU_AGENT_GRAPH_GROUNDING_H

#include "human/agent/memory_loader.h"
#include "human/error.h"
#include "human/memory_alloc.h"
#include <stddef.h>

typedef enum hu_graph_grounding_mode {
    HU_GRAPH_GROUNDING_OFF = 0,
    HU_GRAPH_GROUNDING_SHADOW,
    HU_GRAPH_GROUNDING_ON,
} hu_graph_grounding_mode_t;

/* Reads HU_GRAPH_GROUNDING: unset/"off"/"0" -> OFF, "shadow" -> SHADOW,
 * "on"/"1" -> ON. Unknown values -> OFF (fail-safe). */
hu_graph_grounding_mode_t hu_graph_grounding_mode(void);

/* Best-effort: assembles markdown of the top community summaries for
 * `contact_id`. On any error/empty result, sets *out=NULL, *out_len=0 and
 * returns HU_OK. Caller frees *out via loader->alloc. `max_chars` caps output
 * (0 -> default 600). */
hu_error_t hu_graph_ground_load(hu_memory_loader_t *loader,
                                const char *contact_id, size_t contact_id_len,
                                size_t max_chars,
                                char **out, size_t *out_len);

#endif /* HU_AGENT_GRAPH_GROUNDING_H */
```

- [ ] **Step 2: Write the failing tests**

The seed data each SQLite-backed test needs (apply this SQL against the test's db handle via a small static helper that wraps the SQLite C string-exec API — table schema matches `src/agent/autodream.c:55`):

```sql
CREATE TABLE IF NOT EXISTS community_summaries (
  id INTEGER PRIMARY KEY AUTOINCREMENT, contact_id TEXT NOT NULL DEFAULT '',
  community_id INTEGER NOT NULL, summary_text TEXT NOT NULL,
  entity_count INTEGER NOT NULL DEFAULT 0, edge_count INTEGER NOT NULL DEFAULT 0,
  generated_at INTEGER NOT NULL, schema_version INTEGER NOT NULL DEFAULT 1);

INSERT INTO community_summaries (contact_id, community_id, summary_text, entity_count, edge_count, generated_at) VALUES
  ('alice', 1, 'Climbing partner since 2019.', 9, 12, 1),  -- highest entity+edge -> sorts first
  ('alice', 2, 'Talks in short bursts.',        3,  4, 1),
  ('bob',   1, 'Should not appear.',            9,  9, 1);  -- different contact -> excluded
```

`tests/test_graph_grounding.c`:

```c
#include "human/agent/graph_grounding.h"
#include "human/memory.h"
#include "test_harness.h"
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

/* Thin wrapper over the SQLite C string API; runs one SQL statement batch. */
static void seed_run(sqlite3 *db, const char *sql) {
    char *emsg = NULL;
    (void)sqlite3_exec(db, sql, NULL, NULL, &emsg);  /* test fixture; ignore errors */
    if (emsg) sqlite3_free(emsg);
}

static const char *kSeedSchema =
    "CREATE TABLE IF NOT EXISTS community_summaries ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT, contact_id TEXT NOT NULL DEFAULT '',"
    "community_id INTEGER NOT NULL, summary_text TEXT NOT NULL,"
    "entity_count INTEGER NOT NULL DEFAULT 0, edge_count INTEGER NOT NULL DEFAULT 0,"
    "generated_at INTEGER NOT NULL, schema_version INTEGER NOT NULL DEFAULT 1)";
static const char *kSeedRows =
    "INSERT INTO community_summaries (contact_id, community_id, summary_text, entity_count, edge_count, generated_at) VALUES"
    "('alice', 1, 'Climbing partner since 2019.', 9, 12, 1),"
    "('alice', 2, 'Talks in short bursts.',        3,  4, 1),"
    "('bob',   1, 'Should not appear.',            9,  9, 1)";

static void test_graph_ground_load_returns_contact_summaries(void) {
    hu_memory_t *mem = NULL;
    HU_ASSERT_EQ(hu_sqlite_memory_create(NULL, ":memory:", &mem), HU_OK);
    sqlite3 *db = hu_sqlite_memory_get_db(mem);
    seed_run(db, kSeedSchema);
    seed_run(db, kSeedRows);

    hu_memory_loader_t loader;
    hu_memory_loader_init(&loader, NULL, mem, NULL, 10, 4000);

    char *out = NULL; size_t out_len = 0;
    HU_ASSERT_EQ(hu_graph_ground_load(&loader, "alice", 5, 0, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Climbing partner since 2019") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Talks in short bursts") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Should not appear") == NULL);
    HU_ASSERT_TRUE(strstr(out, "Climbing") < strstr(out, "short bursts")); /* order by count */
    loader.alloc->free(loader.alloc->ctx, out, out_len + 1);
    hu_memory_destroy(mem);
}

static void test_graph_ground_load_empty_is_failopen(void) {
    hu_memory_t *mem = NULL;
    HU_ASSERT_EQ(hu_sqlite_memory_create(NULL, ":memory:", &mem), HU_OK);
    /* no community_summaries table at all */
    hu_memory_loader_t loader;
    hu_memory_loader_init(&loader, NULL, mem, NULL, 10, 4000);
    char *out = (char *)0x1; size_t out_len = 99;
    HU_ASSERT_EQ(hu_graph_ground_load(&loader, "nobody", 6, 0, &out, &out_len), HU_OK);
    HU_ASSERT_TRUE(out == NULL);
    HU_ASSERT_EQ((int)out_len, 0);
    hu_memory_destroy(mem);
}
#endif /* HU_ENABLE_SQLITE */

static void test_graph_grounding_mode_parse(void) {
    unsetenv("HU_GRAPH_GROUNDING");
    HU_ASSERT_EQ((int)hu_graph_grounding_mode(), (int)HU_GRAPH_GROUNDING_OFF);
    setenv("HU_GRAPH_GROUNDING", "shadow", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_mode(), (int)HU_GRAPH_GROUNDING_SHADOW);
    setenv("HU_GRAPH_GROUNDING", "on", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_mode(), (int)HU_GRAPH_GROUNDING_ON);
    setenv("HU_GRAPH_GROUNDING", "garbage", 1);
    HU_ASSERT_EQ((int)hu_graph_grounding_mode(), (int)HU_GRAPH_GROUNDING_OFF);
    unsetenv("HU_GRAPH_GROUNDING");
}

void run_graph_grounding_tests(void) {
    HU_TEST_SUITE("GraphRAG grounding");
    HU_RUN_TEST(test_graph_grounding_mode_parse);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_graph_ground_load_returns_contact_summaries);
    HU_RUN_TEST(test_graph_ground_load_empty_is_failopen);
#endif
}
```

> Note: confirm the in-memory constructor + destroy names against `tests/test_world_model_graph.c` (a fixture that already builds an in-memory `hu_memory_t`). If they differ from `hu_sqlite_memory_create(NULL, ":memory:", &mem)` / `hu_memory_destroy(mem)`, match that file verbatim — the constructor is mechanical; the assertions are the load-bearing part.

In `tests/test_main.c`: add the declaration near the other `run_*` decls (~line 192) and the call near `run_prompt_tests();` (~line 1166):

```c
void run_graph_grounding_tests(void);
```
```c
    run_graph_grounding_tests();
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build --preset dev 2>&1 | tail -20`
Expected: link/compile error — `undefined reference to hu_graph_ground_load` / `hu_graph_grounding_mode` (and the new test file is not compiled until CMake is updated in Step 4).

- [ ] **Step 4: Implement `graph_grounding.c` + wire CMake**

`src/agent/graph_grounding.c`:

```c
#include "human/agent/graph_grounding.h"
#include "human/memory.h"
#include <stdlib.h>
#include <string.h>

hu_graph_grounding_mode_t hu_graph_grounding_mode(void) {
    const char *v = getenv("HU_GRAPH_GROUNDING");
    if (!v || !*v) return HU_GRAPH_GROUNDING_OFF;
    if (strcmp(v, "shadow") == 0) return HU_GRAPH_GROUNDING_SHADOW;
    if (strcmp(v, "on") == 0 || strcmp(v, "1") == 0) return HU_GRAPH_GROUNDING_ON;
    return HU_GRAPH_GROUNDING_OFF;
}

hu_error_t hu_graph_ground_load(hu_memory_loader_t *loader,
                                const char *contact_id, size_t contact_id_len,
                                size_t max_chars,
                                char **out, size_t *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!loader || !out || !out_len || !contact_id || contact_id_len == 0)
        return HU_OK; /* fail-open */
    if (max_chars == 0) max_chars = 600;
#ifdef HU_ENABLE_SQLITE
    sqlite3 *db = loader->memory ? hu_sqlite_memory_get_db(loader->memory) : NULL;
    if (!db) return HU_OK;

    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT summary_text FROM community_summaries WHERE contact_id = ?1 "
        "ORDER BY (entity_count + edge_count) DESC, generated_at DESC LIMIT 3";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_OK; /* table missing or other error -> fail-open */
    sqlite3_bind_text(st, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);

    char *buf = loader->alloc->alloc(loader->alloc->ctx, max_chars + 1);
    if (!buf) { sqlite3_finalize(st); return HU_OK; }
    size_t pos = 0;
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *s = sqlite3_column_text(st, 0);
        if (!s) continue;
        size_t slen = strlen((const char *)s);
        const char *sep = (n == 0) ? "- " : "\n- ";
        size_t seplen = strlen(sep);
        if (pos + seplen + slen >= max_chars) break;
        memcpy(buf + pos, sep, seplen); pos += seplen;
        memcpy(buf + pos, s, slen); pos += slen;
        n++;
    }
    sqlite3_finalize(st);
    if (n == 0) { loader->alloc->free(loader->alloc->ctx, buf, max_chars + 1); return HU_OK; }
    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
#endif
    return HU_OK;
}
```

In `CMakeLists.txt`: find the source list that already contains `src/agent/prompt.c` and add `src/agent/graph_grounding.c` next to it; find the test target's source list (contains `tests/test_prompt.c`) and add `tests/test_graph_grounding.c`.

```bash
grep -n "src/agent/prompt.c" /Users/sethford/Projects/h-uman-graphrag/CMakeLists.txt
grep -n "tests/test_prompt.c" /Users/sethford/Projects/h-uman-graphrag/CMakeLists.txt
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build --preset dev && ./build/human_tests --suite="GraphRAG grounding" -v`
Expected: PASS for all three tests (two when SQLite is off).

- [ ] **Step 6: Commit**

```bash
git -C /Users/sethford/Projects/h-uman-graphrag add include/human/agent/graph_grounding.h src/agent/graph_grounding.c tests/test_graph_grounding.c tests/test_main.c CMakeLists.txt
git -C /Users/sethford/Projects/h-uman-graphrag commit -m "feat(memory): hu_graph_ground_load reads contact community summaries"
```

---

## Task 3: Wire the call sites behind the flag

Invoke `hu_graph_ground_load` after the existing memory load and set `graph_context` only when the flag is `shadow` (compute+log) or `on` (inject). `off` skips entirely.

**Files:**
- Modify: `src/agent/agent_stream.c` (~:1187, the config initializer + before it)
- Modify: `src/agent/agent_turn.c` (~:1465, after `hu_memory_loader_load`)

- [ ] **Step 1: Implement in `agent_stream.c`** — before the `hu_prompt_config_t` designated initializer (which starts ~:1180), add:

```c
        char *graph_ctx = NULL; size_t graph_ctx_len = 0;
        hu_graph_grounding_mode_t gmode = hu_graph_grounding_mode();
        if (gmode != HU_GRAPH_GROUNDING_OFF && agent->memory_session_id) {
            hu_graph_ground_load(&loader, agent->memory_session_id,
                                 agent->memory_session_id_len, 0,
                                 &graph_ctx, &graph_ctx_len);
            if (gmode == HU_GRAPH_GROUNDING_SHADOW) {
                hu_log_info("graph_grounding", NULL,
                            "shadow: %zu graph_context bytes (not injected)", graph_ctx_len);
                if (graph_ctx) agent->alloc->free(agent->alloc->ctx, graph_ctx, graph_ctx_len + 1);
                graph_ctx = NULL; graph_ctx_len = 0;
            }
        }
```

Then inside the initializer, next to `.memory_context = memory_ctx,`:

```c
            .graph_context = graph_ctx,
            .graph_context_len = graph_ctx_len,
```

Add `#include "human/agent/graph_grounding.h"` to the includes at the top of `agent_stream.c`. Free `graph_ctx` wherever `memory_ctx` is freed after the turn (match the existing `memory_ctx` cleanup).

- [ ] **Step 2: Implement in `agent_turn.c`** — after the `hu_memory_loader_load(...)` call (~:1461) and its error log, add the identical block (using whatever the local config struct variable is named; set its `graph_context` / `graph_context_len`). Add the same include. Free alongside `memory_ctx`.

- [ ] **Step 3: Build**

Run: `cmake --build --preset dev 2>&1 | tail -20`
Expected: clean build (no warnings-as-errors).

- [ ] **Step 4: Commit**

```bash
git -C /Users/sethford/Projects/h-uman-graphrag add src/agent/agent_stream.c src/agent/agent_turn.c
git -C /Users/sethford/Projects/h-uman-graphrag commit -m "feat(agent): inject graph_context when HU_GRAPH_GROUNDING != off"
```

---

## Task 4: Full-suite verification + off-is-noop golden

- [ ] **Step 1: Run the full suite (flag unset = off)**

Run: `./build/human_tests 2>&1 | tail -30`
Expected: all suites pass (full suite per quality-gates, not changed-files only). The default-off path must not regress any existing prompt test.

- [ ] **Step 2: Spot-check off is byte-identical** — confirm no existing prompt golden changed and the absent-context test passes:

Run: `./build/human_tests --filter=graph_context -v && ./build/human_tests --suite="Prompt and memory loader" -v`
Expected: PASS; `test_prompt_graph_context_absent_is_noop` proves the no-op.

- [ ] **Step 3: `/verify`** — spawn the verifier agent to run the build + suite and capture `RESULT_verifier=PASS` evidence (per the project loop; reading is not verification).

- [ ] **Step 4: Branch ready for review**

```bash
git -C /Users/sethford/Projects/h-uman-graphrag log --oneline origin/main..HEAD
```
Expected: four feature commits on `feat/graphrag-grounding`, full suite green, flag default-off.

---

## Self-Review (against the spec)

- **Spec §4.1 `hu_graph_ground_load`** → Task 2. ✅ (community-summary read; W12 traversal is Increment 2, explicitly out of scope.)
- **Spec §4.2 new field + budget registration** → Task 1. ✅ (enum + count + names array all updated — the `HU_PROMPT_FIELD_COUNT` bump is the gotcha, covered.)
- **Spec §4.3 prompt placement near identity** → Task 1 Step 3 (graph block before `contact_context`). ✅
- **Spec §4.4 call-site wiring** → Task 3 (both stream + turn). ✅
- **Spec §3 three-state flag** → Task 2 (`hu_graph_grounding_mode`) + Task 3 (off/shadow/on behavior). ✅
- **Spec §7 fail-open** → Task 2 (`test_graph_ground_load_empty_is_failopen`, every error path returns HU_OK with NULL). ✅
- **Spec §8 test plan** → off-identical (Task 1 + Task 4 Step 2), summaries read/ordering/empty (Task 2), shadow logs-not-injects (Task 3 Step 1). ✅
- **Spec §10 success criteria** → Task 4 (full suite + `/verify`). ✅
- **Out of scope (deferred to Increment 2 plan):** budget-split merge with flat RAG, W12 promotion, latency/cache measurement. Noted in the scope note. ✅
