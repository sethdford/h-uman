# Phase 3 — Make the Memory Vtable Real (Repository Pattern, Close T1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Close T1 — the ambient SQLite dependency. Today **146 call sites across 33 files / 28 domain tables** grab the raw `sqlite3*` via `hu_sqlite_memory_get_db()` and run SQL in domain code. Move that SQL behind **per-aggregate repository interfaces** whose only implementation that touches `sqlite3` lives in the engine layer. After this, a non-SQL backend (Redis / pure on-device) becomes *possible* — which is the entire "runs anywhere / privacy-by-architecture" moat.

**Architecture decision (NEEDS SIGN-OFF — this shapes the whole effort):**

| Option | What | Verdict |
|---|---|---|
| **A. Repository per aggregate** *(chosen)* | One small backend-agnostic interface per domain table (`hu_boundary_repo_t`, `hu_comfort_pattern_repo_t`, …); SQL lives only in `src/memory/repos/*_sqlite.c`. | **Recommended.** Truly enables non-SQL backends; consolidates the 33 scattered `get_db` sites into ~28 engine-layer files. Cost: 28 repos. |
| **B. Generic `query/exec` capability on the vtable** *(rejected)* | One method taking an SQL string + binds. | Removes the *type* dependency but keeps **SQL-dialect coupling in domain code** — a Redis backend still can't run it. Does NOT deliver the moat. Rejected. |
| **C. Reuse `hu_memory_query_t`** *(impossible)* | The facade's tagged-union query type. | **Name is taken** and it dispatches on fixed `hu_memory_kind_t` (entity/relation/KV/…), not arbitrary domain tables. Cannot extend. |

This plan delivers: (1) the repository pattern + factory wiring, (2) ONE fully-worked exemplar aggregate (`boundaries`), (3) the full 28-table inventory + a per-aggregate checklist. The remaining 27 aggregates are **follow-on chips** (one per aggregate cluster) — do not attempt 146 migrations in one plan (`~/.claude/rules/agent-task-sizing.md`).

**Tech Stack:** C11 vtable pattern, sqlite3 (engine layer only), `:memory:` test fixtures under `#ifdef HU_ENABLE_SQLITE`.

**Key fact:** `get_db` is *consolidated, not eliminated* — it moves from 33 domain files into ~28 `repos/*_sqlite.c` files under `src/memory/`, which the Phase-0 ratchet exempts (`src/memory/engines/`; extend the exemption to `src/memory/repos/`).

---

## File Structure (exemplar aggregate: `boundaries`)

- Create: `include/human/memory/boundary_repo.h` — backend-agnostic interface (vtable + factory)
- Create: `src/memory/repos/boundary_repo_sqlite.c` — the ONLY place `boundaries` SQL + `get_db` lives
- Create: `tests/test_boundary_repo.c` — `:memory:` TDD against the interface
- Modify: the current `protective.c` (locate in Step 0) — depend on the repo, drop `#include <sqlite3.h>`
- Modify: `src/CMakeLists.txt`, `tests/test_main.c`
- Modify: `scripts/check-sqlite-includer-ratchet.sh` — add `src/memory/repos/` to the exemption; lower BASELINE by 1

---

### Task 0: Locate the exemplar + capture the current SQL (orientation)

- [ ] **Step 1: Find the boundaries call sites and the current SQL**

Run:
```bash
grep -rn 'hu_protective_is_boundary\|hu_protective_add_boundary' src/
grep -rn 'FROM boundaries\|INTO boundaries' src/
```
Expected: the two functions (inventory: `protective.c:68` SIMPLE_READ `SELECT 1 FROM boundaries WHERE contact_id=? AND topic=? LIMIT 1`; `protective.c:97` WRITE `INSERT INTO boundaries (contact_id, topic, type, source, created_at)`). Note the exact file path and the exact SQL + column list — you'll reproduce it verbatim inside the sqlite repo so behavior is unchanged.

---

### Task 1: Define the repository interface (backend-agnostic, no SQL in signatures)

**Files:**
- Create: `include/human/memory/boundary_repo.h`

- [ ] **Step 1: Write the interface**

```c
/* include/human/memory/boundary_repo.h */
#ifndef HU_MEMORY_BOUNDARY_REPO_H
#define HU_MEMORY_BOUNDARY_REPO_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h" /* hu_memory_t */
#include <stdbool.h>
#include <stddef.h>

/* A protective "boundary": this contact + topic is off-limits. Pure domain
 * value object — no storage detail leaks here. */
typedef struct hu_boundary {
    const char *contact_id; size_t contact_id_len;
    const char *topic;      size_t topic_len;
    const char *type;       size_t type_len;   /* e.g. "hard", "soft" */
    const char *source;     size_t source_len; /* provenance */
    int64_t created_at;
} hu_boundary_t;

struct hu_boundary_repo_vtable;
typedef struct hu_boundary_repo {
    void *ctx;
    const struct hu_boundary_repo_vtable *vtable;
} hu_boundary_repo_t;

typedef struct hu_boundary_repo_vtable {
    /* True if (contact, topic) is a recorded boundary. */
    hu_error_t (*is_boundary)(void *ctx, const char *contact_id, size_t contact_id_len,
                              const char *topic, size_t topic_len, bool *out);
    /* Record a boundary (idempotent). */
    hu_error_t (*add)(void *ctx, const hu_boundary_t *b);
    void (*deinit)(void *ctx);
} hu_boundary_repo_vtable_t;

/* Factory: build a repo backed by `mem`. Returns a sqlite-backed repo when
 * `mem` is sqlite; HU_ERR_NOT_SUPPORTED for non-SQL backends (until a native
 * impl exists). This is the ONLY entry point domain code uses — it never sees
 * sqlite3. Caller owns *out and must call out->vtable->deinit. */
hu_error_t hu_boundary_repo_create(hu_memory_t *mem, hu_allocator_t *alloc,
                                   hu_boundary_repo_t *out);

#endif /* HU_MEMORY_BOUNDARY_REPO_H */
```

- [ ] **Step 2: Write the failing test**

```c
/* tests/test_boundary_repo.c */
#ifdef HU_ENABLE_SQLITE
#include "human/memory/boundary_repo.h"
#include "test_harness.h"
#include <string.h>

static void boundary_repo_records_and_reads(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_boundary_repo_t repo;
    HU_ASSERT_EQ(hu_boundary_repo_create(&mem, &alloc, &repo), HU_OK);

    bool is_b = true;
    HU_ASSERT_EQ(repo.vtable->is_boundary(repo.ctx, "alice", 5, "work", 4, &is_b), HU_OK);
    HU_ASSERT_TRUE(!is_b); /* nothing recorded yet */

    hu_boundary_t b = {.contact_id="alice", .contact_id_len=5, .topic="work", .topic_len=4,
                       .type="hard", .type_len=4, .source="user", .source_len=4, .created_at=1};
    HU_ASSERT_EQ(repo.vtable->add(repo.ctx, &b), HU_OK);

    HU_ASSERT_EQ(repo.vtable->is_boundary(repo.ctx, "alice", 5, "work", 4, &is_b), HU_OK);
    HU_ASSERT_TRUE(is_b);
    /* unrelated topic is not a boundary */
    HU_ASSERT_EQ(repo.vtable->is_boundary(repo.ctx, "alice", 5, "weather", 7, &is_b), HU_OK);
    HU_ASSERT_TRUE(!is_b);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

void run_boundary_repo_tests(void) {
    HU_TEST_SUITE("boundary_repo");
    HU_RUN_TEST(boundary_repo_records_and_reads);
}
#else
void run_boundary_repo_tests(void) { (void)0; } /* gate stub, per test-source-gate-symmetry */
#endif
```

- [ ] **Step 3: Verify it fails to link** — `touch tests/test_boundary_repo.c && cmake --build build --target human_tests -j8` → undefined `hu_boundary_repo_create`.

---

### Task 2: Implement the sqlite-backed repo (SQL lives ONLY here)

**Files:**
- Create: `src/memory/repos/boundary_repo_sqlite.c`

- [ ] **Step 1: Write the impl** (reproduce the exact SQL from Task 0)

> Note: schema creation uses prepare/step (not the one-shot SQL runner) — keeps
> all statements parameterizable and uniform with the read/write paths.

```c
/* src/memory/repos/boundary_repo_sqlite.c
 * The ONE place boundaries SQL + the raw sqlite3 handle live. Domain code
 * (protective.c) depends on hu_boundary_repo_t, never on this file. */
#ifdef HU_ENABLE_SQLITE
#include "human/memory/boundary_repo.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

typedef struct { sqlite3 *db; hu_allocator_t *alloc; } repo_ctx_t;

/* Run a parameterless DDL/statement via prepare/step. */
static hu_error_t run_stmt(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return HU_ERR_IO;
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? HU_OK : HU_ERR_IO;
}

static hu_error_t ensure_schema(sqlite3 *db) {
    return run_stmt(db,
        "CREATE TABLE IF NOT EXISTS boundaries ("
        " contact_id TEXT NOT NULL, topic TEXT NOT NULL, type TEXT,"
        " source TEXT, created_at INTEGER,"
        " UNIQUE(contact_id, topic));");
}

static hu_error_t repo_is_boundary(void *ctx, const char *cid, size_t cid_len,
                                   const char *topic, size_t topic_len, bool *out) {
    repo_ctx_t *c = ctx;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(c->db,
            "SELECT 1 FROM boundaries WHERE contact_id=? AND topic=? LIMIT 1;",
            -1, &st, NULL) != SQLITE_OK) return HU_ERR_IO;
    sqlite3_bind_text(st, 1, cid, (int)cid_len, SQLITE_STATIC);   /* never TRANSIENT */
    sqlite3_bind_text(st, 2, topic, (int)topic_len, SQLITE_STATIC);
    *out = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return HU_OK;
}

static hu_error_t repo_add(void *ctx, const hu_boundary_t *b) {
    repo_ctx_t *c = ctx;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(c->db,
            "INSERT OR IGNORE INTO boundaries"
            " (contact_id, topic, type, source, created_at) VALUES (?,?,?,?,?);",
            -1, &st, NULL) != SQLITE_OK) return HU_ERR_IO;
    sqlite3_bind_text(st, 1, b->contact_id, (int)b->contact_id_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, b->topic, (int)b->topic_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, b->type, (int)b->type_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, b->source, (int)b->source_len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, b->created_at);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_IO;
}

static void repo_deinit(void *ctx) {
    repo_ctx_t *c = ctx;
    if (c) c->alloc->free(c->alloc->ctx, c, sizeof(*c));
}

static const hu_boundary_repo_vtable_t k_vt = {
    .is_boundary = repo_is_boundary, .add = repo_add, .deinit = repo_deinit,
};

hu_error_t hu_boundary_repo_create(hu_memory_t *mem, hu_allocator_t *alloc,
                                   hu_boundary_repo_t *out) {
    if (!mem || !alloc || !out) return HU_ERR_INVALID_ARGUMENT;
    sqlite3 *db = hu_sqlite_memory_get_db(mem); /* legal HERE — engine layer */
    if (!db) return HU_ERR_NOT_SUPPORTED;       /* non-sqlite backend */
    if (ensure_schema(db) != HU_OK) return HU_ERR_IO;
    repo_ctx_t *c = alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c) return HU_ERR_OUT_OF_MEMORY;
    c->db = db; c->alloc = alloc;
    out->ctx = c; out->vtable = &k_vt;
    return HU_OK;
}
#endif /* HU_ENABLE_SQLITE */
```

- [ ] **Step 2: Register + run the test** — add `src/memory/repos/boundary_repo_sqlite.c` and `tests/test_boundary_repo.c` to `src/CMakeLists.txt` inside the existing `if(HU_ENABLE_SQLITE)` block (test/source gate-symmetry); declare+call `run_boundary_repo_tests` in `tests/test_main.c` wrapped in `#ifdef HU_ENABLE_SQLITE`.

Run: `cmake --build build --target human_tests -j8 && ./build/human_tests --filter=boundary_repo`
Expected: PASS.

- [ ] **Step 3: Commit** — `feat(memory): boundary repository (sqlite impl behind backend-agnostic interface)`

---

### Task 3: Migrate `protective.c` to the repo (remove the leak)

**Files:** the `protective.c` located in Task 0.

- [ ] **Step 1: Replace raw SQL with repo calls** — in `hu_protective_is_boundary` and `hu_protective_add_boundary`, build the repo once (`hu_boundary_repo_create`) and call `vtable->is_boundary` / `vtable->add`. Delete the `sqlite3_prepare_v2`/`bind`/`step` blocks and the `hu_sqlite_memory_get_db()` call.
- [ ] **Step 2: Remove `#include <sqlite3.h>`** from `protective.c`. Add `#include "human/memory/boundary_repo.h"`.
- [ ] **Step 3: Build prod + FULL suite** — `touch <protective.c path> && cmake --build build --target human -j8 && cmake --build build --target human_tests -j8 && ./build/human_tests`. Expected: 0 failures; existing protective/boundary tests still green (behavior identical — same SQL, just relocated).
- [ ] **Step 4: Tighten the ratchet** — in `scripts/check-sqlite-includer-ratchet.sh`: add `| grep -v 'src/memory/repos/'` to the exemption, and lower `BASELINE` by 1 (protective.c no longer includes sqlite3.h). Run the script; confirm it passes at the new lower ceiling.
- [ ] **Step 5: Commit** — `refactor(memory): protective boundaries use repo, drop raw sqlite (T1 -1)`

---

### Task 4: Establish the per-aggregate checklist (for the follow-on chips)

- [ ] **Step 1: Write the repeatable recipe** into the repo dir as `src/memory/repos/README.md`:

```markdown
# Adding a repository (per-aggregate, closes one slice of T1)
1. grep the raw SQL for the table: `grep -rn 'FROM <table>\|INTO <table>' src/`
2. Define hu_<aggregate>_repo_t in include/human/memory/<aggregate>_repo.h
   (value object + vtable with the domain operations, NO sql in signatures).
3. Implement <aggregate>_repo_sqlite.c under src/memory/repos/ — reproduce the
   EXACT SQL verbatim; SQLITE_STATIC never SQLITE_TRANSIENT.
4. :memory: TDD test pinning each operation.
5. Migrate the domain call sites; delete their #include <sqlite3.h> + get_db.
6. Lower the ratchet BASELINE by the number of files that dropped the include.
```

- [ ] **Step 2: Commit** — `docs(memory): per-aggregate repository recipe`

---

## Appendix — Full T1 inventory (drives the follow-on chips)

**28 domain tables** behind the leak (from the audit). Group into chips of 3-5
related aggregates each (≈6-8 chips):

| Chip cluster | Tables | Rough site count |
|---|---|---|
| Relational state | `contact_relationships`, `contact_baselines`, `reciprocity_scores`, `self_awareness_stats` | ~18 |
| Emotional persistence | `emotional_moments`, `contact_mood_log`, `emotional_residues`, `comfort_patterns` | ~24 |
| Opinions & values | `opinions`, `inferred_values`, `general_lessons` | ~12 |
| Narrative | `life_chapters`, `narration_events`, `growth_milestones`, `micro_moments` | ~14 |
| Style | `style_fingerprints`, `contact_style_evolution`, `topic_baselines` | ~10 |
| Scheduling/queues | `thread_followups`, `delayed_followups`, `inbox_items`, `commitments` | ~16 |
| Proactive (daemon) | the ~47 `hu_service_run*` sites (temporal-window queries) | ~47 — hardest; do AFTER Phase 2b shrinks the daemon |
| RAG/FTS | `memories`/`memories_fts` join in `corrective_rag.c`, `crag` | ~5 |

**Difficulty (from audit):** ~47% of sites are SIMPLE_READ/WRITE (mechanical repo
moves); ~53% are COMPLEX_READ (joins/aggregates/temporal) needing richer repo
methods. The proactive-checkins cluster (~47 sites in one 1,496-LOC function)
should wait until Phase 2b has extracted it from `daemon.c`.

**Both leak APIs:** `hu_sqlite_memory_get_db()` (legacy `hu_memory_t`) AND
`hu_memory_facade_sqlite_db()` (modern facade). Repositories replace both —
ratchet both out.

## Self-Review

- **Design decision surfaced** (A/B/C) with recommendation + rationale — needs sign-off before mass migration. ✓
- **No placeholders:** the exemplar (`boundaries`) has complete interface, sqlite impl, and TDD test. The 27 other aggregates are explicitly scoped as chips with a written recipe, NOT vague "do the rest." ✓
- **Naming:** avoids the taken `hu_memory_query_t`; uses `hu_<aggregate>_repo_t`. ✓
- **Moat alignment:** SQL is consolidated into the engine layer; non-SQL backends become possible (the actual M2/M3/privacy unlock). `SQLITE_STATIC` per project rule. Ratchet tightens per migration. ✓
- **Sequencing:** the daemon proactive cluster (47 sites) is deferred behind Phase 2b — flagged. ✓
