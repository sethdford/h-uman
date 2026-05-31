# Reflection Loop Phase 1 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a periodic reflection pass that distills accumulated conversations into typed, queryable patterns stored in SQLite, with cloud Gemini 3.1 Pro as default provider and a shadow-mode hook for future local-Gemma ratchet.

**Architecture:** New module `src/reflection/` integrated as in-daemon tick (Approach A from brainstorm). Hybrid idle+daily-floor trigger. Output is structured `hu_reflection_pattern_t` rows + per-run JSON dump. Two tiered consumers (system prompt builder, init_proposer). Four-layer failure handling. Phase 2 belief-update gating deferred but predicate `hu_reflection_pattern_has_quorum()` shipped now for testability.

**Tech Stack:** C11, SQLite (gated on `HU_ENABLE_SQLITE`), JSON1 extension for channel queries, existing `hu_provider_t` vtable for LLM calls, existing `hu_log_info_once` for operator-visible disabled-logs.

**Spec:** [`design.md`](./design.md). Acceptance criteria AC-1 through AC-7 listed there.

---

## File structure (locked from spec)

| File | Responsibility | Status |
|---|---|---|
| `include/human/reflection.h` | Public API (`hu_reflection_tick`, `hu_reflection_run`, query helpers, pattern struct, quorum predicate) | NEW |
| `src/reflection/reflection.c` | Tick gating, run orchestration, idle detection | NEW |
| `src/reflection/prompt.c` | Input transcript assembly, system prompt template loader | NEW |
| `src/reflection/schema.c` | JSON parse/validate, stable-id hashing, confidence floor | NEW |
| `src/reflection/storage.c` | SQLite migrations, UPSERT, queries | NEW |
| `src/reflection/consumer.c` | System-prompt slice query, unsurfaced-query, surfaced/retired mutators | NEW |
| `src/reflection/reflection_system_prompt.txt` | The reflection prompt template (separate file for tuning) | NEW |
| `include/human/config.h` | Add `hu_reflection_config_t` to root config | MODIFY |
| `src/config/config_parse.c` | Parse `reflection: {...}` JSON block | MODIFY |
| `src/agent/personal_model.c` | Append reflection slice to `hu_personal_model_build_prompt` output | MODIFY |
| `src/agent/init_proposer.c` | Add reflection-unsurfaced as candidate source | MODIFY |
| `src/daemon.c` | Register `hu_reflection_tick` in main loop | MODIFY |
| `CMakeLists.txt` | Add new sources gated on `HU_ENABLE_SQLITE` | MODIFY |
| `tests/test_reflection_schema.c` | Unit tests for schema.c | NEW |
| `tests/test_reflection_storage.c` | Unit tests for storage.c (in-memory SQLite) | NEW |
| `tests/test_reflection_consumer.c` | Unit tests for consumer.c | NEW |
| `tests/test_reflection_quorum.c` | Phase 2 contract: predicate works, NO mutation in Phase 1 | NEW |
| `tests/test_reflection_e2e.c` | End-to-end with mock provider | NEW |
| `tests/test_main.c` | Register new test runners | MODIFY |
| `scripts/check-reflection-quorum-not-wired.sh` | CI gate: ensures no Phase 1 caller mutates personal_model on quorum | NEW |
| `scripts/eval_reflection_shadow.py` | Sprint 2 shadow-mode eval (Jaccard, critical-miss, calibration) | NEW (Sprint 2) |

---

## Task ordering rationale

Tasks 1-2 (schema + storage) have no h-uman dependencies → foundation.
Task 3 (config) is small, parallel-safe → unblocks 4-5.
Task 4 (prompt + provider call) depends on schema (output parsing).
Task 5 (reflection.c orchestration) depends on 1-4.
Task 6 (consumer queries) depends on 2.
Task 7 (system-prompt integration) depends on 6.
Task 8 (init_proposer integration) depends on 6.
Task 9 (daemon wiring) depends on 5.
Task 10 (e2e tests) depends on 9.
Task 11 (quorum predicate + CI gate) is wiring; can happen after 2.
Task 12 (operator health + acceptance) closes the sprint.

Sprint 2 (US-10..US-13 in spec): shadow mode + eval — separate work item, deferred.

---

## Task 1: Public header + schema parser + stable id

**STATUS: DONE 2026-05-27** — commit `7ed1d482` on `origin/main`.
17 tests pass, 0 ASan errors, header verified standalone-compilable,
all steps below collapsed into a single landing because the schema
walker, stable-id hashing, and tests were all ready at once.

**Files:**
- Create: `include/human/reflection.h`
- Create: `src/reflection/schema.c`
- Create: `tests/test_reflection_schema.c`
- Modify: `tests/test_main.c` (register runner)
- Modify: `CMakeLists.txt` (add sources behind `HU_ENABLE_SQLITE`)

- [x] **Step 1.1: Write the public header**

Create `include/human/reflection.h` with the full struct and API from the spec's "Components → include/human/reflection.h" section. Reproduce the entire struct definition and function prototypes verbatim from [design.md](./design.md#includehumanreflectionh).

After writing, verify the header compiles standalone:
```
cc -std=c11 -Wall -Wextra -Wpedantic -Iinclude -c -x c include/human/reflection.h -o /dev/null
```
Expected: clean compile, no warnings.

- [x] **Step 1.2: Write the failing schema test (malformed JSON rejected)**

Add to `tests/test_reflection_schema.c`:

```c
#include "human/reflection.h"
#include "test_framework.h"
#include <string.h>

static void test_schema_rejects_malformed_json(void) {
    hu_reflection_pattern_t *patterns = NULL;
    int count = 0;
    char *prose = NULL;
    char *error = NULL;
    hu_error_t err = hu_reflection_parse(
        "{not valid json", &patterns, &count, &prose, &error);
    HU_ASSERT_NE(err, HU_OK);
    HU_ASSERT_EQ(patterns, NULL);
    HU_ASSERT_EQ(count, 0);
    HU_ASSERT_NE(error, NULL);
    free(error);
}

void run_reflection_schema_tests(void) {
    HU_TEST_SUITE("reflection_schema");
    HU_RUN_TEST(test_schema_rejects_malformed_json);
    /* more tests added in steps below */
}
```

Register in `tests/test_main.c`:
```c
#ifdef HU_ENABLE_SQLITE
void run_reflection_schema_tests(void);
#endif
/* ... in main(): */
#ifdef HU_ENABLE_SQLITE
    run_reflection_schema_tests();
#endif
```

- [x] **Step 1.3: Run to verify it fails**

```
cmake --build --preset dev --target human_tests 2>&1 | tail -20
```
Expected: undefined reference to `hu_reflection_parse` — confirms test wired correctly.

- [x] **Step 1.4: Implement minimal `hu_reflection_parse` to reject malformed**

In `src/reflection/schema.c`:

```c
#include "human/reflection.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include <stdlib.h>
#include <string.h>

hu_error_t hu_reflection_parse(
    const char *json,
    hu_reflection_pattern_t **out_patterns,
    int *out_count,
    char **out_prose_summary,
    char **out_error)
{
    if (!json || !out_patterns || !out_count) return HU_ERR_INVALID_ARG;
    *out_patterns = NULL;
    *out_count = 0;
    if (out_prose_summary) *out_prose_summary = NULL;

    hu_json_value_t *root = hu_json_parse(json, NULL);
    if (!root) {
        if (out_error) *out_error = strdup("JSON parse failed");
        return HU_ERR_PARSE;
    }
    /* TODO in step 1.6+: walk root, build patterns */
    hu_json_free(root);
    return HU_OK;
}
```

- [x] **Step 1.5: Run to verify malformed-JSON test passes**

```
./build/human_tests --filter=schema_rejects_malformed_json
```
Expected: PASS.

- [x] **Step 1.6: Add tests for required-field validation and confidence range**

Append to `tests/test_reflection_schema.c`:

```c
static void test_schema_requires_pattern_type(void) {
    const char *json =
      "{\"patterns\":[{\"subject\":\"Seth\",\"observation\":\"x\","
      "\"confidence\":0.9}],\"summary\":\"\"}";
    hu_reflection_pattern_t *p = NULL; int n = 0;
    char *prose = NULL, *err = NULL;
    HU_ASSERT_NE(hu_reflection_parse(json, &p, &n, &prose, &err), HU_OK);
    free(err);
}

static void test_schema_rejects_confidence_out_of_range(void) {
    const char *json =
      "{\"patterns\":[{\"type\":\"preference\",\"subject\":\"Seth\","
      "\"observation\":\"x\",\"confidence\":1.5}],\"summary\":\"\"}";
    hu_reflection_pattern_t *p = NULL; int n = 0;
    char *prose = NULL, *err = NULL;
    HU_ASSERT_NE(hu_reflection_parse(json, &p, &n, &prose, &err), HU_OK);
    free(err);
}

static void test_schema_accepts_valid_pattern(void) {
    const char *json =
      "{\"patterns\":[{\"type\":\"preference\",\"subject\":\"Seth\","
      "\"observation\":\"prefers concise replies\",\"confidence\":0.8,"
      "\"evidence_ids\":[\"turn_1\"],\"channels\":[\"imessage\"]}],"
      "\"summary\":\"Seth seems to prefer concise responses.\"}";
    hu_reflection_pattern_t *p = NULL; int n = 0;
    char *prose = NULL, *err = NULL;
    HU_ASSERT_EQ(hu_reflection_parse(json, &p, &n, &prose, &err), HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_EQ(p[0].type, HU_REFLECTION_PATTERN_PREFERENCE);
    HU_ASSERT_STR_EQ(p[0].subject, "Seth");
    HU_ASSERT_TRUE(p[0].confidence > 0.79 && p[0].confidence < 0.81);
    HU_ASSERT_EQ(p[0].evidence_count, 1);
    HU_ASSERT_EQ(p[0].channel_count, 1);
    HU_ASSERT_STR_EQ(p[0].channels[0], "imessage");
    HU_ASSERT_NE(prose, NULL);
    free(prose);
    free(p);
}
```

Register them in `run_reflection_schema_tests()`.

- [x] **Step 1.7: Implement full schema walking**

Replace the TODO in `src/reflection/schema.c` with a full walker: iterate `patterns` array, parse `type` enum via string match against the 6 type names, validate `confidence` in [0,1], copy bounded fields with `strncpy` + manual null-terminate, parse `evidence_ids` and `channels` arrays up to 8 each (truncate with warning if more), compute stable id (see step 1.8).

Allocate `out_patterns` as `calloc(count, sizeof(hu_reflection_pattern_t))`. Copy `summary` to `*out_prose_summary` (caller frees).

- [x] **Step 1.8: Stable id hash test + implementation**

Test:
```c
static void test_stable_id_deterministic(void) {
    char id1[64], id2[64];
    hu_reflection_compute_id(HU_REFLECTION_PATTERN_PREFERENCE,
        "Seth", "prefers concise replies", id1, sizeof id1);
    hu_reflection_compute_id(HU_REFLECTION_PATTERN_PREFERENCE,
        "Seth", "prefers concise replies", id2, sizeof id2);
    HU_ASSERT_STR_EQ(id1, id2);
}

static void test_stable_id_differs_on_subject(void) {
    char id1[64], id2[64];
    hu_reflection_compute_id(HU_REFLECTION_PATTERN_PREFERENCE,
        "Seth", "prefers concise replies", id1, sizeof id1);
    hu_reflection_compute_id(HU_REFLECTION_PATTERN_PREFERENCE,
        "Sam", "prefers concise replies", id2, sizeof id2);
    HU_ASSERT_FALSE(strcmp(id1, id2) == 0);
}
```

Add `hu_reflection_compute_id` to header. Implement in schema.c using existing SHA-256 (search `grep -n "hu_sha256\|HU_SHA256" include/human/`); take SHA-256 of `type_str + "\0" + subject + "\0" + observation[0..128]`, hex-encode first 16 bytes (32 hex chars + null), write to `out_id`.

- [x] **Step 1.9: Confidence-floor flag test + implementation**

Test:
```c
static void test_schema_keeps_low_confidence_for_storage_drop(void) {
    const char *json =
      "{\"patterns\":[{\"type\":\"preference\",\"subject\":\"Seth\","
      "\"observation\":\"x\",\"confidence\":0.3,"
      "\"evidence_ids\":[],\"channels\":[]}],\"summary\":\"\"}";
    hu_reflection_pattern_t *p = NULL; int n = 0;
    char *prose = NULL, *err = NULL;
    HU_ASSERT_EQ(hu_reflection_parse(json, &p, &n, &prose, &err), HU_OK);
    HU_ASSERT_EQ(n, 1);
    /* spec says: parse layer returns it; storage drops it. So parse keeps. */
    HU_ASSERT_TRUE(p[0].confidence < 0.5);
    free(prose); free(p);
}
```
(Confirms parse keeps low-confidence patterns; storage layer in Task 2 drops them.)

- [x] **Step 1.10: Run all schema tests + commit**

```
./build/human_tests --suite=reflection_schema
```
Expected: 6/6 PASS, 0 ASan errors.

```
git add include/human/reflection.h src/reflection/schema.c \
        tests/test_reflection_schema.c tests/test_main.c CMakeLists.txt
git commit -m "feat(reflection): public header + schema parser with stable id"
```

(Full commit message body in the format used by your `.githooks/commit-msg`; same template applies to all task commits.)

---

## Task 2: SQLite storage layer (migrations, UPSERT, queries)

**STATUS: DONE 2026-05-27** — commit `5b805657` on `origin/main`.
4 tests pin AC-T2.1..2.4, full suite 12721/12721 green, 0 ASan.
hu_reflection_compute_id promoted to public API as part of this
task so storage tests can derive stable IDs from struct fields
without round-tripping through hu_reflection_parse.

**Files:**
- Create: `src/reflection/storage.c`
- Create: `tests/test_reflection_storage.c`
- Modify: `tests/test_main.c`
- Modify: `CMakeLists.txt`

- [x] **Step 2.1: Write failing migration test**

`tests/test_reflection_storage.c`:

```c
#include "human/reflection.h"
#include "test_framework.h"
#include <sqlite3.h>

static void test_storage_migrates_creates_tables(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_reflection_storage_migrate(db), HU_OK);

    /* both tables exist */
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
        "SELECT name FROM sqlite_master WHERE type='table' "
        "AND name IN ('reflection_runs','reflection_patterns') "
        "ORDER BY name", -1, &stmt, NULL);
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char*)sqlite3_column_text(stmt, 0),
                     "reflection_patterns");
    HU_ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char*)sqlite3_column_text(stmt, 0),
                     "reflection_runs");
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

void run_reflection_storage_tests(void) {
    HU_TEST_SUITE("reflection_storage");
    HU_RUN_TEST(test_storage_migrates_creates_tables);
}
```

- [x] **Step 2.2: Run to verify it fails**

```
cmake --build --preset dev --target human_tests 2>&1 | tail -10
```
Expected: undefined reference to `hu_reflection_storage_migrate`.

- [x] **Step 2.3: Implement migrations**

`src/reflection/storage.c`:

```c
#include "human/reflection.h"
#include "human/core/log.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_migrate_sql =
    "CREATE TABLE IF NOT EXISTS reflection_runs ("
    "  run_id TEXT PRIMARY KEY,"
    "  provider TEXT NOT NULL,"
    "  started_at_ms INTEGER NOT NULL,"
    "  completed_at_ms INTEGER,"
    "  input_turns INTEGER NOT NULL,"
    "  input_tokens INTEGER,"
    "  output_tokens INTEGER,"
    "  status TEXT NOT NULL,"
    "  error_message TEXT,"
    "  json_dump_path TEXT,"
    "  prose_summary TEXT,"
    "  low_confidence_dropped_count INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS reflection_patterns ("
    "  id TEXT PRIMARY KEY,"
    "  type TEXT NOT NULL,"
    "  subject TEXT NOT NULL,"
    "  observation TEXT NOT NULL,"
    "  confidence REAL NOT NULL,"
    "  evidence_json TEXT NOT NULL,"
    "  channels_json TEXT NOT NULL,"
    "  first_seen_run_id TEXT NOT NULL REFERENCES reflection_runs(run_id),"
    "  last_seen_run_id TEXT NOT NULL REFERENCES reflection_runs(run_id),"
    "  observation_count INTEGER NOT NULL DEFAULT 1,"
    "  created_at_ms INTEGER NOT NULL,"
    "  last_observed_at_ms INTEGER NOT NULL,"
    "  expires_at_ms INTEGER NOT NULL,"
    "  surfaced_to_user INTEGER NOT NULL DEFAULT 0,"
    "  retired INTEGER NOT NULL DEFAULT 0,"
    "  retired_at_ms INTEGER"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_patterns_recent "
    "  ON reflection_patterns(last_observed_at_ms DESC);"
    "CREATE INDEX IF NOT EXISTS idx_patterns_unsurfaced "
    "  ON reflection_patterns(surfaced_to_user, retired, confidence DESC);"
    ;

hu_error_t hu_reflection_storage_migrate(sqlite3 *db) {
    if (!db) return HU_ERR_INVALID_ARG;
    char *err = NULL;
    int rc = sqlite3_exec(db, k_migrate_sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        hu_log_error("reflection", "migrate failed: %s", err ? err : "?");
        sqlite3_free(err);
        return HU_ERR_DB;
    }
    return HU_OK;
}
```

Add prototype to `include/human/reflection.h`:
```c
hu_error_t hu_reflection_storage_migrate(sqlite3 *db);
```

- [x] **Step 2.4: Verify migration test passes**

```
./build/human_tests --filter=storage_migrates
```
Expected: PASS, 0 ASan.

- [x] **Step 2.5: Add UPSERT semantics test**

```c
static void test_storage_upsert_bumps_observation_count(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);

    /* Two runs */
    hu_reflection_storage_insert_run(db, "run_1", "gemini-3.1-pro",
                                     1000, 10);
    hu_reflection_storage_insert_run(db, "run_2", "gemini-3.1-pro",
                                     2000, 12);

    hu_reflection_pattern_t p = {
        .type = HU_REFLECTION_PATTERN_PREFERENCE,
        .subject = "Seth", .observation = "concise replies",
        .confidence = 0.8, .evidence_count = 1, .channel_count = 1,
        .created_at_ms = 1000, .last_observed_at_ms = 1000,
        .expires_at_ms = 1000 + 30L*86400000L
    };
    strcpy(p.evidence_ids[0], "turn_1");
    strcpy(p.channels[0], "imessage");
    hu_reflection_compute_id(p.type, p.subject, p.observation,
                             p.id, sizeof p.id);

    HU_ASSERT_EQ(hu_reflection_storage_upsert(db, "run_1", &p), HU_OK);

    /* re-insert same id under run_2 with higher confidence */
    p.confidence = 0.9;
    p.last_observed_at_ms = 2000;
    HU_ASSERT_EQ(hu_reflection_storage_upsert(db, "run_2", &p), HU_OK);

    /* verify single row, observation_count=2, confidence=0.9 (max) */
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT observation_count, confidence, last_seen_run_id "
        "FROM reflection_patterns WHERE id = ?", -1, &st, NULL);
    sqlite3_bind_text(st, 1, p.id, -1, SQLITE_STATIC);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(st, 0), 2);
    HU_ASSERT_TRUE(sqlite3_column_double(st, 1) > 0.89);
    HU_ASSERT_STR_EQ((const char*)sqlite3_column_text(st, 2), "run_2");
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
    sqlite3_finalize(st);
    sqlite3_close(db);
}
```

- [x] **Step 2.6: Implement insert_run + UPSERT**

In `storage.c`:
- `hu_reflection_storage_insert_run(db, run_id, provider, started_at, input_turns)` — INSERT with status='in_progress', completed_at_ms=NULL
- `hu_reflection_storage_complete_run(db, run_id, status, output_tokens, prose, json_dump_path, low_conf_dropped)` — UPDATE
- `hu_reflection_storage_upsert(db, run_id, pattern)`:
  - If `pattern->confidence < 0.5`: return HU_OK without inserting (caller increments dropped_count via complete_run)
  - Otherwise: `INSERT INTO reflection_patterns(...) VALUES(...) ON CONFLICT(id) DO UPDATE SET observation_count = observation_count + 1, last_observed_at_ms = excluded.last_observed_at_ms, confidence = MAX(confidence, excluded.confidence), last_seen_run_id = excluded.last_seen_run_id, expires_at_ms = excluded.expires_at_ms, evidence_json = excluded.evidence_json, channels_json = excluded.channels_json`
  - `evidence_json` and `channels_json` are JSON arrays serialized from the fixed-size arrays in the struct.

Add prototypes to `include/human/reflection.h`.

- [x] **Step 2.7: Verify UPSERT test passes**

```
./build/human_tests --filter=upsert_bumps
```

- [x] **Step 2.8: Add confidence-floor drop test**

```c
static void test_storage_drops_low_confidence(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    hu_reflection_storage_insert_run(db, "run_1", "gemini-3.1-pro",
                                     1000, 10);
    hu_reflection_pattern_t p = {
        .type = HU_REFLECTION_PATTERN_PREFERENCE,
        .subject = "Seth", .observation = "weak signal",
        .confidence = 0.3, .evidence_count = 0, .channel_count = 0,
        .created_at_ms = 1000, .last_observed_at_ms = 1000,
        .expires_at_ms = 1000 + 30L*86400000L
    };
    hu_reflection_compute_id(p.type, p.subject, p.observation,
                             p.id, sizeof p.id);
    HU_ASSERT_EQ(hu_reflection_storage_upsert(db, "run_1", &p), HU_OK);

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM reflection_patterns",
                       -1, &st, NULL);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(st, 0), 0);  /* dropped */
    sqlite3_finalize(st);
    sqlite3_close(db);
}
```

Run + verify pass.

- [x] **Step 2.9: Commit**

```
git add src/reflection/storage.c tests/test_reflection_storage.c \
        include/human/reflection.h tests/test_main.c CMakeLists.txt
git commit -m "feat(reflection): SQLite storage layer with UPSERT + confidence floor"
```

---

## Task 3: Config plumbing (`hu_reflection_config_t`)

**STATUS: DONE 2026-05-27** — commit `65fbc894` on `origin/main`.
Struct renamed from spec's `hu_reflection_config_t` to
`hu_reflection_loop_config_t` and the hu_config_t field from
`reflection` to `reflection_loop` because `hu_reflection_config_t`
already exists (used by src/intelligence/reflection.c, the older
skillforge-layer reflection — unrelated subsystem). The JSON key
stays `"reflection"` since that's the operator-facing contract.
Tests live in tests/test_config_parse.c (not test_config_extended.c)
matching where the rest of the parse_* tests live. 4 new tests cover
defaults / partial-block merge / overrides / pathological-input
clamping at (0, 720] hours.

**Files:**
- Modify: `include/human/config.h`
- Modify: `src/config/config_parse.c`
- Create/Modify: `tests/test_config_extended.c` (already in git status modified — extend it)

- [x] **Step 3.1: Add struct to config.h**

In `include/human/config.h`, find the root config struct (likely `hu_config_t`) and add:

```c
typedef struct hu_reflection_config_t {
    bool   enabled;                      /* default false (opt-in) */
    bool   local_shadow_mode;            /* default false */
    int    min_interval_hours;           /* default 12 */
    int    idle_threshold_hours;         /* default 2 */
    int    daily_floor_hours;            /* default 24 */
    char   provider[64];                 /* default "gemini-3.1-pro-preview" */
    char   local_provider[64];           /* default "gemma-4-31b-local" */
} hu_reflection_config_t;
```

Add field `hu_reflection_config_t reflection;` to the root config struct. Initialize defaults in the existing config-init function (search `grep -n "hu_config_init\|hu_config_defaults" src/config*.c`).

- [x] **Step 3.2: Write failing parse test**

In `tests/test_config_extended.c`:

```c
static void test_config_parses_reflection_block(void) {
    const char *json =
      "{\"reflection\":{\"enabled\":true,\"min_interval_hours\":6,"
      "\"provider\":\"gemini-3.1-pro-preview\"}}";
    hu_config_t cfg = {0};
    hu_config_defaults(&cfg);
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, json, NULL), HU_OK);
    HU_ASSERT_TRUE(cfg.reflection.enabled);
    HU_ASSERT_EQ(cfg.reflection.min_interval_hours, 6);
    HU_ASSERT_STR_EQ(cfg.reflection.provider, "gemini-3.1-pro-preview");
    /* defaults still hold for unspecified */
    HU_ASSERT_EQ(cfg.reflection.idle_threshold_hours, 2);
    HU_ASSERT_EQ(cfg.reflection.daily_floor_hours, 24);
    HU_ASSERT_FALSE(cfg.reflection.local_shadow_mode);
}
```

- [x] **Step 3.3: Implement parser**

In `src/config/config_parse.c`, find where other config blocks are parsed (e.g., the reaction_collection block per the silent-config-gated-subsystems rule reference). Add a `parse_reflection_block` mirror that:
- Optional fields with type-checked reads
- Unknown subkeys emit the same "unknown key: 'reflection.X' (ignored)" warning as other subsystems

- [x] **Step 3.4: Run + commit**

```
./build/human_tests --filter=config_parses_reflection
git add include/human/config.h src/config/config_parse.c tests/test_config_extended.c
git commit -m "feat(config): add reflection.* block with sensible defaults"
```

---

## Task 4: Reflection prompt + input transcript assembly

**Files:**
- Create: `src/reflection/prompt.c`
- Create: `src/reflection/reflection_system_prompt.txt`
- Modify: `CMakeLists.txt` (install the .txt to share dir, or embed via xxd-style binary include)
- Tests: extend `tests/test_reflection_schema.c` or create `tests/test_reflection_prompt.c`

- [ ] **Step 4.1: Write the system prompt template**

Create `src/reflection/reflection_system_prompt.txt`:

```
You are a reflection layer over a personal-assistant transcript. Your job is to
identify durable patterns in how the user (and their relationships) operate,
based ONLY on the conversations provided. Do not invent observations not
grounded in the transcript.

Output STRICT JSON matching this schema:
{
  "patterns": [
    {
      "type": "topic_recurrence" | "behavioral_shift" | "preference" |
              "emotional_state" | "schedule_pattern" | "relationship",
      "subject": "string, up to 128 chars (usually 'Seth', or another named person)",
      "observation": "string, up to 512 chars, specific and grounded in evidence",
      "confidence": number in [0, 1],
      "evidence_ids": ["turn_id_1", ...],
      "channels": ["channel_name", ...]
    }
  ],
  "summary": "2-3 sentence prose summary of the most important patterns this run"
}

Rules:
- Confidence < 0.5 means "I noticed something but I'm unsure" — emit it; the storage layer will filter.
- A pattern observed on a single occasion has lower confidence than one observed multiple times.
- Prefer specificity ("Seth shifts to one-word replies after 9pm weeknights") over generality ("Seth is sometimes terse").
- Do not emit patterns that restate stable known facts (e.g., "Seth's name is Seth"). Patterns are about CHANGES, RECURRENCES, or NEWLY-OBSERVABLE traits.
- Emit at most 15 patterns per run; choose the most signal-rich.
```

- [ ] **Step 4.2: Decide template loading strategy**

Two options:
- **Embedded:** compile-time `xxd -i` produces `reflection_system_prompt_txt[]`, included via header. No install step. Check via `grep -rn "xxd" CMakeLists.txt` to see if used elsewhere.
- **Runtime read:** install template to `${CMAKE_INSTALL_DATADIR}/human/reflection/`, read at first tick. Easier to tune without rebuild.

Pick whichever matches existing h-uman convention for prompt templates. Document the choice in a comment at the top of `prompt.c`.

- [ ] **Step 4.3: Write failing input-assembly test**

```c
static void test_prompt_assembles_input_with_turn_ids(void) {
    hu_test_daemon_t *d = hu_test_daemon_new();
    hu_test_daemon_add_turn(d, "imessage", 1000,
                            "user", "I've barely slept this week");
    hu_test_daemon_add_turn(d, "imessage", 1100,
                            "assistant", "Sorry — anything specific keeping you up?");
    hu_test_daemon_add_turn(d, "telegram", 1200,
                            "user", "Work deadlines piling on");

    char *buf = NULL; int turn_count = 0;
    HU_ASSERT_EQ(hu_reflection_build_input(d, /*since_ms=*/0,
                                            &buf, &turn_count), HU_OK);
    HU_ASSERT_EQ(turn_count, 3);
    HU_ASSERT_NE(buf, NULL);
    HU_ASSERT_TRUE(strstr(buf, "[channel=imessage]") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "[channel=telegram]") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "barely slept") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Work deadlines") != NULL);
    free(buf);
    hu_test_daemon_free(d);
}
```

- [ ] **Step 4.4: Implement `hu_reflection_build_input`**

In `prompt.c`:
- Query daemon's message ledger for turns with `ts > since_ms`, across all channels, ordered by ts ASC
- For each turn, append `[id=t_<ts>_<channel>_<seq>] [channel=<name>] [ts=<iso8601>] <sender>: <text>\n`
- Track running char count; cap at 100K chars (≈ 25K tokens); if cap reached, drop oldest turns and re-emit (the truncation strategy in spec)
- Return final buffer (caller frees) + turn_count actually included

If `hu_test_daemon_*` helpers don't exist, create them in a new `tests/helpers/test_daemon.c` and document the pattern for future tests.

- [ ] **Step 4.5: Run + commit**

```
./build/human_tests --filter=prompt_assembles
git add src/reflection/prompt.c src/reflection/reflection_system_prompt.txt \
        tests/test_reflection_schema.c CMakeLists.txt include/human/reflection.h
git commit -m "feat(reflection): input transcript assembly + system prompt template"
```

---

## Task 5: Reflection orchestration (`reflection.c` — tick + run)

**Files:**
- Create: `src/reflection/reflection.c`
- Create: `tests/test_reflection_run.c` (will become test_reflection_e2e.c in Task 10)

- [ ] **Step 5.1: Write tick gate tests**

```c
static void test_tick_returns_ok_when_disabled(void) {
    hu_test_daemon_t *d = hu_test_daemon_new();
    d->cfg.reflection.enabled = false;
    HU_ASSERT_EQ(hu_reflection_tick(d), HU_OK);
    HU_ASSERT_EQ(hu_test_count_rows(d->db, "reflection_runs"), 0);
    hu_test_daemon_free(d);
}

static void test_tick_emits_one_shot_log_when_disabled(void) {
    hu_test_daemon_t *d = hu_test_daemon_new();
    d->cfg.reflection.enabled = false;
    hu_test_log_capture_start();
    hu_reflection_tick(d);
    hu_reflection_tick(d);  /* second tick should NOT re-log */
    int count = hu_test_log_count_containing("reflection subsystem disabled");
    HU_ASSERT_EQ(count, 1);
    hu_test_log_capture_end();
    hu_test_daemon_free(d);
}

static void test_tick_respects_min_interval(void) {
    hu_test_daemon_t *d = hu_test_daemon_new();
    d->cfg.reflection.enabled = true;
    d->cfg.reflection.min_interval_hours = 12;
    hu_test_insert_completed_run(d->db, "run_prev",
                                  hu_test_now_ms() - 3600*1000);
    HU_ASSERT_EQ(hu_reflection_tick(d), HU_OK);
    HU_ASSERT_EQ(hu_test_count_rows(d->db, "reflection_runs"), 1);
    hu_test_daemon_free(d);
}
```

- [ ] **Step 5.2: Implement tick**

Pattern after `src/agent/init_proposer.c` (the existing in-flight tick subsystem) for structure. Sketch:

```c
#include "human/reflection.h"
#include "human/core/log.h"
#include <stdatomic.h>

static atomic_bool g_warned_disabled = false;
static atomic_bool g_warned_enabled = false;

hu_error_t hu_reflection_tick(hu_daemon_t *d) {
    if (!d) return HU_ERR_INVALID_ARG;
    if (!d->cfg.reflection.enabled) {
        hu_log_info_once(&g_warned_disabled, "reflection", NULL,
            "reflection subsystem disabled by config "
            "(cfg.reflection.enabled=false); set reflection.enabled=true "
            "in ~/.human/config.json to activate");
        return HU_OK;
    }
    hu_log_info_once(&g_warned_enabled, "reflection", NULL,
        "reflection subsystem enabled (provider=%s, min_interval=%dh, "
        "idle_threshold=%dh, daily_floor=%dh)",
        d->cfg.reflection.provider,
        d->cfg.reflection.min_interval_hours,
        d->cfg.reflection.idle_threshold_hours,
        d->cfg.reflection.daily_floor_hours);

    uint64_t now = hu_now_ms();
    uint64_t last_completed = hu_reflection_storage_last_completed_ms(d->db);
    uint64_t since_last = now - last_completed;

    if (since_last < (uint64_t)d->cfg.reflection.min_interval_hours * 3600000)
        return HU_OK;

    bool force = since_last >= (uint64_t)d->cfg.reflection.daily_floor_hours * 3600000;

    if (!force) {
        uint64_t last_activity = hu_daemon_last_activity_ms(d);
        if (now - last_activity < (uint64_t)d->cfg.reflection.idle_threshold_hours * 3600000)
            return HU_OK;
    }

    return hu_reflection_run(d, force);
}
```

`hu_reflection_storage_last_completed_ms` is a new helper in storage.c — `SELECT MAX(completed_at_ms) FROM reflection_runs WHERE status='ok'`. Returns 0 if no rows.

`hu_daemon_last_activity_ms` — likely already exists; if not, query the message ledger for `MAX(ts)` across inbound + outbound. Search `grep -rn "last_activity\|last_message" src/` before adding.

- [ ] **Step 5.3: Implement `hu_reflection_run` skeleton**

```c
hu_error_t hu_reflection_run(hu_daemon_t *d, bool force) {
    (void)force;
    if (!d) return HU_ERR_INVALID_ARG;

    char run_id[64];
    uint64_t now = hu_now_ms();
    snprintf(run_id, sizeof run_id, "refl_%" PRIu64, now);

    uint64_t since_ms = hu_reflection_storage_last_completed_ms(d->db);
    char *input = NULL;
    int turn_count = 0;
    hu_error_t err = hu_reflection_build_input(d, since_ms, &input, &turn_count);
    if (err != HU_OK) { free(input); return err; }

    hu_reflection_storage_insert_run(d->db, run_id,
        d->cfg.reflection.provider, now, turn_count);

    char *output = NULL;
    err = hu_provider_chat_with_system(d->router, d->cfg.reflection.provider,
        hu_reflection_system_prompt(), input, &output);
    free(input);
    if (err != HU_OK) {
        hu_reflection_storage_complete_run(d->db, run_id, "provider_error",
            0, NULL, NULL, 0);
        free(output);
        return HU_OK;  /* don't propagate — daemon continues */
    }

    hu_reflection_pattern_t *patterns = NULL;
    int pattern_count = 0;
    char *prose = NULL;
    char *parse_err = NULL;
    err = hu_reflection_parse(output, &patterns, &pattern_count, &prose, &parse_err);
    if (err != HU_OK) {
        hu_reflection_storage_complete_run(d->db, run_id, "schema_invalid",
            0, NULL, parse_err, 0);
        free(parse_err); free(output);
        return HU_OK;  /* single repair retry wired in Step 5.5 */
    }

    int dropped = 0;
    for (int i = 0; i < pattern_count; i++) {
        if (patterns[i].id[0] == 0) {
            hu_reflection_compute_id(patterns[i].type, patterns[i].subject,
                patterns[i].observation, patterns[i].id, sizeof patterns[i].id);
        }
        if (patterns[i].confidence < 0.5) { dropped++; continue; }
        patterns[i].created_at_ms = now;
        patterns[i].last_observed_at_ms = now;
        patterns[i].expires_at_ms = now + 30L * 86400000L;
        hu_reflection_storage_upsert(d->db, run_id, &patterns[i]);
    }

    char dump_path[512];
    snprintf(dump_path, sizeof dump_path,
             "%s/reflections/%s/%s.json",
             hu_human_data_dir(), d->cfg.reflection.provider, run_id);
    hu_write_file_atomic(dump_path, output, strlen(output));

    hu_reflection_storage_complete_run(d->db, run_id, "ok",
        0, prose, dump_path, dropped);

    free(output); free(prose); free(patterns);
    return HU_OK;
}
```

`hu_reflection_system_prompt()` — returns const char* to the embedded template; lives in prompt.c.

- [ ] **Step 5.4: Run tick gate tests**

```
./build/human_tests --suite=reflection_run
```

- [ ] **Step 5.5: Add single-retry repair logic (Layer 1 fully)**

When `hu_reflection_parse` fails the FIRST time: build a repair prompt with the original system + user + `"Your output failed schema validation: <parse_err>. Re-emit valid JSON matching the schema strictly."` Call provider once more. If still fails → mark `schema_invalid`. Track retry count in `reflection_runs.error_message` field for ops visibility.

Pin with a test that supplies a sequence of provider responses [malformed, valid].

- [ ] **Step 5.6: Commit**

```
git add src/reflection/reflection.c tests/test_reflection_run.c \
        include/human/reflection.h
git commit -m "feat(reflection): tick gating + run orchestration with single-retry repair"
```

---

## Task 6: Consumer queries (system-prompt slice + unsurfaced + mutators)

**Files:**
- Create: `src/reflection/consumer.c`
- Create: `tests/test_reflection_consumer.c`

- [ ] **Step 6.1: Write failing query test**

```c
static void test_query_for_system_prompt_filters_by_channel(void) {
    sqlite3 *db = NULL; sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    hu_reflection_storage_insert_run(db, "run_1", "p", 1000, 10);

    /* Pattern only seen on imessage */
    hu_test_insert_pattern(db, "run_1", HU_REFLECTION_PATTERN_PREFERENCE,
        "Seth", "imessage-only thing", 0.9, "imessage");
    /* Pattern seen across imessage AND telegram (cross-channel) */
    hu_test_insert_pattern_multi_channel(db, "run_1",
        HU_REFLECTION_PATTERN_BEHAVIORAL_SHIFT,
        "Seth", "cross-channel thing", 0.9,
        (const char*[]){"imessage", "telegram"}, 2);

    hu_reflection_pattern_t *out = NULL; int n = 0;
    HU_ASSERT_EQ(hu_reflection_query_for_system_prompt(
        db, "telegram", 5, &out, &n), HU_OK);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_TRUE(strstr(out[0].observation, "cross-channel") != NULL);
    free(out);
    sqlite3_close(db);
}

static void test_query_excludes_retired_and_surfaced(void) {
    /* surfaced=1 → excluded; retired=1 → excluded; both → excluded.
       Insert 3 patterns: normal, surfaced, retired. Query returns 1.
       Use hu_reflection_mark_surfaced + hu_reflection_retire to mutate. */
}

static void test_query_caps_at_max_patterns(void) {
    /* Insert 10 valid patterns; query with max=5 returns 5 */
}

static void test_query_orders_by_confidence_recency(void) {
    /* Insert with varied confidences and ages; verify ordering */
}
```

- [ ] **Step 6.2: Implement the queries**

In `consumer.c`:

```c
hu_error_t hu_reflection_query_for_system_prompt(
    sqlite3 *db, const char *channel, int max_patterns,
    hu_reflection_pattern_t **out_patterns, int *out_count)
{
    /* SQL:
       SELECT id, type, subject, observation, confidence, evidence_json,
              channels_json, created_at_ms, last_observed_at_ms,
              expires_at_ms, surfaced_to_user, retired
         FROM reflection_patterns
        WHERE retired = 0
          AND surfaced_to_user = 0
          AND last_observed_at_ms > ?  -- now - 7d
          AND confidence > 0.7
          AND (EXISTS(SELECT 1 FROM json_each(channels_json) WHERE value = ?)
               OR json_array_length(channels_json) > 1)
        ORDER BY confidence * (1.0 / (1.0 + (? - last_observed_at_ms) / 86400000.0)) DESC
        LIMIT ?
    */
    /* Bind: now - 7d (ms), channel, now (ms for recency), max_patterns.
       Walk rows, deserialize evidence_json + channels_json back into struct arrays. */
}

hu_error_t hu_reflection_query_unsurfaced(
    sqlite3 *db, double min_confidence,
    hu_reflection_pattern_t **out_patterns, int *out_count)
{
    /* SELECT ... WHERE retired=0 AND surfaced_to_user=0
                  AND last_observed_at_ms > now - 30d
                  AND confidence > min_confidence
                  ORDER BY confidence DESC */
}

void hu_reflection_mark_surfaced(sqlite3 *db, const char *id) {
    /* UPDATE reflection_patterns SET surfaced_to_user=1 WHERE id=? */
}

void hu_reflection_retire(sqlite3 *db, const char *id) {
    /* UPDATE reflection_patterns SET retired=1, retired_at_ms=? WHERE id=? */
}
```

`hu_test_insert_pattern` and `hu_test_insert_pattern_multi_channel` are new test helpers — create in `tests/helpers/test_reflection_helpers.c` and document. They serialize channels via `sqlite3_mprintf` of a JSON array literal.

- [ ] **Step 6.3: Run + verify all consumer tests pass**

- [ ] **Step 6.4: Commit**

```
git commit -m "feat(reflection): consumer queries (system-prompt slice + unsurfaced)"
```

---

## Task 7: System-prompt integration (append reflection slice)

**Files:**
- Modify: `src/memory/personal_model.c` (or wherever `hu_personal_model_build_prompt` lives)
- Test: extend existing personal_model tests

- [ ] **Step 7.1: Locate the build-prompt function and add appendage**

Find the function (`grep -n "hu_personal_model_build_prompt" src/`). At the end of its prompt construction, before returning, append:

```c
#ifdef HU_ENABLE_SQLITE
    if (db && cfg && cfg->reflection.enabled) {
        hu_reflection_pattern_t *refl = NULL; int n = 0;
        if (hu_reflection_query_for_system_prompt(
                db, current_channel, /*max=*/5, &refl, &n) == HU_OK && n > 0) {
            hu_strbuf_appendf(out, "\n\nRecent observations about Seth:\n");
            for (int i = 0; i < n; i++) {
                hu_strbuf_appendf(out, "- %s (confidence %.2f)\n",
                    refl[i].observation, refl[i].confidence);
            }
            char *summary = hu_reflection_latest_prose_summary(db);
            if (summary && *summary) {
                hu_strbuf_appendf(out, "\nLatest reflection: %s\n", summary);
                free(summary);
            }
            free(refl);
        }
    }
#endif
```

`hu_reflection_latest_prose_summary` is a new helper in consumer.c — `SELECT prose_summary FROM reflection_runs WHERE status='ok' ORDER BY completed_at_ms DESC LIMIT 1`.

- [ ] **Step 7.2: Test that channel-filtering reaches the prompt**

Add `tests/test_personal_model_with_reflection.c`:

```c
static void test_personal_model_prompt_includes_reflection_slice(void) {
    /* setup: in-memory db with 1 imessage pattern, 1 cross-channel pattern.
       call hu_personal_model_build_prompt with channel="imessage" — assert
       prompt contains both observations.
       call with channel="telegram" — assert prompt contains ONLY the
       cross-channel observation. */
}
```

- [ ] **Step 7.3: Commit**

```
git commit -m "feat(reflection): wire reflection slice into hu_personal_model_build_prompt"
```

---

## Task 8: init_proposer integration (reflection as candidate source)

**Files:**
- Modify: `src/agent/init_proposer.c` (already in flight per git status)
- Test: extend existing `tests/test_init_proposer.c`

- [ ] **Step 8.1: Add reflection candidate source**

In `init_proposer.c`, find the candidate-gathering step (whatever the T2 initiative-layer spec calls "context bundle"). Add reflection as a source:

```c
#ifdef HU_ENABLE_SQLITE
hu_reflection_pattern_t *refl = NULL; int n = 0;
if (hu_reflection_query_unsurfaced(d->db, /*min_conf=*/0.6, &refl, &n) == HU_OK) {
    for (int i = 0; i < n; i++) {
        hu_init_candidate_t cand = {0};
        snprintf(cand.source, sizeof cand.source, "reflection:%s", refl[i].id);
        snprintf(cand.text, sizeof cand.text, "%s", refl[i].observation);
        cand.confidence = refl[i].confidence;
        cand.created_at_ms = refl[i].created_at_ms;
        hu_init_proposer_add_candidate(ctx, &cand);
    }
    free(refl);
}
#endif
```

- [ ] **Step 8.2: When init_proposer surfaces a reflection-sourced candidate, call `mark_surfaced`**

In init_proposer's send-path (wherever it confirms a proposal was actually dispatched):

```c
if (strncmp(chosen->source, "reflection:", 11) == 0) {
    const char *pattern_id = chosen->source + 11;
    hu_reflection_mark_surfaced(d->db, pattern_id);
}
```

- [ ] **Step 8.3: Add retire-on-contradiction wiring**

Locate the reaction_collection or response_guard negative-feedback callback (search `grep -rn "negative_reaction\|thumbs_down\|user_contradicted" src/`). When such a signal is received AND the most-recent send was a reflection-sourced proposal, call `hu_reflection_retire(db, pattern_id)`.

If this callback site doesn't exist yet, document this as a follow-up task and create a TODO chip via `mcp__ccd_session__spawn_task` describing the gap.

- [ ] **Step 8.4: Tests**

```c
static void test_init_proposer_pulls_reflection_unsurfaced(void) { /* ... */ }
static void test_proposer_marks_surfaced_after_send(void) { /* ... */ }
static void test_retire_on_negative_reaction(void) { /* ... */ }
```

- [ ] **Step 8.5: Commit**

```
git commit -m "feat(reflection,init_proposer): consume reflection patterns as candidates"
```

---

## Task 9: Daemon wiring (register the tick)

**Files:**
- Modify: `src/daemon.c` (or wherever the tick loop lives — search `grep -n "tick_reaction_collection\|tick_feeds" src/`)

- [ ] **Step 9.1: Find the tick dispatch site**

```
grep -n "hu_daemon_tick_\|tick_feeds\|tick_reaction" src/daemon.c | head
```

Add a new call in the same loop:

```c
#ifdef HU_ENABLE_SQLITE
hu_reflection_tick(d);   /* gates internally; cheap when disabled */
#endif
```

- [ ] **Step 9.2: Storage migration on daemon startup**

In daemon startup (where other migrations run — `grep -n "hu_.*_migrate\|sqlite3_exec.*CREATE" src/daemon*.c`):

```c
#ifdef HU_ENABLE_SQLITE
hu_reflection_storage_migrate(d->db);
#endif
```

- [ ] **Step 9.3: Quick smoke run**

```
cmake --build --preset dev --target human_daemon
./build/human_daemon --config-test ~/.human/config.json
```
Expected: daemon starts, no errors. With `reflection.enabled=false` (default), no reflection runs occur.

- [ ] **Step 9.4: Commit**

```
git commit -m "feat(daemon): register hu_reflection_tick + run storage migration on startup"
```

---

## Task 10: End-to-end test with mock provider

**Files:**
- Create: `tests/test_reflection_e2e.c`
- Possibly: extend `tests/helpers/` with a mock-provider helper

- [ ] **Step 10.1: Mock provider returning canned valid output → pipeline works end-to-end**

```c
static void test_e2e_valid_provider_response_inserts_patterns(void) {
    hu_test_daemon_t *d = hu_test_daemon_new();
    d->cfg.reflection.enabled = true;
    hu_test_daemon_add_turn(d, "imessage", 1000, "user",
                            "I keep forgetting to eat lunch");
    hu_test_daemon_add_turn(d, "imessage", 2000, "user",
                            "Skipped lunch again, no time today");

    hu_test_set_mock_provider_response(d,
      "{\"patterns\":[{\"type\":\"behavioral_shift\","
      "\"subject\":\"Seth\",\"observation\":\"frequently skipping lunch\","
      "\"confidence\":0.85,\"evidence_ids\":[\"t_1000\",\"t_2000\"],"
      "\"channels\":[\"imessage\"]}],\"summary\":\"Seth has been skipping lunch.\"}");

    HU_ASSERT_EQ(hu_reflection_run(d, /*force=*/true), HU_OK);

    HU_ASSERT_EQ(hu_test_count_rows_where(d->db, "reflection_runs",
                                          "status='ok'"), 1);
    HU_ASSERT_EQ(hu_test_count_rows(d->db, "reflection_patterns"), 1);
    hu_test_daemon_free(d);
}

static void test_e2e_malformed_response_marks_schema_invalid(void) {
    hu_test_daemon_t *d = hu_test_daemon_new();
    d->cfg.reflection.enabled = true;
    hu_test_daemon_add_turn(d, "imessage", 1000, "user", "hi");
    hu_test_set_mock_provider_response(d, "{not valid json");
    hu_test_set_mock_provider_response_n(d, 2, "{still bad");

    HU_ASSERT_EQ(hu_reflection_run(d, true), HU_OK);
    HU_ASSERT_EQ(hu_test_count_rows_where(d->db, "reflection_runs",
        "status='schema_invalid'"), 1);
    HU_ASSERT_EQ(hu_test_count_rows(d->db, "reflection_patterns"), 0);
    hu_test_daemon_free(d);
}

static void test_e2e_provider_error_marks_provider_error_status(void) {
    /* Mock provider returns HU_ERR_NETWORK → status='provider_error', no
       patterns, daemon-side function returns HU_OK (no crash) */
}

static void test_e2e_low_confidence_patterns_dropped_count_recorded(void) {
    /* Mock returns 3 patterns: confidence 0.9, 0.3, 0.4 → 1 inserted,
       low_confidence_dropped_count=2 in run row */
}
```

- [ ] **Step 10.2: Implement mock-provider plumbing**

If `hu_test_set_mock_provider_response` doesn't exist, add a test-mode provider in `tests/helpers/mock_provider.c` that implements the `hu_provider_t` vtable and returns canned responses in FIFO order. Wire `hu_test_daemon_new` to install it as the default router provider when the daemon is in test mode (gate with `HU_IS_TEST`).

- [ ] **Step 10.3: Run + commit**

```
./build/human_tests --suite=reflection_e2e
git commit -m "test(reflection): end-to-end with mock provider for all 4 status paths"
```

---

## Task 11: Quorum predicate (Phase 2 contract, Phase 1 telemetry only)

**Files:**
- Modify: `src/reflection/storage.c` (add `hu_reflection_pattern_has_quorum`)
- Create: `tests/test_reflection_quorum.c`
- Create: `scripts/check-reflection-quorum-not-wired.sh` (CI gate)
- Modify: `.githooks/pre-commit` (call the new check)

- [ ] **Step 11.1: Write tests for the predicate itself**

```c
static void test_quorum_false_single_observation(void) {
    /* Insert pattern once; has_quorum returns false */
}

static void test_quorum_false_three_observations_low_confidence(void) {
    /* Insert pattern 3 times across 3 runs, each with confidence 0.65;
       has_quorum returns false (need > 0.7 each) */
}

static void test_quorum_true_three_observations_high_confidence(void) {
    /* Insert across 3 runs each with confidence > 0.7 → true */
}
```

- [ ] **Step 11.2: Implement predicate**

```c
bool hu_reflection_pattern_has_quorum(sqlite3 *db, const char *pattern_id) {
    if (!db || !pattern_id) return false;
    /* Phase 1 approximation: we store only the MAX confidence per pattern.
       For a strict "≥3 distinct runs, each > 0.7" we would need a
       reflection_pattern_observations(pattern_id, run_id, confidence) join
       table. Phase 1 acceptable simplification:
           observation_count >= 3 AND confidence > 0.7
       Document this in design.md "Open questions" and revisit before
       Phase 2 wires belief updates. */
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT observation_count, confidence FROM reflection_patterns "
        "WHERE id=? AND retired=0", -1, &st, NULL);
    sqlite3_bind_text(st, 1, pattern_id, -1, SQLITE_STATIC);
    bool has = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        int oc = sqlite3_column_int(st, 0);
        double conf = sqlite3_column_double(st, 1);
        has = (oc >= 3 && conf > 0.7);
    }
    sqlite3_finalize(st);
    return has;
}
```

- [ ] **Step 11.3: Create CI gate script**

`scripts/check-reflection-quorum-not-wired.sh`:

```sh
#!/usr/bin/env bash
# Phase 1 contract: hu_reflection_pattern_has_quorum is TELEMETRY ONLY.
# It must not be used to gate mutations against hu_personal_model_t until
# Phase 2 lands. This script fails CI if any src/ file outside src/reflection/
# mentions has_quorum near personal_model symbols.
set -euo pipefail

HITS=$(grep -rln 'hu_reflection_pattern_has_quorum' src/ 2>/dev/null \
       | grep -v '^src/reflection/' || true)

VIOLATIONS=""
for f in $HITS; do
    if grep -q 'hu_personal_model_' "$f"; then
        VIOLATIONS+=" $f"
    fi
done

if [ -n "$VIOLATIONS" ]; then
    echo "ERROR: Phase 1 contract violated."
    echo "The following files use hu_reflection_pattern_has_quorum AND"
    echo "reference hu_personal_model_ — this means quorum may be gating"
    echo "a belief mutation, which is reserved for Phase 2."
    echo "Files: $VIOLATIONS"
    exit 1
fi
echo "OK: no Phase 1 quorum-mutation violations."
```

Make executable: `chmod +x scripts/check-reflection-quorum-not-wired.sh`

Wire into `.githooks/pre-commit` (append a call alongside the existing checks). Also document in `docs/plans/2026-05-26-reflection-loop/design.md` "Phase 2 plan" that lifting this gate requires explicit removal of the check + a Phase 2 spec.

- [ ] **Step 11.4: Smoke the CI gate**

```
bash scripts/check-reflection-quorum-not-wired.sh
```
Expected: "OK: no Phase 1 quorum-mutation violations." (Should pass — Phase 1 doesn't wire it.)

- [ ] **Step 11.5: Commit**

```
git add src/reflection/storage.c tests/test_reflection_quorum.c \
        include/human/reflection.h scripts/check-reflection-quorum-not-wired.sh \
        .githooks/pre-commit
git commit -m "feat(reflection): hu_reflection_pattern_has_quorum + Phase 1 CI gate"
```

---

## Task 12: Operator health logging + final acceptance verification

**Files:**
- Modify: `src/reflection/reflection.c` (failure-rate warning)
- Verification: run full `human_tests` + manual daemon smoke

- [ ] **Step 12.1: Add daily failure-rate warning**

In `reflection.c`, after `complete_run`, check:

```c
static void check_failure_rate(hu_daemon_t *d) {
    static atomic_bool warned = false;
    uint64_t since_24h = hu_now_ms() - 86400000;
    int total = hu_reflection_storage_count_runs_since(d->db, since_24h);
    int failed = hu_reflection_storage_count_failed_runs_since(d->db, since_24h);
    if (total >= 4 && failed * 2 > total) {  /* > 50% failure rate */
        hu_log_info_once(&warned, "reflection", NULL,
            "reflection failure rate > 50%% over last 24h (%d/%d failed) "
            "— check ~/.human/reflections/ json dumps for cause", failed, total);
    }
}
```

The two `count_*` helpers move from test helpers into `src/reflection/storage.c` as real public functions, since they're now used outside tests.

- [ ] **Step 12.2: Run full test suite**

```
./build/human_tests
```
Expected: 0 failures, 0 ASan errors. Total test count should be > prior baseline by ~25-30 (the new reflection tests).

- [ ] **Step 12.3: Run gate-symmetry check**

```
bash scripts/check-test-source-gate-symmetry.sh
```
Expected: PASS — all `src/reflection/*.c` gated on `HU_ENABLE_SQLITE` match the test gating.

- [ ] **Step 12.4: Run change-aware preflight**

```
scripts/agent-preflight.sh
```

- [ ] **Step 12.5: Manual smoke against acceptance criteria**

With reflection enabled in `~/.human/config.json`:
```json
{
  "reflection": {
    "enabled": true,
    "min_interval_hours": 0,
    "idle_threshold_hours": 0,
    "daily_floor_hours": 0,
    "provider": "gemini-3.1-pro-preview"
  }
}
```
(Zero thresholds for smoke test only — restore to 12/2/24 for production.)

Start daemon, send 20+ test turns across imessage + telegram, wait for reflection tick. Verify:
- **AC-1:** A `reflection_runs` row exists with `status='ok'`
- **AC-2:** `reflection_patterns` has ≥3 rows covering ≥2 distinct types
- **AC-3:** Run again on same corpus; pattern set overlap ≥ 80% by id
- **AC-4:** Inject malformed provider response via mock toggle; `status='schema_invalid'`, daemon still serving
- **AC-5:** Query mimicking `query_for_system_prompt` returns ≤5 rows, channel-filtered
- **AC-6:** init_proposer surfaces one; user contradicts; pattern retired
- **AC-7:** Disable reflection; verify one-shot log + no further runs

Document results in `docs/plans/2026-05-26-reflection-loop/results/acceptance-2026-05-XX.md` (date of run).

- [ ] **Step 12.6: Final commit**

```
git commit -m "chore(reflection): operator health logging + acceptance results"
```

---

## What Sprint 2 looks like (deferred, separate plan)

US-10 → US-13 from the spec become a Sprint 2 plan. Brief sketch:
- Add `reflection.local_shadow_mode` to config + dual-call in `reflection.c`
- Create `scripts/eval_reflection_shadow.py` per design's eval-harness section
- Wire retire-on-contradiction if Task 8.3 deferred any of it
- Run 14-day shadow eval; commit verdict JSON; decide on ratchet criterion

That gets its own plan when this one ships.

---

## Self-review checklist (run before handing off to implementer)

- [x] Every task has exact file paths
- [x] Every step that changes code shows the code or a complete enough sketch
- [x] Every command is runnable
- [x] All 7 acceptance criteria from design.md trace to at least one task (AC-1 → Tasks 3,5,9,12.5; AC-2 → Tasks 4,10,12.5; AC-3 → Tasks 2,10,12.5; AC-4 → Tasks 1,2,5,10,12.5; AC-5 → Tasks 6,7,12.5; AC-6 → Tasks 8,12.5; AC-7 → Tasks 3,5,12.5)
- [x] No "implement appropriate error handling" handwaves
- [x] Function names consistent across tasks (`hu_reflection_run`, `hu_reflection_tick`, `hu_reflection_storage_*`, `hu_reflection_query_*`)
- [x] Gate symmetry: all source files gated `HU_ENABLE_SQLITE` have tests gated identically (declared in Task 1 step 1.2 and reinforced in Task 12.3)
- [x] Commits are small (one task per commit, conventional commits format)
