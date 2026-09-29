---
title: Named-entity extraction — implementation plan
date: 2026-09-29
status: draft (awaiting review)
spec: docs/superpowers/specs/2026-09-29-named-entity-extraction-design.md
---

# Named-Entity Extraction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Each contact's graph holds the named people, places, orgs and events they talk about, typed and fresh, and grounding can name them — live-typed-name recall on 40 real moments goes from the re-measured baseline to ≥ 15/40 under `HU_GRAPH_NAMES=live`, with `off` byte-identical to today.

**Architecture:** Four write paths feed one C API. A pure per-turn catcher (`name_extract.c`) runs on the contact's inbound text in the daemon (`HU_NAME_CATCH`). A nightly GLM pass (`insight_stream.py --names`) and a one-time migration (`graph_retype_entities.py`) emit typed entity JSONL lines. All of them write through `hu_graph_upsert_entity_typed`, directly or via the `human memory import-facts` importer. On the read side, the lexical → fallback → self composition moves out of the agent into `hu_graph_ground_compose_turn`, which both the live turn and `human memory ground --full` call. That function applies the `HU_GRAPH_NAMES` gate. `eval_name_grounding.py` measures that shared path.

**Tech Stack:** C11 (`-Wall -Wextra -Wpedantic -Werror`, custom `test_framework.h`), SQLite via `src/memory/graph.c` only, Python 3 stdlib + pytest (hermetic), local GLM on `127.0.0.1:8741`.

**Spec:** `docs/superpowers/specs/2026-09-29-named-entity-extraction-design.md` (binding). Executors read both.

## Global Constraints

- **Worktree:** every command uses absolute paths. `W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities` is set at the top of each command block because `cd` does not persist between tool calls. Commit with `git -C "$W"`.
- **Never** touch `~/.human`, `~/Library/Messages/chat.db`, ports 8741/8743, launchd plists, or run the daemon. Tests must not either.
- Gates: `HU_NAME_CATCH=off|shadow|live` and `HU_GRAPH_NAMES=off|shadow|live`. Both are parsed with `hu_gate_mode_from_env` and **default OFF**.
- With `HU_GRAPH_NAMES=off`, grounding output is **byte-identical** to today for identical graph contents (pinned by a test in Task 4 and re-run in Task 5).
- The per-turn catcher uses **no model**. Its input is **the contact's inbound text only**, never the generated reply.
- Catcher writes in LIVE: a KNOWN name → `hu_graph_upsert_entity` bump. A new CAPITALIZED name → `hu_graph_upsert_entity_typed(..., UNKNOWN, "names:turn", 0.3)`. In SHADOW it logs `name_catch shadow: known=%zu new=%zu (not written)`. When disabled it logs one line per process.
- Retype policy (`hu_graph_entity_retype_allowed`): allowed iff `old ∈ {UNKNOWN, TOPIC}` and `new ∈ {PERSON, PLACE, ORGANIZATION, EVENT, TOPIC}` and `old != new`, **or** `old == UNKNOWN` → any non-UNKNOWN. A name type is never downgraded. EMOTION is never touched. Provenance is written only when the row has none (first writer wins).
- JSONL entity line: `{"kind":"entity","contact":…,"name":…,"type":…,"source":…,"confidence":…}` with `type ∈ {person, place, org, event, topic}`. Anything else skips the line. The CLI prints `{"imported":N,"entities":E,"skipped":M,…}` and succeeds iff `N+E > 0`.
- Sources and confidences: `names:turn` 0.3, `names:nightly` 0.8, `names:migrate` (this plan: 0.6).
- Nightly pass: local model only (loopback URL), thinking suppressed (`chat_template_kwargs.enable_thinking=false`), `--names-days` default **2**, `--human-bin` default `~/.local/bin/human-daemon`. JSONL is **0600**. The manifest holds **counts only** (`contacts`, `names_kept`, `names_rejected`, `by_type`, `import_entities`, `model_errors`, …). Exit 2 = refused / nothing written; exit 3 = every attempted contact errored.
- Migration: backs up with the SQLite online backup API to `~/.human/backups/graph.db.bak-retype-<ts>` (0600) **before any write**. It refuses (exit 2) if the backup fails or graph.db is locked. Batches are **40** names per call. Unanswered names stay UNKNOWN. Re-runs are idempotent.
- Harness: **40** inbound 1:1 moments, **14** days, **≤ 8** per contact. It refuses (exit 2, writes nothing) with fewer than 40. Output goes to `~/.human/logs/name-grounding-<ts>.json`, counts only.
- Python never writes graph.db, except the migration's backup copy. Every graph write goes through the C importer or the typed upsert.
- C rules: free every allocation (ASan). Use `SQLITE_STATIC`, never `SQLITE_TRANSIENT`. No real network or process spawning in C tests (`HU_IS_TEST`). Tests are deterministic.
- **sqlite-gated test symmetry** (`.claude/rules/test-source-gate-symmetry.md`): every new C source is unconditional in CMake. A test that needs SQLite wraps those bodies in `#ifdef HU_ENABLE_SQLITE` inside an unconditional test file, and the pure tests stay outside the `#ifdef`.
- **sqlite-includer ratchet:** no new `#include <sqlite3.h>` under `src/` (only `src/memory/graph.c`, which already has it, touches SQLite). `name_extract.c`, `daemon_name_catch.c`, `graph_ingest.c` and `graph_grounding.c` use the graph API only.
- **Dead-strip ratchet:** every new C function needs a live caller in the `human` binary or a test reference. Every new `.c` needs a product caller in the **same commit**, or counter A grows. That is why the catcher's extractor and its daemon wiring are one task.
- **File-size ceiling:** `src/daemon.c` is 10,402 lines against a 10,420 ceiling. The Task 3 edit adds at most 8 lines.
- **Clone ratchet:** new C must not add duplicated 6-line windows. Run `bash "$W/scripts/check-clone-ratchet.sh"` before each C commit. If it fails, restructure (different helper shape or variable names), don't copy-paste.
- **Agent-core boundary:** `src/agent/graph_grounding.c` must not include a provider factory or `memcmp` a channel name.
- **No new root files:** new C lives in `src/memory/` and `src/daemon/`.
- **Tests reference production symbols** (`.claude/rules/test-references-production-symbol.md`). `tests/test_X.c` must call `hu_*` symbols from `src/**/X.c`.
- Commits are conventional (`feat(names): …`, `refactor(grounding): …`, `test(names): …`) and end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Implementers on other models substitute their own trailer.
- Python tests are hermetic: temp sqlite, a monkeypatched model, a fake `human` binary written to `tmp_path`. They never read `~/.human` or chat.db. They run under the existing CI pytest step.
- Build: `cmake --build "$W/build" --target human human_tests -j8`. Tests run from the worktree root, because some tests read `src/…` by relative path: `cd "$W" && ./build/human_tests --suite=<name>`. Before every commit, run the full suite: `cd "$W" && ./build/human_tests 2>&1 | grep -E 'Results:'`, which must show 0 failed.

## Review Focus

1. **Non-ASCII names in inbound texts** ("met José today", "Zoë's party"). The ASCII scanner must not emit a mangled fragment ("Jos", "Zo") as a new name. A reasonable person expects no candidate rather than a wrong one. Pinned in Task 3 by `test_extract_non_ascii_glued_token_is_not_a_candidate`.
2. **Names texted in lowercase** ("priya's surgery"). The model often answers "priya" too, and a lowercase person row never matches the catcher's or the backfill's capitalized "Priya". Expected: person, place, org and event names are stored with capitalized words. Pinned in Task 7 by `test_verify_names_capitalizes_name_types`.
3. **The same person as two rows** (legacy lowercase UNKNOWN "salim" plus typed "Salim"). Under LIVE the reply must not list them twice. Expected: only the typed row renders. Pinned in Task 5 by `test_names_live_renders_one_line_for_a_lowercase_duplicate`.
4. **First message from a contact with an empty graph.** Expected: the catcher still records their new Capitalized names. Pinned in Task 3 by `test_first_message_from_empty_contact_records_names`.
5. **Shouting and title-case texts** ("OMG SALIM IS HERE", "we said Happy New Year Everyone"). Expected: no candidates, because all-caps is not Capitalized and a 4-token run is not a name. Pinned in Task 3 by `test_extract_all_caps_and_long_title_runs_are_not_names`.

## Spec gaps and conflicts resolved in this plan

- `tests/test_insight_stream_names.py` **already exists**: it holds the wide pass's name tests. The nightly pass's tests go in `tests/test_insight_stream_names_pass.py`, which the existing CI glob already covers.
- Per spec, `hu_graph_upsert_entity_typed` bumps `last_seen`/`mention_count` on existing rows. The one-time migration would then make ~468 retyped entities look freshly mentioned and distort the fallback's recency and mention ranking. So the typed upsert takes `unsigned flags` with `HU_GRAPH_UPSERT_NO_TOUCH` (retype-only: no bump, never creates a row), and the JSONL line gains optional `"retype_only": true`. Only the migration uses it.
- "Same enumeration as the wide pass": the wide pass skips persona contacts, because the persona pass curates their insights. Persona contacts are the contacts the daemon replies to. So the names pass uses the wide enumeration **without** the persona skip, plus the spec's `--names-days` activity filter.
- JSONL type `org` has no string helper (`hu_entity_type_from_string` knows `organization`). The importer maps exactly `person/place/org/event/topic`.
- `compose_turn(…, flags, …)`: `flags` is a gate bitmask resolved from the environment by `hu_graph_ground_turn_flags_from_env()`, so composition is testable without `setenv`. Logging stays in the agent loader, because the tier used in log lines is only known there.
- **Topic line:** the spec doesn't say when it renders on an otherwise empty block. It is appended only to a non-empty block, which preserves "no match → empty injection". "Most recent" needs a new query, `hu_graph_list_recent_entities_of_type`.
- **LIVE seeding:** "entity lines drawn only from PERSON/PLACE/ORG/EVENT and Capitalized UNKNOWN" applies to the lexical path too. A named EMOTION or TOPIC no longer gets an entity line under LIVE. The owner ("About you:") block is unaffected by `HU_GRAPH_NAMES`.
- **Typed-name bonus:** the spec gives no value. `HU_GG_TYPED_NAME_BONUS = 0.5`, so at equal coverage a typed name outranks an untyped one.
- **Migration modes:** `--dry-run` (counts only, no model, no backup) and `--write` are required and mutually exclusive.
- **Catcher input:** the daemon's `combined` buffer includes model-generated vision and transcription text. The catcher reads each raw `msgs[b].content` and skips group chats.
- **Harness gates:** `HU_GRAPH_GROUNDING_CONTACT_FALLBACK` and `HU_GRAPH_GROUNDING_SELF_FACTS` default to `live` (prod) and are recorded in the output.
- **Task split vs. guidance:** the catcher's extractor and its daemon wiring merge into one task, because a `.c` with no product caller fails the dead-strip counter A at pre-commit. The grounding change splits into a pure refactor (Task 4) and the gate (Task 5).

---

### Task 1: Graph — retype policy and typed upsert

**Files:**
- Modify: `include/human/memory/graph.h` (after `hu_graph_find_entity`, line ~101)
- Modify: `src/memory/graph.c` (a new block after the `hu_graph_upsert_entity` `#else/#endif` at ~:400-416; the predicate after `hu_entity_type_to_string` at ~:2193)
- Test: `tests/test_graph.c`

**Interfaces:**
- Consumes: `hu_graph_open/close`, `hu_graph_upsert_entity`, `hu_graph_sqlite_connection`, the static `now_ms()`, `hu_sql_txn_*` in graph.c.
- Produces:
  - `bool hu_graph_entity_retype_allowed(hu_entity_type_t old_type, hu_entity_type_t new_type);`
  - `#define HU_GRAPH_UPSERT_NO_TOUCH 0x1u`
  - `hu_error_t hu_graph_upsert_entity_typed(hu_graph_t *g, const char *contact_id, size_t contact_id_len, const char *name, size_t name_len, hu_entity_type_t type, const char *provenance, float confidence, unsigned flags, int64_t *out_id);` It returns `HU_ERR_INVALID_ARGUMENT` on bad args, `HU_ERR_NOT_FOUND` for NO_TOUCH on a missing row, `HU_ERR_IO` on SQL failure, and `HU_ERR_NOT_SUPPORTED` without SQLite.

- [ ] **Step 1: Write the failing tests**

In `tests/test_graph.c`, add `#include <math.h>` and `#include <stdbool.h>` to the includes. Then insert this block **before** the existing `#ifdef HU_ENABLE_SQLITE`; the predicate is pure and runs in every build:

```c
/* Retype policy truth table (spec 2026-09-29 §4.1): UNKNOWN may become any
 * type, TOPIC may become a name type, and nothing else ever changes. */
static void graph_retype_allowed_truth_table(void) {
    const hu_entity_type_t names[] = {HU_ENTITY_PERSON, HU_ENTITY_PLACE, HU_ENTITY_ORGANIZATION,
                                      HU_ENTITY_EVENT};
    for (size_t i = 0; i < 4; i++) {
        HU_ASSERT_TRUE(hu_graph_entity_retype_allowed(HU_ENTITY_UNKNOWN, names[i]));
        HU_ASSERT_TRUE(hu_graph_entity_retype_allowed(HU_ENTITY_TOPIC, names[i]));
        HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(names[i], HU_ENTITY_TOPIC));
        HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(names[i], HU_ENTITY_UNKNOWN));
        HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(names[i], HU_ENTITY_EMOTION));
        HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(HU_ENTITY_EMOTION, names[i]));
        for (size_t j = 0; j < 4; j++) /* a name type is never swapped or downgraded */
            HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(names[i], names[j]));
    }
    HU_ASSERT_TRUE(hu_graph_entity_retype_allowed(HU_ENTITY_UNKNOWN, HU_ENTITY_TOPIC));
    HU_ASSERT_TRUE(hu_graph_entity_retype_allowed(HU_ENTITY_UNKNOWN, HU_ENTITY_EMOTION));
    HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(HU_ENTITY_UNKNOWN, HU_ENTITY_UNKNOWN));
    HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(HU_ENTITY_TOPIC, HU_ENTITY_TOPIC));
    HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(HU_ENTITY_TOPIC, HU_ENTITY_EMOTION));
    HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(HU_ENTITY_TOPIC, HU_ENTITY_UNKNOWN));
    HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(HU_ENTITY_EMOTION, HU_ENTITY_TOPIC));
    HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(HU_ENTITY_EMOTION, HU_ENTITY_UNKNOWN));
    /* out-of-range enum values fail closed */
    HU_ASSERT_FALSE(hu_graph_entity_retype_allowed((hu_entity_type_t)42, HU_ENTITY_PERSON));
    HU_ASSERT_FALSE(hu_graph_entity_retype_allowed(HU_ENTITY_UNKNOWN, (hu_entity_type_t)42));
}
```

Inside the existing `#ifdef HU_ENABLE_SQLITE` block (after `graph_close_valid_releases`), add:

```c
#include <sqlite3.h>

typedef struct typed_row {
    bool found;
    int type;
    int mention_count;
    int64_t last_seen;
    char provenance[64];
    double confidence;
} typed_row_t;

/* Read the columns hu_graph_find_entity does not expose (provenance, confidence). */
static typed_row_t typed_row(hu_graph_t *g, const char *cid, const char *name) {
    typed_row_t r;
    memset(&r, 0, sizeof(r));
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(hu_graph_sqlite_connection(g),
                           "SELECT type, mention_count, last_seen, COALESCE(provenance, ''),"
                           " confidence FROM entities WHERE contact_id = ?1 AND name = ?2",
                           -1, &q, NULL) != SQLITE_OK)
        return r;
    sqlite3_bind_text(q, 1, cid, -1, SQLITE_STATIC);
    sqlite3_bind_text(q, 2, name, -1, SQLITE_STATIC);
    if (sqlite3_step(q) == SQLITE_ROW) {
        r.found = true;
        r.type = sqlite3_column_int(q, 0);
        r.mention_count = sqlite3_column_int(q, 1);
        r.last_seen = sqlite3_column_int64(q, 2);
        snprintf(r.provenance, sizeof(r.provenance), "%s",
                 (const char *)sqlite3_column_text(q, 3));
        r.confidence = sqlite3_column_double(q, 4);
    }
    sqlite3_finalize(q);
    return r;
}

static void graph_upsert_typed_insert_sets_type_provenance_confidence(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Salim", 5, HU_ENTITY_PERSON,
                                              "names:nightly", 0.8f, 0, &id),
                 HU_OK);
    HU_ASSERT_TRUE(id > 0);
    typed_row_t r = typed_row(g, "c1", "Salim");
    HU_ASSERT_TRUE(r.found);
    HU_ASSERT_EQ(r.type, (int)HU_ENTITY_PERSON);
    HU_ASSERT_EQ(r.mention_count, 1);
    HU_ASSERT_STR_EQ(r.provenance, "names:nightly");
    HU_ASSERT_FLOAT_EQ(r.confidence, 0.8, 1e-6);
    hu_graph_close(g, &alloc);
}

static void graph_upsert_typed_upgrades_unknown_and_bumps(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);
    int64_t id1 = 0, id2 = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "c1", 2, "Vanguard", 8, HU_ENTITY_UNKNOWN, NULL, &id1),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Vanguard", 8, HU_ENTITY_ORGANIZATION,
                                              "names:nightly", 0.8f, 0, &id2),
                 HU_OK);
    HU_ASSERT_EQ(id1, id2);
    typed_row_t r = typed_row(g, "c1", "Vanguard");
    HU_ASSERT_EQ(r.type, (int)HU_ENTITY_ORGANIZATION);
    HU_ASSERT_EQ(r.mention_count, 2);
    HU_ASSERT_STR_EQ(r.provenance, "names:nightly"); /* the legacy row had none */
    hu_graph_close(g, &alloc);
}

static void graph_upsert_typed_never_downgrades_a_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Salim", 5, HU_ENTITY_PERSON,
                                              "names:turn", 0.3f, 0, &id),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Salim", 5, HU_ENTITY_TOPIC,
                                              "names:nightly", 0.8f, 0, &id),
                 HU_OK);
    typed_row_t r = typed_row(g, "c1", "Salim");
    HU_ASSERT_EQ(r.type, (int)HU_ENTITY_PERSON);
    HU_ASSERT_EQ(r.mention_count, 2);
    HU_ASSERT_STR_EQ(r.provenance, "names:turn"); /* first writer wins */
    hu_graph_close(g, &alloc);
}

static void graph_upsert_typed_topic_upgrades_to_place(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "c1", 2, "Tampa", 5, HU_ENTITY_TOPIC, NULL, &id), HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Tampa", 5, HU_ENTITY_PLACE,
                                              "names:nightly", 0.8f, 0, &id),
                 HU_OK);
    HU_ASSERT_EQ(typed_row(g, "c1", "Tampa").type, (int)HU_ENTITY_PLACE);
    hu_graph_close(g, &alloc);
}

static void graph_upsert_typed_leaves_emotion_alone(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "c1", 2, "grief", 5, HU_ENTITY_EMOTION, NULL, &id),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "grief", 5, HU_ENTITY_PERSON,
                                              "names:nightly", 0.8f, 0, &id),
                 HU_OK);
    HU_ASSERT_EQ(typed_row(g, "c1", "grief").type, (int)HU_ENTITY_EMOTION);
    hu_graph_close(g, &alloc);
}

static void graph_upsert_typed_no_touch_retypes_without_bumping_or_creating(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "c1", 2, "Acme", 4, HU_ENTITY_UNKNOWN, NULL, &id),
                 HU_OK);
    typed_row_t before = typed_row(g, "c1", "Acme");
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Acme", 4, HU_ENTITY_ORGANIZATION,
                                              "names:migrate", 0.6f, HU_GRAPH_UPSERT_NO_TOUCH,
                                              &id),
                 HU_OK);
    typed_row_t after = typed_row(g, "c1", "Acme");
    HU_ASSERT_EQ(after.type, (int)HU_ENTITY_ORGANIZATION);
    HU_ASSERT_EQ(after.mention_count, before.mention_count);
    HU_ASSERT_EQ(after.last_seen, before.last_seen);
    HU_ASSERT_STR_EQ(after.provenance, "names:migrate");
    /* retype-only never creates */
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "Ghost", 5, HU_ENTITY_PERSON,
                                              "names:migrate", 0.6f, HU_GRAPH_UPSERT_NO_TOUCH,
                                              &id),
                 HU_ERR_NOT_FOUND);
    HU_ASSERT_FALSE(typed_row(g, "c1", "Ghost").found);
    hu_graph_close(g, &alloc);
}

static void graph_upsert_typed_rejects_bad_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(NULL, "c1", 2, "A", 1, HU_ENTITY_PERSON, NULL,
                                              0.5f, 0, &id),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "A", 0, HU_ENTITY_PERSON, NULL, 0.5f,
                                              0, &id),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_graph_upsert_entity_typed(g, "c1", 2, "A", 1, HU_ENTITY_PERSON, NULL, 0.5f,
                                              0, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    hu_graph_close(g, &alloc);
}
```

Register them. In the SQLite `run_graph_tests` (after `HU_TEST_SUITE("graph");`) add:

```c
    HU_RUN_TEST(graph_retype_allowed_truth_table);
    HU_RUN_TEST(graph_upsert_typed_insert_sets_type_provenance_confidence);
    HU_RUN_TEST(graph_upsert_typed_upgrades_unknown_and_bumps);
    HU_RUN_TEST(graph_upsert_typed_never_downgrades_a_name);
    HU_RUN_TEST(graph_upsert_typed_topic_upgrades_to_place);
    HU_RUN_TEST(graph_upsert_typed_leaves_emotion_alone);
    HU_RUN_TEST(graph_upsert_typed_no_touch_retypes_without_bumping_or_creating);
    HU_RUN_TEST(graph_upsert_typed_rejects_bad_args);
```

and in the `#else` `run_graph_tests`:

```c
void run_graph_tests(void) {
    HU_TEST_SUITE("graph");
    HU_RUN_TEST(graph_retype_allowed_truth_table);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cmake --build "$W/build" --target human_tests -j8 2>&1 | grep -E 'error|warning' | head
```
Expected: compile errors, `implicit declaration of function 'hu_graph_entity_retype_allowed'` / `'hu_graph_upsert_entity_typed'`.

- [ ] **Step 3: Declare the API in `include/human/memory/graph.h`**

Add `#include <stdbool.h>` next to the other system includes. After the `hu_graph_find_entity` prototype, add:

```c
/* Retype policy for typed upserts (spec 2026-09-29 §4.1). True iff an existing
 * entity of `old_type` may become `new_type`:
 *   old UNKNOWN -> any type but UNKNOWN;
 *   old TOPIC   -> PERSON | PLACE | ORGANIZATION | EVENT;
 *   anything else (a name type, EMOTION, out-of-range) -> never.
 * A name type is never downgraded or swapped; EMOTION is never touched. */
bool hu_graph_entity_retype_allowed(hu_entity_type_t old_type, hu_entity_type_t new_type);

/* hu_graph_upsert_entity_typed flag: retype only. An existing row keeps its
 * last_seen and mention_count (the one-time migration must not make every
 * retyped entity look freshly mentioned) and a missing row is NOT created
 * (HU_ERR_NOT_FOUND). */
#define HU_GRAPH_UPSERT_NO_TOUCH 0x1u

/* Typed upsert. Insert: sets type, provenance (NULL/"" -> none) and confidence
 * (clamped to [0,1], mirrored into confidence_mean). Existing row: bumps
 * last_seen and mention_count like hu_graph_upsert_entity (unless NO_TOUCH),
 * changes the type only when hu_graph_entity_retype_allowed allows it, and
 * writes `provenance` only when the row has none (first writer wins). */
hu_error_t hu_graph_upsert_entity_typed(hu_graph_t *g, const char *contact_id,
                                        size_t contact_id_len, const char *name, size_t name_len,
                                        hu_entity_type_t type, const char *provenance,
                                        float confidence, unsigned flags, int64_t *out_id);
```

- [ ] **Step 4: Implement in `src/memory/graph.c`**

Directly after the `#endif` that closes the `hu_graph_upsert_entity` stub (just before `#ifdef HU_ENABLE_SQLITE` / `hu_graph_find_entity`), insert:

```c
#ifdef HU_ENABLE_SQLITE

/* Typed-upsert row lookup: SQLITE_ROW (id/type set), SQLITE_DONE (no row) or
 * an error code. */
static int ge_typed_lookup(sqlite3 *db, const char *cid, int cid_len, const char *name,
                           size_t name_len, int64_t *id, hu_entity_type_t *type) {
    sqlite3_stmt *q = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT id, type FROM entities WHERE contact_id = ?1 AND name = ?2",
                                -1, &q, NULL);
    if (rc != SQLITE_OK)
        return rc;
    sqlite3_bind_text(q, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_text(q, 2, name, (int)name_len, SQLITE_STATIC);
    rc = sqlite3_step(q);
    if (rc == SQLITE_ROW) {
        *id = sqlite3_column_int64(q, 0);
        *type = (hu_entity_type_t)sqlite3_column_int(q, 1);
    }
    sqlite3_finalize(q);
    return rc;
}

static int ge_typed_insert(sqlite3 *db, const char *cid, int cid_len, const char *name,
                           size_t name_len, hu_entity_type_t type, const char *prov,
                           size_t prov_len, float confidence, int64_t ts) {
    sqlite3_stmt *q = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "INSERT INTO entities (contact_id, name, type, first_seen, last_seen, mention_count,"
        " provenance, confidence, confidence_mean) VALUES (?1, ?2, ?3, ?4, ?4, 1, ?5, ?6, ?6)",
        -1, &q, NULL);
    if (rc != SQLITE_OK)
        return rc;
    sqlite3_bind_text(q, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_text(q, 2, name, (int)name_len, SQLITE_STATIC);
    sqlite3_bind_int(q, 3, (int)type);
    sqlite3_bind_int64(q, 4, ts);
    if (prov_len > 0)
        sqlite3_bind_text(q, 5, prov, (int)prov_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(q, 5);
    sqlite3_bind_double(q, 6, (double)confidence);
    rc = sqlite3_step(q);
    sqlite3_finalize(q);
    return rc;
}

/* Existing row: new type, provenance only when it has none, and (touch) the
 * same last_seen / mention_count bump as hu_graph_upsert_entity. */
static int ge_typed_update(sqlite3 *db, int64_t id, hu_entity_type_t type, const char *prov,
                           size_t prov_len, bool touch, int64_t ts) {
    sqlite3_stmt *q = NULL;
    int rc = sqlite3_prepare_v2(
        db,
        "UPDATE entities SET type = ?1, provenance = COALESCE(NULLIF(provenance, ''), ?2),"
        " last_seen = CASE WHEN ?3 THEN ?4 ELSE last_seen END,"
        " mention_count = mention_count + ?3 WHERE id = ?5",
        -1, &q, NULL);
    if (rc != SQLITE_OK)
        return rc;
    sqlite3_bind_int(q, 1, (int)type);
    if (prov_len > 0)
        sqlite3_bind_text(q, 2, prov, (int)prov_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(q, 2);
    sqlite3_bind_int(q, 3, touch ? 1 : 0);
    sqlite3_bind_int64(q, 4, ts);
    sqlite3_bind_int64(q, 5, id);
    rc = sqlite3_step(q);
    sqlite3_finalize(q);
    return rc;
}

hu_error_t hu_graph_upsert_entity_typed(hu_graph_t *g, const char *contact_id,
                                        size_t contact_id_len, const char *name, size_t name_len,
                                        hu_entity_type_t type, const char *provenance,
                                        float confidence, unsigned flags, int64_t *out_id) {
    if (!g || !g->db || !name || name_len == 0 || !out_id)
        return HU_ERR_INVALID_ARGUMENT;
    const char *cid = contact_id ? contact_id : "";
    int cid_len = contact_id ? (int)contact_id_len : 0;
    size_t prov_len = provenance ? strlen(provenance) : 0;
    if (confidence < 0.0f)
        confidence = 0.0f;
    else if (confidence > 1.0f)
        confidence = 1.0f;
    bool touch = (flags & HU_GRAPH_UPSERT_NO_TOUCH) == 0;

    hu_sql_txn_t txn = {0};
    if (hu_sql_txn_begin(&txn, g->db) != HU_OK)
        return HU_ERR_IO;
    int64_t id = 0;
    int64_t ts = now_ms();
    hu_entity_type_t old_type = HU_ENTITY_UNKNOWN;
    bool invalidate = false;
    int rc = ge_typed_lookup(g->db, cid, cid_len, name, name_len, &id, &old_type);
    if (rc == SQLITE_DONE && !touch) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_NOT_FOUND; /* retype-only never creates a row */
    }
    if (rc == SQLITE_DONE) {
        rc = ge_typed_insert(g->db, cid, cid_len, name, name_len, type, provenance, prov_len,
                             confidence, ts);
        id = sqlite3_last_insert_rowid(g->db);
        invalidate = true;
    } else if (rc == SQLITE_ROW) {
        hu_entity_type_t next = hu_graph_entity_retype_allowed(old_type, type) ? type : old_type;
        invalidate = next != old_type;
        rc = ge_typed_update(g->db, id, next, provenance, prov_len, touch, ts);
    }
    if (rc != SQLITE_DONE || hu_sql_txn_commit(&txn) != HU_OK) {
        hu_sql_txn_rollback(&txn);
        return HU_ERR_IO;
    }
    *out_id = id;
    /* P2 #7 — a new or retyped entity makes this contact's world-model cache stale. */
    if (invalidate)
        hu_world_model_invalidate(cid, (size_t)cid_len);
    return HU_OK;
}

#else

hu_error_t hu_graph_upsert_entity_typed(hu_graph_t *g, const char *contact_id,
                                        size_t contact_id_len, const char *name, size_t name_len,
                                        hu_entity_type_t type, const char *provenance,
                                        float confidence, unsigned flags, int64_t *out_id) {
    (void)out_id;
    (void)flags;
    (void)confidence;
    (void)provenance;
    (void)type;
    (void)name_len;
    (void)name;
    (void)contact_id_len;
    (void)contact_id;
    (void)g;
    return HU_ERR_NOT_SUPPORTED;
}

#endif
```

After `hu_entity_type_to_string` (outside any `#ifdef`), add the pure predicate:

```c
bool hu_graph_entity_retype_allowed(hu_entity_type_t old_type, hu_entity_type_t new_type) {
    switch (new_type) {
    case HU_ENTITY_PERSON:
    case HU_ENTITY_PLACE:
    case HU_ENTITY_ORGANIZATION:
    case HU_ENTITY_EVENT:
        return old_type == HU_ENTITY_UNKNOWN || old_type == HU_ENTITY_TOPIC;
    case HU_ENTITY_TOPIC:
    case HU_ENTITY_EMOTION:
        return old_type == HU_ENTITY_UNKNOWN;
    default:
        return false; /* UNKNOWN or out of range: never a retype target */
    }
}
```

(Check it against the truth table. A name type is only reachable from UNKNOWN or TOPIC, so TOPIC→TOPIC is false. TOPIC and EMOTION are only reachable from UNKNOWN. UNKNOWN as a target is never allowed. An out-of-range `old_type` never equals UNKNOWN or TOPIC.)

- [ ] **Step 5: Run the tests to verify they pass**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cmake --build "$W/build" --target human human_tests -j8 2>&1 | grep -E 'error|warning' | head
cd "$W" && ./build/human_tests --suite=graph 2>&1 | tail -3
```
Expected: no errors or warnings; the `graph` suites pass with 0 failed.

- [ ] **Step 6: Gates, full suite, commit**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
bash "$W/scripts/check-clone-ratchet.sh" && bash "$W/scripts/check-sqlite-includer-ratchet.sh"
cd "$W" && ./build/human_tests 2>&1 | grep -E 'Results:'
git -C "$W" add include/human/memory/graph.h src/memory/graph.c tests/test_graph.c
git -C "$W" commit -m "feat(graph): typed entity upsert with a no-downgrade retype policy

hu_graph_upsert_entity never changed an existing entity's type and no writer
set provenance/confidence, so the graph could not learn that 'Salim' is a
person. The typed upsert types on insert, upgrades only UNKNOWN/TOPIC rows,
never touches a name type or EMOTION, and lets the first writer own
provenance. NO_TOUCH is retype-only for the one-time migration.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
Expected: `Results:` shows 0 failed; the pre-commit hooks pass.

---

### Task 2: Importer — typed entity lines and CLI counts

**Files:**
- Modify: `include/human/memory/graph_ingest.h:71-80`
- Modify: `src/memory/graph_ingest.c:156-305`
- Modify: `src/app/cli_commands.c:433-462` (`memory_import_facts`)
- Test: `tests/test_cli_memory_import.c`

**Interfaces:**
- Consumes: `hu_graph_upsert_entity_typed`, `HU_GRAPH_UPSERT_NO_TOUCH` (Task 1); `hu_graph_name_is_self_placeholder`, `hu_graph_name_is_nonreferential`.
- Produces:
  - `hu_error_t hu_graph_import_facts_jsonl(hu_allocator_t *alloc, hu_graph_t *g, const char *path, const char *exclude, size_t *imported_out, size_t *entities_out, size_t *skipped_out);` It returns `HU_OK` iff `imported + entities > 0`, else `HU_ERR_NOT_FOUND`.
  - JSONL entity line: `{"kind":"entity","contact":str,"name":str,"type":"person"|"place"|"org"|"event"|"topic","source":str?,"confidence":num?,"retype_only":bool?}`.
  - CLI stdout: `{"imported": N, "entities": E, "skipped": M, "graph": "<path>"}`. Exit is non-zero when `N+E == 0`. Tasks 7 and 8 parse `"entities"`.

- [ ] **Step 1: Write the failing tests**

In `tests/test_cli_memory_import.c`, change the existing call in `test_import_facts_ingests_in_ts_order_and_supersedes` to the new signature:

```c
    size_t imported = 0, entities = 0, skipped = 0;
    HU_ASSERT_EQ(hu_graph_import_facts_jsonl(&alloc, g, jpath, "asking_about", &imported,
                                             &entities, &skipped),
                 HU_OK);
    HU_ASSERT_EQ((long)imported, 3L);
    HU_ASSERT_EQ((long)entities, 0L);
    HU_ASSERT_EQ((long)skipped, 1L);
```

Add `#include <sqlite3.h>` and `#include <stdbool.h>` after the existing includes (inside the `#ifdef HU_ENABLE_SQLITE`). Then add before `run_cli_memory_import_tests`:

```c
typedef struct ent_row {
    bool found;
    int type;
    int mentions;
    char provenance[64];
} ent_row_t;

static ent_row_t ent_row(hu_graph_t *g, const char *cid, const char *name) {
    ent_row_t r;
    memset(&r, 0, sizeof(r));
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(hu_graph_sqlite_connection(g),
                           "SELECT type, mention_count, COALESCE(provenance, '') FROM entities"
                           " WHERE contact_id = ?1 AND name = ?2",
                           -1, &q, NULL) != SQLITE_OK)
        return r;
    sqlite3_bind_text(q, 1, cid, -1, SQLITE_STATIC);
    sqlite3_bind_text(q, 2, name, -1, SQLITE_STATIC);
    if (sqlite3_step(q) == SQLITE_ROW) {
        r.found = true;
        r.type = sqlite3_column_int(q, 0);
        r.mentions = sqlite3_column_int(q, 1);
        snprintf(r.provenance, sizeof(r.provenance), "%s",
                 (const char *)sqlite3_column_text(q, 2));
    }
    sqlite3_finalize(q);
    return r;
}

#define ENT_CID "+15550000001"

/* Entity lines (spec §4.3) are typed through hu_graph_upsert_entity_typed and
 * counted apart from facts; a bad type, a missing contact and a placeholder
 * name are skipped, never guessed. */
static void test_import_entity_lines_are_typed_and_counted(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char jpath[128];
    snprintf(jpath, sizeof(jpath), "/tmp/hu_cli_import_ent_%d.jsonl", (int)getpid());
    write_file(jpath,
               "{\"kind\":\"entity\",\"contact\":\"" ENT_CID "\",\"name\":\"Salim\","
               "\"type\":\"person\",\"source\":\"names:nightly\",\"confidence\":0.8}\n"
               "{\"kind\":\"entity\",\"contact\":\"" ENT_CID "\",\"name\":\"lake house\","
               "\"type\":\"topic\",\"source\":\"names:nightly\",\"confidence\":0.8}\n"
               "{\"kind\":\"entity\",\"contact\":\"" ENT_CID "\",\"name\":\"Jupiter\","
               "\"type\":\"planet\"}\n"
               "{\"kind\":\"entity\",\"name\":\"Nobody\",\"type\":\"person\"}\n"
               "{\"kind\":\"entity\",\"contact\":\"" ENT_CID "\",\"name\":\"user\","
               "\"type\":\"person\"}\n"
               "{\"contact\":\"self\",\"subject\":\"user\",\"predicate\":\"works_at\","
               "\"object\":\"acme\",\"confidence\":0.8,\"ts\":150,\"source\":\"t:3\"}\n");
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "ignored-in-test", 15, &g), HU_OK);
    size_t imported = 0, entities = 0, skipped = 0;
    HU_ASSERT_EQ(
        hu_graph_import_facts_jsonl(&alloc, g, jpath, NULL, &imported, &entities, &skipped),
        HU_OK);
    HU_ASSERT_EQ((long)imported, 1L);
    HU_ASSERT_EQ((long)entities, 2L);
    HU_ASSERT_EQ((long)skipped, 3L); /* planet, missing contact, placeholder "user" */
    ent_row_t salim = ent_row(g, ENT_CID, "Salim");
    HU_ASSERT_TRUE(salim.found);
    HU_ASSERT_EQ(salim.type, (int)HU_ENTITY_PERSON);
    HU_ASSERT_STR_EQ(salim.provenance, "names:nightly");
    HU_ASSERT_EQ(ent_row(g, ENT_CID, "lake house").type, (int)HU_ENTITY_TOPIC);
    HU_ASSERT_FALSE(ent_row(g, ENT_CID, "Jupiter").found);
    HU_ASSERT_FALSE(ent_row(g, ENT_CID, "user").found);
    hu_graph_close(g, &alloc);
    unlink(jpath);
}

/* "retype_only": the migration types what exists and creates nothing. */
static void test_import_retype_only_line_retypes_without_touching_or_creating(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char jpath[128];
    snprintf(jpath, sizeof(jpath), "/tmp/hu_cli_import_rt_%d.jsonl", (int)getpid());
    write_file(jpath,
               "{\"kind\":\"entity\",\"contact\":\"" ENT_CID "\",\"name\":\"Vanguard\","
               "\"type\":\"org\",\"source\":\"names:migrate\",\"confidence\":0.6,"
               "\"retype_only\":true}\n"
               "{\"kind\":\"entity\",\"contact\":\"" ENT_CID "\",\"name\":\"Ghost\","
               "\"type\":\"person\",\"source\":\"names:migrate\",\"retype_only\":true}\n");
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "ignored-in-test", 15, &g), HU_OK);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, ENT_CID, strlen(ENT_CID), "Vanguard", 8,
                                        HU_ENTITY_UNKNOWN, NULL, &id),
                 HU_OK);
    size_t imported = 0, entities = 0, skipped = 0;
    HU_ASSERT_EQ(
        hu_graph_import_facts_jsonl(&alloc, g, jpath, NULL, &imported, &entities, &skipped),
        HU_OK);
    HU_ASSERT_EQ((long)entities, 1L);
    HU_ASSERT_EQ((long)skipped, 1L);
    ent_row_t v = ent_row(g, ENT_CID, "Vanguard");
    HU_ASSERT_EQ(v.type, (int)HU_ENTITY_ORGANIZATION);
    HU_ASSERT_EQ(v.mentions, 1);
    HU_ASSERT_STR_EQ(v.provenance, "names:migrate");
    HU_ASSERT_FALSE(ent_row(g, ENT_CID, "Ghost").found);
    hu_graph_close(g, &alloc);
    unlink(jpath);
}

/* Success iff N+E > 0, through the real subcommand: an entity-only file is a
 * real import; a file whose only line is invalid is not. */
static void test_import_entity_only_file_is_success_through_the_cli(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char gpath[128], jpath[128];
    snprintf(gpath, sizeof(gpath), "/tmp/hu_cli_import_eo_%d.db", (int)getpid());
    snprintf(jpath, sizeof(jpath), "/tmp/hu_cli_import_eo_%d.jsonl", (int)getpid());
    setenv("HU_GRAPH_DB", gpath, 1);
    char *argv[] = {"human", "memory", "import-facts", jpath, NULL};
    write_file(jpath, "{\"kind\":\"entity\",\"contact\":\"" ENT_CID "\",\"name\":\"Salim\","
                      "\"type\":\"person\"}\n");
    HU_ASSERT_EQ(cmd_memory(&alloc, 4, argv), HU_OK);
    write_file(jpath, "{\"kind\":\"entity\",\"contact\":\"" ENT_CID "\",\"name\":\"Salim\","
                      "\"type\":\"planet\"}\n");
    HU_ASSERT_NEQ(cmd_memory(&alloc, 4, argv), HU_OK);
    unlink(gpath);
    unlink(jpath);
    unsetenv("HU_GRAPH_DB");
}
```

Register them in `run_cli_memory_import_tests`:

```c
    HU_RUN_TEST(test_import_entity_lines_are_typed_and_counted);
    HU_RUN_TEST(test_import_retype_only_line_retypes_without_touching_or_creating);
    HU_RUN_TEST(test_import_entity_only_file_is_success_through_the_cli);
```

- [ ] **Step 2: Run to verify failure**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cmake --build "$W/build" --target human_tests -j8 2>&1 | grep -E 'error' | head
```
Expected: `too many arguments to function call` for `hu_graph_import_facts_jsonl`.

- [ ] **Step 3: Update the header**

Replace the `hu_graph_import_facts_jsonl` comment and prototype in `include/human/memory/graph_ingest.h` with:

```c
/* Import a JSONL file into `g`. Two line kinds:
 *  - fact lines (no "kind"): {contact, subject, predicate, object, confidence,
 *    ts, source} through hu_graph_ingest_fact, in ascending `ts` order so
 *    supersession is chronological. `exclude` is an optional comma-separated
 *    predicate list to skip (e.g. "asking_about").
 *  - entity lines (spec 2026-09-29 §4.3): {"kind":"entity", contact, name,
 *    type, source, confidence, retype_only} through
 *    hu_graph_upsert_entity_typed. type is exactly person|place|org|event|topic
 *    (case-insensitive); anything else, a missing contact/name, or a self
 *    placeholder / non-referential name skips the line. retype_only=true maps
 *    to HU_GRAPH_UPSERT_NO_TOUCH (the migration: no bump, never creates).
 * Counts are always written. Returns HU_ERR_NOT_FOUND when the file is
 * unreadable OR imported + entities == 0: an empty import must never look like
 * a finished one. */
hu_error_t hu_graph_import_facts_jsonl(hu_allocator_t *alloc, hu_graph_t *g, const char *path,
                                       const char *exclude, size_t *imported_out,
                                       size_t *entities_out, size_t *skipped_out);
```

- [ ] **Step 4: Implement the importer**

In `src/memory/graph_ingest.c`, add `#include <strings.h>` after `#include <string.h>`. Replace everything from `typedef struct import_fact {` through the closing brace of the SQLite `hu_graph_import_facts_jsonl` (just before `#else /* !HU_ENABLE_SQLITE */`) with:

```c
typedef struct import_fact {
    char contact[128];
    char subject[128];
    char predicate[64];
    char object[256];
    char source[128];
    float confidence;
    int64_t ts;
} import_fact_t;

typedef struct import_entity {
    char contact[128];
    char name[128];
    char source[64];
    hu_entity_type_t type;
    float confidence;
    bool retype_only;
} import_entity_t;

static int import_fact_cmp_ts(const void *a, const void *b) {
    int64_t x = ((const import_fact_t *)a)->ts, y = ((const import_fact_t *)b)->ts;
    return (x > y) - (x < y);
}

/* Double a heap array holding `n` elements of `elem` bytes. Returns the new
 * array (old one freed) or NULL on OOM (old one kept, *cap unchanged). */
static void *import_grow(hu_allocator_t *alloc, void *arr, size_t n, size_t *cap, size_t elem) {
    size_t ncap = *cap * 2;
    void *grown = alloc->alloc(alloc->ctx, ncap * elem);
    if (!grown)
        return NULL;
    memcpy(grown, arr, n * elem);
    alloc->free(alloc->ctx, arr, *cap * elem);
    *cap = ncap;
    return grown;
}

/* Entity-line types (spec 2026-09-29 §4.3): exactly these five. */
static bool import_entity_type(const char *s, hu_entity_type_t *out) {
    static const struct {
        const char *name;
        hu_entity_type_t type;
    } k_types[] = {
        {"person", HU_ENTITY_PERSON}, {"place", HU_ENTITY_PLACE}, {"org", HU_ENTITY_ORGANIZATION},
        {"event", HU_ENTITY_EVENT},   {"topic", HU_ENTITY_TOPIC},
    };
    for (size_t i = 0; s && i < sizeof(k_types) / sizeof(k_types[0]); i++) {
        if (strcasecmp(s, k_types[i].name) == 0) {
            *out = k_types[i].type;
            return true;
        }
    }
    return false;
}

/* An entity line -> *e. False (the line is skipped) on a missing or oversized
 * contact/name, an unknown type, or a name hu_graph_ingest_fact would also
 * refuse (self placeholder, non-referential). Never truncates a name. */
static bool import_parse_entity(const hu_json_value_t *v, import_entity_t *e) {
    const char *contact = hu_json_get_string(v, "contact");
    const char *name = hu_json_get_string(v, "name");
    const char *src = hu_json_get_string(v, "source");
    memset(e, 0, sizeof(*e));
    if (!contact || !contact[0] || strlen(contact) >= sizeof(e->contact) || !name || !name[0] ||
        strlen(name) >= sizeof(e->name) ||
        !import_entity_type(hu_json_get_string(v, "type"), &e->type))
        return false;
    size_t name_len = strlen(name);
    if (hu_graph_name_is_self_placeholder(name, name_len) ||
        hu_graph_name_is_nonreferential(name, name_len))
        return false;
    snprintf(e->contact, sizeof(e->contact), "%s", contact);
    snprintf(e->name, sizeof(e->name), "%s", name);
    snprintf(e->source, sizeof(e->source), "%s", src && src[0] ? src : "import");
    e->confidence = (float)hu_json_get_number(v, "confidence", 0.5);
    e->retype_only = hu_json_get_bool(v, "retype_only", false);
    return true;
}

/* One JSON object per line; see graph_ingest.h for both line kinds. Facts are
 * ingested in ts order so supersession is chronological (a lives_in from May
 * closes a lives_in from March, never the reverse); entity lines follow. */
hu_error_t hu_graph_import_facts_jsonl(hu_allocator_t *alloc, hu_graph_t *g, const char *path,
                                       const char *exclude, size_t *imported_out,
                                       size_t *entities_out, size_t *skipped_out) {
    if (imported_out)
        *imported_out = 0;
    if (entities_out)
        *entities_out = 0;
    if (skipped_out)
        *skipped_out = 0;
    if (!alloc || !g || !path)
        return HU_ERR_INVALID_ARGUMENT;
    FILE *fp = fopen(path, "r");
    if (!fp)
        return HU_ERR_NOT_FOUND;
    size_t cap = 256, n = 0, ecap = 16, ne = 0, skipped = 0;
    import_fact_t *facts = (import_fact_t *)alloc->alloc(alloc->ctx, cap * sizeof(*facts));
    import_entity_t *ents = (import_entity_t *)alloc->alloc(alloc->ctx, ecap * sizeof(*ents));
    if (!facts || !ents) {
        if (facts)
            alloc->free(alloc->ctx, facts, cap * sizeof(*facts));
        if (ents)
            alloc->free(alloc->ctx, ents, ecap * sizeof(*ents));
        fclose(fp);
        return HU_ERR_OUT_OF_MEMORY;
    }
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0)
            continue;
        hu_json_value_t *v = NULL;
        if (hu_json_parse(alloc, line, len, &v) != HU_OK || !v) {
            skipped++;
            continue;
        }
        const char *kind = hu_json_get_string(v, "kind");
        if (kind && strcmp(kind, "entity") == 0) {
            if (ne == ecap) {
                import_entity_t *ge = import_grow(alloc, ents, ne, &ecap, sizeof(*ents));
                if (!ge) {
                    hu_json_free(alloc, v);
                    break;
                }
                ents = ge;
            }
            if (import_parse_entity(v, &ents[ne]))
                ne++;
            else
                skipped++;
            hu_json_free(alloc, v);
            continue;
        }
        const char *pred = hu_json_get_string(v, "predicate");
        const char *obj = hu_json_get_string(v, "object");
        const char *subj = hu_json_get_string(v, "subject");
        const char *contact = hu_json_get_string(v, "contact");
        const char *src = hu_json_get_string(v, "source");
        bool excluded = false;
        if (exclude && pred) {
            /* comma-separated, whole-token match */
            const char *p = exclude;
            size_t pl = strlen(pred);
            while (*p) {
                const char *e = strchr(p, ',');
                size_t tl = e ? (size_t)(e - p) : strlen(p);
                if (tl == pl && strncmp(p, pred, pl) == 0) {
                    excluded = true;
                    break;
                }
                p = e ? e + 1 : p + tl;
            }
        }
        if (!pred || !obj || !obj[0] || excluded) {
            skipped++;
            hu_json_free(alloc, v);
            continue;
        }
        if (n == cap) {
            import_fact_t *gf = import_grow(alloc, facts, n, &cap, sizeof(*facts));
            if (!gf) {
                hu_json_free(alloc, v);
                break;
            }
            facts = gf;
        }
        import_fact_t *f = &facts[n++];
        memset(f, 0, sizeof(*f));
        snprintf(f->contact, sizeof(f->contact), "%s", contact && contact[0] ? contact : "self");
        snprintf(f->subject, sizeof(f->subject), "%s", subj && subj[0] ? subj : "user");
        snprintf(f->predicate, sizeof(f->predicate), "%s", pred);
        snprintf(f->object, sizeof(f->object), "%s", obj);
        snprintf(f->source, sizeof(f->source), "%s", src ? src : "import");
        f->confidence = (float)hu_json_get_number(v, "confidence", 0.5);
        f->ts = (int64_t)hu_json_get_number(v, "ts", 0);
        hu_json_free(alloc, v);
    }
    fclose(fp);
    qsort(facts, n, sizeof(*facts), import_fact_cmp_ts);

    size_t imported = 0, entities = 0;
    for (size_t i = 0; i < n; i++) {
        const import_fact_t *f = &facts[i];
        if (hu_graph_ingest_fact(g, f->contact, strlen(f->contact), f->subject, f->predicate,
                                 f->object, f->confidence, f->ts, f->source) == HU_OK)
            imported++;
        else
            skipped++;
    }
    for (size_t i = 0; i < ne; i++) {
        const import_entity_t *e = &ents[i];
        int64_t id = 0;
        if (hu_graph_upsert_entity_typed(g, e->contact, strlen(e->contact), e->name,
                                         strlen(e->name), e->type, e->source, e->confidence,
                                         e->retype_only ? HU_GRAPH_UPSERT_NO_TOUCH : 0u,
                                         &id) == HU_OK)
            entities++;
        else
            skipped++;
    }
    alloc->free(alloc->ctx, facts, cap * sizeof(*facts));
    alloc->free(alloc->ctx, ents, ecap * sizeof(*ents));
    if (imported_out)
        *imported_out = imported;
    if (entities_out)
        *entities_out = entities;
    if (skipped_out)
        *skipped_out = skipped;
    return imported + entities > 0 ? HU_OK : HU_ERR_NOT_FOUND;
}
```

Replace the non-SQLite stub of `hu_graph_import_facts_jsonl` with:

```c
hu_error_t hu_graph_import_facts_jsonl(hu_allocator_t *alloc, hu_graph_t *g, const char *path,
                                       const char *exclude, size_t *imported_out,
                                       size_t *entities_out, size_t *skipped_out) {
    (void)alloc;
    (void)g;
    (void)path;
    (void)exclude;
    if (imported_out)
        *imported_out = 0;
    if (entities_out)
        *entities_out = 0;
    if (skipped_out)
        *skipped_out = 0;
    return HU_ERR_NOT_SUPPORTED;
}
```

- [ ] **Step 5: Update the CLI**

In `src/app/cli_commands.c`, `memory_import_facts`, replace the lines from `size_t imported = 0, skipped = 0;` through `return err;` with:

```c
    size_t imported = 0, entities = 0, skipped = 0;
    err = hu_graph_import_facts_jsonl(alloc, g, argv[3], exclude, &imported, &entities, &skipped);
    hu_graph_close(g, alloc);
    printf("{\"imported\": %zu, \"entities\": %zu, \"skipped\": %zu, \"graph\": \"%s\"}\n",
           imported, entities, skipped, graph_path);
    if (err == HU_ERR_NOT_FOUND && imported == 0 && entities == 0)
        fprintf(stderr, "import-facts: nothing imported from %s\n", argv[3]);
    return err;
```

In the comment above `memory_import_facts`, add the line: `Entity lines ({"kind":"entity",...}) are typed via hu_graph_upsert_entity_typed.`

- [ ] **Step 6: Run to verify pass**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cmake --build "$W/build" --target human human_tests -j8 2>&1 | grep -E 'error|warning' | head
cd "$W" && ./build/human_tests --suite=cli_memory_import 2>&1 | tail -3
cd "$W" && ./build/human_tests --suite=graph_ingest 2>&1 | tail -3
```
Expected: both suites pass, 0 failed.

- [ ] **Step 7: Gates, full suite, commit**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
bash "$W/scripts/check-clone-ratchet.sh" && bash "$W/scripts/check-sqlite-includer-ratchet.sh"
cd "$W" && ./build/human_tests 2>&1 | grep -E 'Results:'
git -C "$W" add include/human/memory/graph_ingest.h src/memory/graph_ingest.c src/app/cli_commands.c tests/test_cli_memory_import.c
git -C "$W" commit -m "feat(names): typed entity lines in the JSONL importer

The importer had no type field and always created objects UNKNOWN, so no
offline pass could teach the graph a name's type. Entity lines go through
the typed upsert (retype_only -> NO_TOUCH); the CLI reports entities apart
from facts and succeeds iff anything landed.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Per-turn name catcher (pure extractor + daemon wiring + `HU_NAME_CATCH`)

This is one task because `name_extract.o` has no product caller until the daemon calls it. A separate commit would grow the dead-strip counter A and fail pre-commit.

**Files:**
- Create: `include/human/memory/name_extract.h`, `src/memory/name_extract.c`
- Create: `include/human/daemon/name_catch.h`, `src/daemon/daemon_name_catch.c`
- Modify: `CMakeLists.txt` (add `src/memory/name_extract.c` after `src/memory/graph_ingest.c`; add `src/daemon/daemon_name_catch.c` after `src/daemon/daemon_comfort_summary.c`; add `tests/test_name_extract.c` after `tests/test_graph_ingest.c`; add `tests/test_daemon_name_catch.c` after `tests/test_daemon_identity_graph.c`)
- Modify: `src/daemon.c` (one include; a ≤7-line call after the `hu_daemon_store_conversation_summary` block at ~:8274-8279)
- Modify: `tests/test_main.c` (declare and call `run_name_extract_tests`, `run_daemon_name_catch_tests`)
- Test: `tests/test_name_extract.c`, `tests/test_daemon_name_catch.c`

**Interfaces:**
- Consumes: `hu_graph_upsert_entity_typed` (Task 1), `hu_graph_upsert_entity`, `hu_graph_list_entities`, `hu_graph_entities_free`, `hu_gate_mode_from_env`, `hu_log_info_once`.
- Produces:
  - `typedef enum { HU_NAME_KNOWN = 0, HU_NAME_CAPITALIZED } hu_name_kind_t;`
  - `typedef struct hu_name_ref { const char *name; size_t len; } hu_name_ref_t;`
  - `typedef struct hu_name_candidate { const char *name; size_t len; hu_name_kind_t kind; } hu_name_candidate_t;`
  - `bool hu_name_entity_is_nameable(hu_entity_type_t type, const char *name, size_t len);` Task 5 uses it for grounding.
  - `size_t hu_name_extract(const char *text, size_t len, const hu_name_ref_t *known, size_t known_count, hu_name_candidate_t *out, size_t out_cap);`
  - `hu_gate_mode_t hu_name_catch_mode(void);`, `hu_name_catch_action_t hu_name_catch_action(hu_gate_mode_t, hu_name_kind_t);`, `hu_error_t hu_daemon_name_catch(...)`, `void hu_daemon_name_catch_tick(...)`.

- [ ] **Step 1: Write the extractor's failing tests** — `tests/test_name_extract.c`

```c
/* The zero-model per-turn name catcher (spec 2026-09-29 §4.2): KNOWN names of
 * the contact matched at word boundaries, and new Capitalized 1-3 token runs
 * that are not at a sentence start and not stopwords. */
#include "human/memory/name_extract.h"
#include "test_framework.h"
#include <stdbool.h>
#include <string.h>

static size_t extract(const char *text, const hu_name_ref_t *known, size_t kn,
                      hu_name_candidate_t *out, size_t cap) {
    return hu_name_extract(text, strlen(text), known, kn, out, cap);
}

static bool cand_is(const hu_name_candidate_t *c, const char *name, hu_name_kind_t kind) {
    return c->len == strlen(name) && memcmp(c->name, name, c->len) == 0 && c->kind == kind;
}

static void test_nameable_truth_table(void) {
    HU_ASSERT_TRUE(hu_name_entity_is_nameable(HU_ENTITY_PERSON, "salim", 5));
    HU_ASSERT_TRUE(hu_name_entity_is_nameable(HU_ENTITY_PLACE, "tampa", 5));
    HU_ASSERT_TRUE(hu_name_entity_is_nameable(HU_ENTITY_ORGANIZATION, "Acme", 4));
    HU_ASSERT_TRUE(hu_name_entity_is_nameable(HU_ENTITY_EVENT, "Coachella", 9));
    HU_ASSERT_TRUE(hu_name_entity_is_nameable(HU_ENTITY_UNKNOWN, "Zed Corp", 8));
    HU_ASSERT_FALSE(hu_name_entity_is_nameable(HU_ENTITY_UNKNOWN, "different direction", 19));
    HU_ASSERT_FALSE(hu_name_entity_is_nameable(HU_ENTITY_TOPIC, "Pickleball", 10));
    HU_ASSERT_FALSE(hu_name_entity_is_nameable(HU_ENTITY_EMOTION, "Grief", 5));
    HU_ASSERT_FALSE(hu_name_entity_is_nameable(HU_ENTITY_PERSON, NULL, 0));
    HU_ASSERT_FALSE(hu_name_entity_is_nameable((hu_entity_type_t)42, "X", 1));
}

static void test_extract_capitalized_mid_sentence(void) {
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("had lunch with Salim yesterday", NULL, 0, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Salim", HU_NAME_CAPITALIZED));
    HU_ASSERT_EQ((long)extract("went to Tampa Bay with Priya", NULL, 0, c, 8), 2L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Tampa Bay", HU_NAME_CAPITALIZED));
    HU_ASSERT_TRUE(cand_is(&c[1], "Priya", HU_NAME_CAPITALIZED));
    HU_ASSERT_EQ((long)extract("dinner at Priya's place", NULL, 0, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Priya", HU_NAME_CAPITALIZED)); /* possessive stripped */
    HU_ASSERT_EQ((long)extract("I saw Jo", NULL, 0, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Jo", HU_NAME_CAPITALIZED)); /* 2 chars is the floor */
}

static void test_extract_sentence_start_is_not_a_name(void) {
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("Salim came by", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("ok. Salim came by", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("wow!\nSalim came by", NULL, 0, c, 8), 0L);
}

static void test_extract_stoplist_but_mom_and_dad_allowed(void) {
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("see you Monday in March", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("thanks God lol Hey", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("call Mom later and Dad too", NULL, 0, c, 8), 2L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Mom", HU_NAME_CAPITALIZED));
    HU_ASSERT_TRUE(cand_is(&c[1], "Dad", HU_NAME_CAPITALIZED));
}

static void test_extract_known_names_word_boundary_and_lowercase(void) {
    hu_name_ref_t known[] = {{"Al", 2}, {"tampa", 5}};
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("Also that", known, 2, c, 8), 0L); /* "Al" is not in "Also" */
    HU_ASSERT_EQ((long)extract("saw al back in TAMPA", known, 2, c, 8), 2L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Al", HU_NAME_KNOWN));
    HU_ASSERT_TRUE(cand_is(&c[1], "tampa", HU_NAME_KNOWN)); /* the stored spelling */
}

static void test_extract_known_wins_over_capitalized_duplicate(void) {
    hu_name_ref_t known[] = {{"Salim", 5}};
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("lunch with Salim", known, 1, c, 8), 1L);
    HU_ASSERT_TRUE(cand_is(&c[0], "Salim", HU_NAME_KNOWN));
}

static void test_extract_respects_out_cap_and_null_inputs(void) {
    hu_name_candidate_t c[2];
    HU_ASSERT_EQ((long)extract("saw Ann and Bob and Cal and Dee", NULL, 0, c, 2), 2L);
    HU_ASSERT_EQ((long)hu_name_extract(NULL, 5, NULL, 0, c, 2), 0L);
    HU_ASSERT_EQ((long)hu_name_extract("saw Ann", 7, NULL, 0, NULL, 2), 0L);
    HU_ASSERT_EQ((long)hu_name_extract("saw Ann", 7, NULL, 0, c, 0), 0L);
}

/* Review Focus 1: an ASCII fragment of a non-ASCII word is not a name. */
static void test_extract_non_ascii_glued_token_is_not_a_candidate(void) {
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("met Jos\xc3\xa9 today", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("going to Zo\xc3\xab's party", NULL, 0, c, 8), 0L);
}

/* Review Focus 5: all-caps is not Capitalized; a 4-token title run is not a name. */
static void test_extract_all_caps_and_long_title_runs_are_not_names(void) {
    hu_name_candidate_t c[8];
    HU_ASSERT_EQ((long)extract("omg SALIM IS HERE", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("we said Happy New Year Everyone", NULL, 0, c, 8), 0L);
    HU_ASSERT_EQ((long)extract("at the NYC office", NULL, 0, c, 8), 0L);
}

void run_name_extract_tests(void) {
    HU_TEST_SUITE("name_extract");
    HU_RUN_TEST(test_nameable_truth_table);
    HU_RUN_TEST(test_extract_capitalized_mid_sentence);
    HU_RUN_TEST(test_extract_sentence_start_is_not_a_name);
    HU_RUN_TEST(test_extract_stoplist_but_mom_and_dad_allowed);
    HU_RUN_TEST(test_extract_known_names_word_boundary_and_lowercase);
    HU_RUN_TEST(test_extract_known_wins_over_capitalized_duplicate);
    HU_RUN_TEST(test_extract_respects_out_cap_and_null_inputs);
    HU_RUN_TEST(test_extract_non_ascii_glued_token_is_not_a_candidate);
    HU_RUN_TEST(test_extract_all_caps_and_long_title_runs_are_not_names);
}
```

- [ ] **Step 2: Write the catcher's failing tests** — `tests/test_daemon_name_catch.c`

```c
/* Per-turn name catcher wiring (spec 2026-09-29 §4.2): HU_NAME_CATCH gate,
 * pure write decision, graph effects per mode, and the daemon call site
 * feeding the contact's inbound text only. */
#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/daemon/name_catch.h"
#include "human/memory/graph.h"
#include "test_framework.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_name_catch_action_truth_table(void) {
    HU_ASSERT_EQ((int)hu_name_catch_action(HU_GATE_OFF, HU_NAME_KNOWN), (int)HU_NAME_CATCH_SKIP);
    HU_ASSERT_EQ((int)hu_name_catch_action(HU_GATE_OFF, HU_NAME_CAPITALIZED),
                 (int)HU_NAME_CATCH_SKIP);
    HU_ASSERT_EQ((int)hu_name_catch_action(HU_GATE_SHADOW, HU_NAME_KNOWN),
                 (int)HU_NAME_CATCH_SKIP);
    HU_ASSERT_EQ((int)hu_name_catch_action(HU_GATE_SHADOW, HU_NAME_CAPITALIZED),
                 (int)HU_NAME_CATCH_SKIP);
    HU_ASSERT_EQ((int)hu_name_catch_action(HU_GATE_LIVE, HU_NAME_KNOWN), (int)HU_NAME_CATCH_BUMP);
    HU_ASSERT_EQ((int)hu_name_catch_action(HU_GATE_LIVE, HU_NAME_CAPITALIZED),
                 (int)HU_NAME_CATCH_INSERT);
}

static void test_name_catch_mode_defaults_off(void) {
    unsetenv("HU_NAME_CATCH");
    HU_ASSERT_EQ((int)hu_name_catch_mode(), (int)HU_GATE_OFF);
    setenv("HU_NAME_CATCH", "shadow", 1);
    HU_ASSERT_EQ((int)hu_name_catch_mode(), (int)HU_GATE_SHADOW);
    setenv("HU_NAME_CATCH", "live", 1);
    HU_ASSERT_EQ((int)hu_name_catch_mode(), (int)HU_GATE_LIVE);
    setenv("HU_NAME_CATCH", "garbage", 1);
    HU_ASSERT_EQ((int)hu_name_catch_mode(), (int)HU_GATE_OFF);
    unsetenv("HU_NAME_CATCH");
}

static void test_bad_args_are_rejected_and_counts_zeroed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_name_catch_counts_t c = {9, 9, 9};
    HU_ASSERT_EQ(hu_daemon_name_catch(&alloc, NULL, HU_GATE_LIVE, "c", 1, "hi Ann", 6, &c),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((long)(c.known + c.fresh + c.written), 0L);
}

/* The call site is compiled out of test builds (HU_IS_TEST), so pin it by
 * source: the catcher is fed each raw inbound message, never the reply
 * (no self-reinforcing hallucination). Source-presence style, like
 * test_gate_comment_exists_at_agent_turn_1471. */
static void test_daemon_feeds_the_catcher_inbound_text_only(void) {
    FILE *f = fopen("src/daemon.c", "r");
    HU_ASSERT_NOT_NULL(f);
    char line[512], call[1024] = {0};
    bool in_call = false;
    while (fgets(line, sizeof(line), f)) {
        if (!in_call && strstr(line, "hu_daemon_name_catch_tick(") != NULL)
            in_call = true;
        if (in_call) {
            strncat(call, line, sizeof(call) - strlen(call) - 1);
            if (strchr(line, ';') != NULL)
                break;
        }
    }
    fclose(f);
    HU_ASSERT_STR_CONTAINS(call, "msgs[b].content");
    HU_ASSERT_STR_NOT_CONTAINS(call, "response");
    HU_ASSERT_STR_NOT_CONTAINS(call, "combined");
}

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

#define NC_CID "+15550001111"

typedef struct nc_row {
    bool found;
    int type;
    int mentions;
    char provenance[32];
    double confidence;
} nc_row_t;

static nc_row_t nc_row(hu_graph_t *g, const char *cid, const char *name) {
    nc_row_t r;
    memset(&r, 0, sizeof(r));
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(hu_graph_sqlite_connection(g),
                           "SELECT type, mention_count, COALESCE(provenance, ''), confidence"
                           " FROM entities WHERE contact_id = ?1 AND name = ?2",
                           -1, &q, NULL) != SQLITE_OK)
        return r;
    sqlite3_bind_text(q, 1, cid, -1, SQLITE_STATIC);
    sqlite3_bind_text(q, 2, name, -1, SQLITE_STATIC);
    if (sqlite3_step(q) == SQLITE_ROW) {
        r.found = true;
        r.type = sqlite3_column_int(q, 0);
        r.mentions = sqlite3_column_int(q, 1);
        snprintf(r.provenance, sizeof(r.provenance), "%s",
                 (const char *)sqlite3_column_text(q, 2));
        r.confidence = sqlite3_column_double(q, 3);
    }
    sqlite3_finalize(q);
    return r;
}

/* NC_CID knows Salim (PERSON), tampa (lowercase PLACE) and Pickleball (TOPIC). */
static hu_graph_t *nc_graph(hu_allocator_t *alloc) {
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(alloc, ":memory:", 8, &g), HU_OK);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, NC_CID, strlen(NC_CID), "Salim", 5, HU_ENTITY_PERSON,
                                        NULL, &id),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, NC_CID, strlen(NC_CID), "tampa", 5, HU_ENTITY_PLACE,
                                        NULL, &id),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, NC_CID, strlen(NC_CID), "Pickleball", 10,
                                        HU_ENTITY_TOPIC, NULL, &id),
                 HU_OK);
    return g;
}

static const char k_text[] = "saw Salim and Priya back in tampa";

static void test_live_bumps_known_and_inserts_new(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = nc_graph(&alloc);
    hu_name_catch_counts_t c;
    HU_ASSERT_EQ(hu_daemon_name_catch(&alloc, g, HU_GATE_LIVE, NC_CID, strlen(NC_CID), k_text,
                                      strlen(k_text), &c),
                 HU_OK);
    HU_ASSERT_EQ((long)c.known, 2L); /* Salim, tampa */
    HU_ASSERT_EQ((long)c.fresh, 1L); /* Priya */
    HU_ASSERT_EQ((long)c.written, 3L);
    HU_ASSERT_EQ(nc_row(g, NC_CID, "Salim").mentions, 2);
    HU_ASSERT_EQ(nc_row(g, NC_CID, "tampa").mentions, 2);
    nc_row_t p = nc_row(g, NC_CID, "Priya");
    HU_ASSERT_TRUE(p.found);
    HU_ASSERT_EQ(p.type, (int)HU_ENTITY_UNKNOWN);
    HU_ASSERT_STR_EQ(p.provenance, "names:turn");
    HU_ASSERT_FLOAT_EQ(p.confidence, 0.3, 1e-6);
    hu_graph_close(g, &alloc);
}

static void test_shadow_counts_but_writes_nothing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = nc_graph(&alloc);
    hu_name_catch_counts_t c;
    HU_ASSERT_EQ(hu_daemon_name_catch(&alloc, g, HU_GATE_SHADOW, NC_CID, strlen(NC_CID), k_text,
                                      strlen(k_text), &c),
                 HU_OK);
    HU_ASSERT_EQ((long)c.known, 2L);
    HU_ASSERT_EQ((long)c.fresh, 1L);
    HU_ASSERT_EQ((long)c.written, 0L);
    HU_ASSERT_EQ(nc_row(g, NC_CID, "Salim").mentions, 1);
    HU_ASSERT_FALSE(nc_row(g, NC_CID, "Priya").found);
    hu_graph_close(g, &alloc);
}

static void test_off_does_nothing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = nc_graph(&alloc);
    hu_name_catch_counts_t c;
    HU_ASSERT_EQ(hu_daemon_name_catch(&alloc, g, HU_GATE_OFF, NC_CID, strlen(NC_CID), k_text,
                                      strlen(k_text), &c),
                 HU_OK);
    HU_ASSERT_EQ((long)(c.known + c.fresh + c.written), 0L);
    HU_ASSERT_FALSE(nc_row(g, NC_CID, "Priya").found);
    hu_graph_close(g, &alloc);
}

static void test_topic_names_are_not_known(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = nc_graph(&alloc);
    hu_name_catch_counts_t c;
    const char *text = "more pickleball tonight";
    HU_ASSERT_EQ(hu_daemon_name_catch(&alloc, g, HU_GATE_LIVE, NC_CID, strlen(NC_CID), text,
                                      strlen(text), &c),
                 HU_OK);
    HU_ASSERT_EQ((long)c.known, 0L);
    HU_ASSERT_EQ(nc_row(g, NC_CID, "Pickleball").mentions, 1);
    hu_graph_close(g, &alloc);
}

/* Review Focus 4: a contact with no graph yet still gets their names recorded. */
static void test_first_message_from_empty_contact_records_names(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, ":memory:", 8, &g), HU_OK);
    hu_name_catch_counts_t c;
    const char *cid = "+15550002222", *text = "dinner with Priya";
    HU_ASSERT_EQ(hu_daemon_name_catch(&alloc, g, HU_GATE_LIVE, cid, strlen(cid), text,
                                      strlen(text), &c),
                 HU_OK);
    HU_ASSERT_EQ((long)c.fresh, 1L);
    HU_ASSERT_EQ((long)c.written, 1L);
    HU_ASSERT_TRUE(nc_row(g, cid, "Priya").found);
    hu_graph_close(g, &alloc);
}

static void test_tick_follows_the_env_gate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = nc_graph(&alloc);
    const char *text = "dinner with Priya";
    unsetenv("HU_NAME_CATCH");
    hu_daemon_name_catch_tick(&alloc, g, NC_CID, strlen(NC_CID), text, strlen(text));
    HU_ASSERT_FALSE(nc_row(g, NC_CID, "Priya").found);
    setenv("HU_NAME_CATCH", "live", 1);
    hu_daemon_name_catch_tick(&alloc, g, NC_CID, strlen(NC_CID), text, strlen(text));
    unsetenv("HU_NAME_CATCH");
    HU_ASSERT_TRUE(nc_row(g, NC_CID, "Priya").found);
    hu_graph_close(g, &alloc);
}
#endif /* HU_ENABLE_SQLITE */

void run_daemon_name_catch_tests(void) {
    HU_TEST_SUITE("daemon_name_catch");
    HU_RUN_TEST(test_name_catch_action_truth_table);
    HU_RUN_TEST(test_name_catch_mode_defaults_off);
    HU_RUN_TEST(test_bad_args_are_rejected_and_counts_zeroed);
    HU_RUN_TEST(test_daemon_feeds_the_catcher_inbound_text_only);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_live_bumps_known_and_inserts_new);
    HU_RUN_TEST(test_shadow_counts_but_writes_nothing);
    HU_RUN_TEST(test_off_does_nothing);
    HU_RUN_TEST(test_topic_names_are_not_known);
    HU_RUN_TEST(test_first_message_from_empty_contact_records_names);
    HU_RUN_TEST(test_tick_follows_the_env_gate);
#endif
}
```

Register both test files. In `CMakeLists.txt`, add `    tests/test_name_extract.c` after `    tests/test_graph_ingest.c` and `    tests/test_daemon_name_catch.c` after `    tests/test_daemon_identity_graph.c`. In `tests/test_main.c`, add `void run_name_extract_tests(void);` after `void run_graph_ingest_tests(void);` and `void run_daemon_name_catch_tests(void);` after `void run_daemon_contact_optout_tests(void);`. Add the calls `run_name_extract_tests();` after `run_graph_ingest_tests();` and `run_daemon_name_catch_tests();` after `run_daemon_contact_optout_tests();`.

- [ ] **Step 3: Run to verify failure**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cmake --build "$W/build" --target human_tests -j8 2>&1 | grep -E 'error' | head
```
Expected: `'human/memory/name_extract.h' file not found`.

- [ ] **Step 4: Create `include/human/memory/name_extract.h`**

```c
#ifndef HU_MEMORY_NAME_EXTRACT_H
#define HU_MEMORY_NAME_EXTRACT_H

#include "human/memory/graph.h"
#include <stdbool.h>
#include <stddef.h>

/* Zero-model per-turn name catcher (spec 2026-09-29 §4.2). Pure: no
 * allocation, no graph, no model. */

typedef enum hu_name_kind {
    HU_NAME_KNOWN = 0,   /* an existing entity name of this contact appears in the text */
    HU_NAME_CAPITALIZED, /* a new Capitalized 1-3 token run */
} hu_name_kind_t;

typedef struct hu_name_ref {
    const char *name;
    size_t len;
} hu_name_ref_t;

/* `name` points into the caller's `text` (CAPITALIZED) or into `known[]`
 * (KNOWN, the stored spelling) — valid only while those live. */
typedef struct hu_name_candidate {
    const char *name;
    size_t len;
    hu_name_kind_t kind;
} hu_name_candidate_t;

#define HU_NAME_MIN_LEN    2
#define HU_NAME_MAX_LEN    40
#define HU_NAME_MAX_TOKENS 3

/* Entities that count as NAMES (spec §4.2 KNOWN, §4.6 LIVE seeding):
 * PERSON, PLACE, ORGANIZATION, EVENT in any case, or an UNKNOWN whose first
 * byte is an ASCII capital. Never TOPIC or EMOTION. */
bool hu_name_entity_is_nameable(hu_entity_type_t type, const char *name, size_t len);

/* Up to `out_cap` candidates from `text`, KNOWN first, one per name
 * (case-insensitive):
 *  - KNOWN: a `known` name found case-insensitively at word boundaries.
 *  - CAPITALIZED: a run of 1-3 tokens separated by single spaces, each an
 *    ASCII capital followed by a lowercase letter ("Priya's" -> "Priya"),
 *    not at a sentence start (text start or after . ! ? newline), not a
 *    stopword (I, days, months, greetings Hey/Hi/Ok/Lol/Yeah/Thanks, God;
 *    "Mom"/"Dad" allowed), 2-40 bytes, and not glued to a non-ASCII byte. */
size_t hu_name_extract(const char *text, size_t len, const hu_name_ref_t *known,
                       size_t known_count, hu_name_candidate_t *out, size_t out_cap);

#endif /* HU_MEMORY_NAME_EXTRACT_H */
```

- [ ] **Step 5: Create `src/memory/name_extract.c`**

```c
/* name_extract.c — zero-model per-turn name catcher (spec 2026-09-29 §4.2).
 * See include/human/memory/name_extract.h. Precision over recall: a missed
 * name is caught by the nightly typed pass; a wrong one would ground a reply. */
#include "human/memory/name_extract.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

bool hu_name_entity_is_nameable(hu_entity_type_t type, const char *name, size_t len) {
    if (!name || len == 0)
        return false;
    switch (type) {
    case HU_ENTITY_PERSON:
    case HU_ENTITY_PLACE:
    case HU_ENTITY_ORGANIZATION:
    case HU_ENTITY_EVENT:
        return true;
    case HU_ENTITY_UNKNOWN:
        return name[0] >= 'A' && name[0] <= 'Z';
    default:
        return false; /* TOPIC, EMOTION, out of range */
    }
}

/* Capitalized words that are not names. "Mom" and "Dad" are deliberately
 * absent: they are how people name their parents. */
static bool ne_is_stopword(const char *w, size_t len) {
    static const char *const k_stop[] = {
        "I",       "I'm",      "I'll",     "I've",     "I'd",       "Monday",   "Tuesday",
        "Wednesday", "Thursday", "Friday", "Saturday", "Sunday",    "January",  "February",
        "March",   "April",    "May",      "June",     "July",      "August",   "September",
        "October", "November", "December", "Hey",      "Hi",        "Hello",    "Ok",
        "Okay",    "Lol",      "Lmao",     "Yeah",     "Yes",       "Yep",      "No",
        "Nope",    "Thanks",   "Thank",    "God",      "Omg",       "Oh",       "Haha",
        "Sorry",   "Please"};
    for (size_t i = 0; i < sizeof(k_stop) / sizeof(k_stop[0]); i++) {
        if (strlen(k_stop[i]) == len && memcmp(k_stop[i], w, len) == 0)
            return true;
    }
    return false;
}

/* Case-insensitive occurrence of needle bounded by non-alnum bytes (or the
 * ends) on both sides: "Al" is not in "Also" (substring-classifier-pitfalls). */
static bool ne_has_word(const char *hay, size_t hay_len, const char *needle, size_t nl) {
    if (nl == 0 || nl > hay_len)
        return false;
    for (size_t at = 0; at + nl <= hay_len; at++) {
        bool open_edge = at == 0 || !isalnum((unsigned char)hay[at - 1]);
        bool close_edge = at + nl == hay_len || !isalnum((unsigned char)hay[at + nl]);
        if (open_edge && close_edge && strncasecmp(hay + at, needle, nl) == 0)
            return true;
    }
    return false;
}

/* Append unless full or already present case-insensitively (KNOWN goes first,
 * so a KNOWN match wins over the same Capitalized run). */
static size_t ne_add(hu_name_candidate_t *out, size_t n, size_t cap, const char *name,
                     size_t len, hu_name_kind_t kind) {
    if (n >= cap)
        return n;
    for (size_t i = 0; i < n; i++) {
        if (out[i].len == len && strncasecmp(out[i].name, name, len) == 0)
            return n;
    }
    out[n].name = name;
    out[n].len = len;
    out[n].kind = kind;
    return n + 1;
}

typedef struct ne_run {
    size_t start;
    size_t end;
    size_t tokens;
    bool at_sentence_start;
} ne_run_t;

/* Emit the open Capitalized run if it qualifies, then close it. */
static size_t ne_flush(const char *text, ne_run_t *run, hu_name_candidate_t *out, size_t n,
                       size_t cap) {
    size_t run_len = run->end - run->start;
    if (run->tokens >= 1 && run->tokens <= HU_NAME_MAX_TOKENS && !run->at_sentence_start &&
        run_len >= HU_NAME_MIN_LEN && run_len <= HU_NAME_MAX_LEN)
        n = ne_add(out, n, cap, text + run->start, run_len, HU_NAME_CAPITALIZED);
    run->tokens = 0;
    return n;
}

size_t hu_name_extract(const char *text, size_t len, const hu_name_ref_t *known,
                       size_t known_count, hu_name_candidate_t *out, size_t out_cap) {
    if (!text || len == 0 || !out || out_cap == 0)
        return 0;
    size_t n = 0;
    for (size_t k = 0; known && k < known_count; k++) {
        if (known[k].name && known[k].len > 0 &&
            ne_has_word(text, len, known[k].name, known[k].len))
            n = ne_add(out, n, out_cap, known[k].name, known[k].len, HU_NAME_KNOWN);
    }

    ne_run_t run = {0, 0, 0, false};
    bool sentence_start = true;
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)text[i];
        if (!isalpha(c)) {
            if (c == '.' || c == '!' || c == '?' || c == '\n')
                sentence_start = true;
            else if (isdigit(c) || c >= 0x80)
                sentence_start = false; /* content, not punctuation */
            /* A run continues only across exactly one space. */
            if (run.tokens > 0 && !(c == ' ' && i == run.end))
                n = ne_flush(text, &run, out, n, out_cap);
            i++;
            continue;
        }
        size_t s = i;
        while (i < len && (isalpha((unsigned char)text[i]) || text[i] == '\''))
            i++;
        size_t tl = i - s;
        if (tl > 2 && text[s + tl - 2] == '\'' &&
            (text[s + tl - 1] == 's' || text[s + tl - 1] == 'S'))
            tl -= 2; /* possessive: Priya's -> Priya */
        bool starts_sentence = sentence_start;
        sentence_start = false;
        /* Letters glued to a non-ASCII byte belong to a word this scanner
         * cannot read ("José" would become "Jos"): never a candidate. */
        bool clean = (s == 0 || (unsigned char)text[s - 1] < 0x80) &&
                     (i == len || (unsigned char)text[i] < 0x80);
        bool cap = clean && tl >= 2 && isupper((unsigned char)text[s]) &&
                   islower((unsigned char)text[s + 1]) && !ne_is_stopword(text + s, tl);
        if (!cap) {
            if (run.tokens > 0)
                n = ne_flush(text, &run, out, n, out_cap);
            continue;
        }
        if (run.tokens > 0 && s == run.end + 1 && text[run.end] == ' ') {
            run.end = s + tl;
            run.tokens++;
            continue;
        }
        if (run.tokens > 0)
            n = ne_flush(text, &run, out, n, out_cap);
        run.start = s;
        run.end = s + tl;
        run.tokens = 1;
        run.at_sentence_start = starts_sentence;
    }
    if (run.tokens > 0)
        n = ne_flush(text, &run, out, n, out_cap);
    return n;
}
```

- [ ] **Step 6: Create `include/human/daemon/name_catch.h`**

```c
#ifndef HU_DAEMON_NAME_CATCH_H
#define HU_DAEMON_NAME_CATCH_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include "human/memory/graph.h"
#include "human/memory/name_extract.h"
#include <stddef.h>

/* Per-turn name catcher (spec 2026-09-29 §4.2). Zero-model. Reads the
 * CONTACT's inbound text only — never the generated reply, so the daemon
 * cannot reinforce its own hallucinations. In LIVE it bumps known names
 * (freshness) and records new Capitalized names as UNKNOWN entities with
 * provenance "names:turn". */

#define HU_NAME_CATCH_PROVENANCE  "names:turn"
#define HU_NAME_CATCH_CONFIDENCE  0.3f
#define HU_NAME_CATCH_MAX         8   /* candidates per inbound message */
#define HU_NAME_CATCH_KNOWN_LIMIT 256 /* known names considered per contact */

typedef enum hu_name_catch_action {
    HU_NAME_CATCH_SKIP = 0,
    HU_NAME_CATCH_BUMP,   /* known name: hu_graph_upsert_entity freshness bump */
    HU_NAME_CATCH_INSERT, /* new name: typed upsert, UNKNOWN, names:turn, 0.3 */
} hu_name_catch_action_t;

typedef struct hu_name_catch_counts {
    size_t known;   /* KNOWN candidates seen */
    size_t fresh;   /* CAPITALIZED (new) candidates seen */
    size_t written; /* graph writes that succeeded (LIVE only) */
} hu_name_catch_counts_t;

/* HU_NAME_CATCH per hu_gate_mode_parse; unset -> OFF. */
hu_gate_mode_t hu_name_catch_mode(void);

/* Pure write decision: OFF and SHADOW never write; LIVE bumps a KNOWN name
 * and inserts a CAPITALIZED one. */
hu_name_catch_action_t hu_name_catch_action(hu_gate_mode_t mode, hu_name_kind_t kind);

/* One inbound message for `contact_id` at `mode`. Known names are the
 * contact's top HU_NAME_CATCH_KNOWN_LIMIT entities by mentions that
 * hu_name_entity_is_nameable accepts. `out` is always written. Returns the
 * first graph-write error (later candidates are still attempted). */
hu_error_t hu_daemon_name_catch(hu_allocator_t *alloc, hu_graph_t *g, hu_gate_mode_t mode,
                                const char *contact_id, size_t contact_id_len,
                                const char *inbound, size_t inbound_len,
                                hu_name_catch_counts_t *out);

/* Daemon entry point: resolves HU_NAME_CATCH, logs once per process whether
 * the catcher is disabled (naming the env key) or active, logs
 * "name_catch shadow: known=%zu new=%zu (not written)" in SHADOW, and logs
 * and swallows errors so a reply is never blocked. */
void hu_daemon_name_catch_tick(hu_allocator_t *alloc, hu_graph_t *g, const char *contact_id,
                               size_t contact_id_len, const char *inbound, size_t inbound_len);

#endif /* HU_DAEMON_NAME_CATCH_H */
```

- [ ] **Step 7: Create `src/daemon/daemon_name_catch.c`**

```c
/* daemon_name_catch.c — per-turn name catcher wiring (spec 2026-09-29 §4.2).
 * See include/human/daemon/name_catch.h. Graph API only (no sqlite3.h). */
#include "human/daemon/name_catch.h"
#include "human/core/log.h"

#include <stdatomic.h>
#include <string.h>

hu_gate_mode_t hu_name_catch_mode(void) {
    /* Default OFF. The rollout (spec §6) sets live at deploy: this gate only
     * writes the graph; what reaches a reply is gated by HU_GRAPH_NAMES. */
    return hu_gate_mode_from_env("HU_NAME_CATCH", HU_GATE_OFF);
}

hu_name_catch_action_t hu_name_catch_action(hu_gate_mode_t mode, hu_name_kind_t kind) {
    if (mode != HU_GATE_LIVE)
        return HU_NAME_CATCH_SKIP;
    return kind == HU_NAME_KNOWN ? HU_NAME_CATCH_BUMP : HU_NAME_CATCH_INSERT;
}

static hu_error_t nc_write(hu_graph_t *g, hu_name_catch_action_t action, const char *cid,
                           size_t cid_len, const hu_name_candidate_t *cand) {
    int64_t id = 0;
    if (action == HU_NAME_CATCH_BUMP) /* KNOWN: the stored spelling, so no new row */
        return hu_graph_upsert_entity(g, cid, cid_len, cand->name, cand->len, HU_ENTITY_UNKNOWN,
                                      NULL, &id);
    return hu_graph_upsert_entity_typed(g, cid, cid_len, cand->name, cand->len,
                                        HU_ENTITY_UNKNOWN, HU_NAME_CATCH_PROVENANCE,
                                        HU_NAME_CATCH_CONFIDENCE, 0u, &id);
}

hu_error_t hu_daemon_name_catch(hu_allocator_t *alloc, hu_graph_t *g, hu_gate_mode_t mode,
                                const char *contact_id, size_t contact_id_len,
                                const char *inbound, size_t inbound_len,
                                hu_name_catch_counts_t *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!alloc || !g || !contact_id || contact_id_len == 0 || !inbound || inbound_len == 0 ||
        !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (mode == HU_GATE_OFF)
        return HU_OK;
    hu_graph_entity_t *ents = NULL;
    size_t n_ents = 0;
    if (hu_graph_list_entities(g, alloc, contact_id, contact_id_len, HU_NAME_CATCH_KNOWN_LIMIT,
                               &ents, &n_ents) != HU_OK)
        n_ents = 0; /* no known names: new Capitalized names still count */
    hu_name_ref_t *refs =
        n_ents ? (hu_name_ref_t *)alloc->alloc(alloc->ctx, n_ents * sizeof(*refs)) : NULL;
    size_t n_refs = 0;
    for (size_t i = 0; refs && i < n_ents; i++) {
        if (hu_name_entity_is_nameable(ents[i].type, ents[i].name, ents[i].name_len)) {
            refs[n_refs].name = ents[i].name;
            refs[n_refs].len = ents[i].name_len;
            n_refs++;
        }
    }
    hu_name_candidate_t cands[HU_NAME_CATCH_MAX];
    size_t n_cands =
        hu_name_extract(inbound, inbound_len, refs, n_refs, cands, HU_NAME_CATCH_MAX);
    hu_error_t first_err = HU_OK;
    for (size_t i = 0; i < n_cands; i++) {
        if (cands[i].kind == HU_NAME_KNOWN)
            out->known++;
        else
            out->fresh++;
        hu_name_catch_action_t action = hu_name_catch_action(mode, cands[i].kind);
        if (action == HU_NAME_CATCH_SKIP)
            continue;
        hu_error_t werr = nc_write(g, action, contact_id, contact_id_len, &cands[i]);
        if (werr == HU_OK)
            out->written++;
        else if (first_err == HU_OK)
            first_err = werr;
    }
    /* KNOWN candidates point into ents[]: free only after every write. */
    if (refs)
        alloc->free(alloc->ctx, refs, n_ents * sizeof(*refs));
    if (ents)
        hu_graph_entities_free(alloc, ents, n_ents);
    return first_err;
}

void hu_daemon_name_catch_tick(hu_allocator_t *alloc, hu_graph_t *g, const char *contact_id,
                               size_t contact_id_len, const char *inbound, size_t inbound_len) {
    static atomic_bool announced = false;
    hu_gate_mode_t mode = hu_name_catch_mode();
    if (mode == HU_GATE_OFF) {
        hu_log_info_once(&announced, "name_catch", NULL,
                         "name_catch disabled (HU_NAME_CATCH unset or off); set "
                         "HU_NAME_CATCH=shadow or =live in the daemon's launchd plist "
                         "EnvironmentVariables to activate");
        return;
    }
    hu_log_info_once(&announced, "name_catch", NULL, "name_catch active: HU_NAME_CATCH=%s",
                     mode == HU_GATE_LIVE ? "live" : "shadow");
    hu_name_catch_counts_t c;
    hu_error_t err = hu_daemon_name_catch(alloc, g, mode, contact_id, contact_id_len, inbound,
                                          inbound_len, &c);
    if (err != HU_OK)
        hu_log_warn("name_catch", NULL, "name_catch: graph write failed: %s (reply unaffected)",
                    hu_error_string(err));
    if (mode == HU_GATE_SHADOW)
        hu_log_info("name_catch", NULL, "name_catch shadow: known=%zu new=%zu (not written)",
                    c.known, c.fresh);
}
```

In `CMakeLists.txt`, add `    src/memory/name_extract.c` after `    src/memory/graph_ingest.c` and `    src/daemon/daemon_name_catch.c` after `    src/daemon/daemon_comfort_summary.c`.

- [ ] **Step 8: Wire the daemon call site** (`src/daemon.c`)

Add `#include "human/daemon/name_catch.h"` after `#include "human/daemon/ml_facade.h"`. Directly after the closing `}` of the `/* Store conversation summary as long-term memory */` block (the `hu_daemon_store_conversation_summary(...)` call, ~:8274-8279), add exactly:

```c
#ifndef HU_IS_TEST
                /* Name catcher (spec 2026-09-29 §4.2): each raw inbound text, never the reply. */
                if (err == HU_OK && response && response_len > 0 && graph && !msgs[batch_start].is_group)
                    for (size_t b = batch_start; b <= batch_end; b++)
                        hu_daemon_name_catch_tick(alloc, graph, batch_key, key_len,
                                                  msgs[b].content, strlen(msgs[b].content));
#endif
```

Leave line wrapping to clang-format (the pre-commit hook runs it). Then confirm the ceiling:

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
wc -l "$W/src/daemon.c"   # must be <= 10420
bash "$W/scripts/check-file-size-ceiling.sh"
```

- [ ] **Step 9: Build and run to verify pass**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cmake --build "$W/build" --target human human_tests -j8 2>&1 | grep -E 'error|warning' | head
cd "$W" && ./build/human_tests --suite=name_extract 2>&1 | tail -3
cd "$W" && ./build/human_tests --suite=daemon_name_catch 2>&1 | tail -3
```
Expected: `Linking C executable human` appears, and both suites pass with 0 failed.

- [ ] **Step 10: Gates, wiring proof, full suite, commit**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
bash "$W/scripts/check-test-source-gate-symmetry.sh"
bash "$W/scripts/check-clone-ratchet.sh" && bash "$W/scripts/check-sqlite-includer-ratchet.sh"
bash "$W/scripts/check-no-new-root-files.sh" && bash "$W/scripts/check-file-size-ceiling.sh"
grep -rn 'hu_daemon_name_catch_tick\|hu_name_extract(' "$W/src" --include='*.c' | grep -v 'daemon_name_catch.c\|name_extract.c'   # must show daemon.c and daemon_name_catch.c callers
HU_DEAD_STRIP_STRICT=1 bash "$W/scripts/check-dead-strip-ratchet.sh"
cd "$W" && ./build/human_tests 2>&1 | grep -E 'Results:'
git -C "$W" add include/human/memory/name_extract.h src/memory/name_extract.c include/human/daemon/name_catch.h src/daemon/daemon_name_catch.c src/daemon.c CMakeLists.txt tests/test_name_extract.c tests/test_daemon_name_catch.c tests/test_main.c
git -C "$W" commit -m "feat(names): zero-model per-turn name catcher behind HU_NAME_CATCH

Nothing live wrote the names contacts mention (llm_decides skips the
deep-extract; the regex fallback wrote 1 relation ever). The catcher reads
each raw inbound text, bumps known names and records new Capitalized names
as UNKNOWN (names:turn, 0.3). Default OFF with a one-shot disabled log;
SHADOW logs counts only.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
Expected: the grep shows `src/daemon.c:…hu_daemon_name_catch_tick(` and `src/daemon/daemon_name_catch.c:…hu_name_extract(`; the dead-strip gate passes (or prints `RATCHET_SKIP` if `build/` is not the dev preset — then rely on CI's `dead-strip-ratchet` job); 0 failed.

---

### Task 4: Extract `hu_graph_ground_compose_turn` (pure refactor, OFF byte-identity first)

**Files:**
- Modify: `include/human/agent/graph_grounding.h`
- Modify: `src/agent/graph_grounding.c` (`gg_log_shadow_and_free` ~:512, the new function after `gg_append_labeled` ~:544, `hu_agent_load_graph_grounding` ~:555-636)
- Test: `tests/test_graph_grounding.c`

**Interfaces:**
- Consumes: `hu_graph_ground_compose`, `hu_graph_ground_compose_ex`, `HU_GG_CONTACT_FALLBACK`, `HU_GG_REQUIRE_FULL_NAME`, `hu_graph_grounding_contact_fallback_mode`, `hu_graph_grounding_self_facts_mode`, `hu_graph_ground_fingerprint`.
- Produces:
  - `#define HU_GG_TURN_FALLBACK_SHADOW 0x01u`, `HU_GG_TURN_FALLBACK_LIVE 0x02u`, `HU_GG_TURN_SELF_SHADOW 0x04u`, `HU_GG_TURN_SELF_LIVE 0x08u`
  - `typedef struct hu_graph_ground_turn_stats { size_t matched_entities; bool via_fallback; bool via_self; bool fallback_shadow; size_t fallback_shadow_bytes; uint32_t fallback_shadow_fp; bool self_shadow; size_t self_shadow_bytes; uint32_t self_shadow_fp; } hu_graph_ground_turn_stats_t;` Task 5 appends fields.
  - `unsigned hu_graph_ground_turn_flags_from_env(void);`
  - `hu_error_t hu_graph_ground_compose_turn(hu_memory_loader_t *loader, const char *contact_id, size_t contact_id_len, const char *msg, size_t msg_len, unsigned turn_flags, char **out, size_t *out_len, hu_graph_ground_turn_stats_t *stats);` It fails open (always `HU_OK`). The caller frees `*out` via `loader->alloc` (`*out_len + 1`). `stats` may be NULL.

- [ ] **Step 1: Pin today's bytes through the EXISTING loader (characterization — must PASS before any change)**

Inside `#ifdef HU_ENABLE_SQLITE` in `tests/test_graph_grounding.c`, after `test_load_grounding_contact_fallback_gate`, add:

```c
/* ── compose_turn: the lexical -> fallback -> self composition shared by the
 * live turn and `human memory ground --full` (spec 2026-09-29 §4.6) ────── */

static void set_turn_env(const char *fallback, const char *self_facts) {
    if (fallback)
        setenv("HU_GRAPH_GROUNDING_CONTACT_FALLBACK", fallback, 1);
    else
        unsetenv("HU_GRAPH_GROUNDING_CONTACT_FALLBACK");
    if (self_facts)
        setenv("HU_GRAPH_GROUNDING_SELF_FACTS", self_facts, 1);
    else
        unsetenv("HU_GRAPH_GROUNDING_SELF_FACTS");
}

static void clear_turn_env(void) {
    unsetenv("HU_GRAPH_GROUNDING_CONTACT_FALLBACK");
    unsetenv("HU_GRAPH_GROUNDING_SELF_FACTS");
    unsetenv("HU_GRAPH_NAMES");
}

/* The live loader's injected block on an ANALYTICAL turn with grounding ON. */
static char *loader_ctx(gg_fixture_t *fx, const char *cid, const char *msg, size_t *len_out) {
    hu_agent_t *agent = (hu_agent_t *)calloc(1, sizeof(hu_agent_t));
    HU_ASSERT_NOT_NULL(agent);
    agent->alloc = &fx->alloc;
    agent->memory_session_id = cid;
    agent->memory_session_id_len = strlen(cid);
    agent->turn_tier = (int)HU_TIER_ANALYTICAL;
    setenv("HU_GRAPH_GROUNDING", "on", 1);
    char *ctx = NULL;
    size_t ctx_len = 0;
    hu_agent_load_graph_grounding(agent, &fx->loader, msg, strlen(msg), &ctx, &ctx_len);
    unsetenv("HU_GRAPH_GROUNDING");
    free(agent);
    *len_out = ctx_len;
    return ctx;
}

/* Today's bytes for a lexical hit plus an owner fact, OFF for every names
 * gate. Any change to what the live turn injects must fail here. */
static const char k_golden_lexical_self[] =
    "- sailboat (topic)\n"
    "  - sailboat related_to marina: docked at slip 14 since spring\n"
    "\n"
    "About you:\n"
    "- tampa bay (place)\n";

static void test_loader_golden_lexical_plus_self_facts(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    int64_t id = 0;
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(fx.graph, "self", 4, "tampa bay", 9, HU_ENTITY_PLACE, NULL, &id),
        HU_OK);
    set_turn_env(NULL, "live");
    size_t len = 0;
    char *ctx = loader_ctx(&fx, "alice", "hows the sailboat down in tampa bay", &len);
    clear_turn_env();
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_STR_EQ(ctx, k_golden_lexical_self);
    HU_ASSERT_EQ((long)len, (long)strlen(k_golden_lexical_self));
    fx.alloc.free(fx.alloc.ctx, ctx, len + 1);
    gg_fixture_close(&fx);
}
```

Register `HU_RUN_TEST(test_loader_golden_lexical_plus_self_facts);` in the SQLite block of `run_graph_grounding_tests`.

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cmake --build "$W/build" --target human_tests -j8 2>&1 | grep -E 'error|warning' | head
cd "$W" && ./build/human_tests --suite=GraphRAG --filter=golden 2>&1 | tail -5
```
Expected: **PASS on unmodified `src/`**. This is a characterization test: the golden records today's behaviour. If it fails, the literal (derived by reading the code) is wrong, not the code. Replace the literal with the exact bytes the FAIL message prints, re-run until it passes, and only then continue. Do not touch `src/` in this step.

- [ ] **Step 2: Write the failing compose_turn tests**

Add after the golden test:

```c
static char *turn_ctx(gg_fixture_t *fx, const char *cid, const char *msg, size_t *len_out,
                      hu_graph_ground_turn_stats_t *st) {
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose_turn(&fx->loader, cid, strlen(cid), msg, strlen(msg),
                                              hu_graph_ground_turn_flags_from_env(), &out, &len,
                                              st),
                 HU_OK);
    *len_out = len;
    return out;
}

static void assert_loader_equals_turn(gg_fixture_t *fx, const char *cid, const char *msg) {
    size_t a_len = 0, b_len = 0;
    hu_graph_ground_turn_stats_t st;
    char *a = loader_ctx(fx, cid, msg, &a_len);
    char *b = turn_ctx(fx, cid, msg, &b_len, &st);
    HU_ASSERT_EQ((long)a_len, (long)b_len);
    if (a_len > 0)
        HU_ASSERT_TRUE(memcmp(a, b, a_len) == 0);
    if (a)
        fx->alloc.free(fx->alloc.ctx, a, a_len + 1);
    if (b)
        fx->alloc.free(fx->alloc.ctx, b, b_len + 1);
}

/* Probe/live parity: what `ground --full` prints is what the turn injects
 * (before the tier gate), for every sub-gate combination. */
static void test_loader_live_output_equals_compose_turn(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    int64_t id = 0;
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(fx.graph, "self", 4, "tampa bay", 9, HU_ENTITY_PLACE, NULL, &id),
        HU_OK);
    const char *hit = "hows the sailboat coming along";
    const char *miss = "wanna grab tacos tonight";
    const char *both = "hows the sailboat down in tampa bay";
    set_turn_env(NULL, NULL);
    assert_loader_equals_turn(&fx, "alice", hit);
    assert_loader_equals_turn(&fx, "alice", miss);
    set_turn_env("live", NULL);
    assert_loader_equals_turn(&fx, "alice", miss);
    assert_loader_equals_turn(&fx, "alice", hit);
    set_turn_env(NULL, "live");
    assert_loader_equals_turn(&fx, "alice", both);
    set_turn_env("shadow", "shadow");
    assert_loader_equals_turn(&fx, "alice", miss);
    assert_loader_equals_turn(&fx, "alice", both);
    set_turn_env("live", "live");
    assert_loader_equals_turn(&fx, "alice", miss);
    assert_loader_equals_turn(&fx, "alice", both);
    clear_turn_env();
    gg_fixture_close(&fx);
}

static void test_compose_turn_golden_and_stats(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    int64_t id = 0;
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(fx.graph, "self", 4, "tampa bay", 9, HU_ENTITY_PLACE, NULL, &id),
        HU_OK);
    set_turn_env(NULL, "live");
    size_t len = 0;
    hu_graph_ground_turn_stats_t st;
    char *out = turn_ctx(&fx, "alice", "hows the sailboat down in tampa bay", &len, &st);
    clear_turn_env();
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, k_golden_lexical_self);
    HU_ASSERT_EQ((long)st.matched_entities, 1L);
    HU_ASSERT_TRUE(st.via_self);
    HU_ASSERT_FALSE(st.via_fallback);
    fx.alloc.free(fx.alloc.ctx, out, len + 1);
    gg_fixture_close(&fx);
}

static void test_compose_turn_reports_shadow_sub_gates_without_injecting(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    set_turn_env("shadow", "shadow");
    size_t len = 99;
    hu_graph_ground_turn_stats_t st;
    char *out = turn_ctx(&fx, "alice", "wanna grab tacos tonight", &len, &st);
    clear_turn_env();
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ((long)len, 0L);
    HU_ASSERT_TRUE(st.fallback_shadow);
    HU_ASSERT_TRUE(st.fallback_shadow_bytes > 0); /* alice has entities to fall back on */
    HU_ASSERT_TRUE(st.fallback_shadow_fp != 0);
    HU_ASSERT_TRUE(st.self_shadow);
    HU_ASSERT_FALSE(st.via_fallback);
    HU_ASSERT_FALSE(st.via_self);
    gg_fixture_close(&fx);
}

static void test_compose_turn_null_loader_is_failopen(void) {
    char *out = (char *)0x1;
    size_t len = 99;
    hu_graph_ground_turn_stats_t st;
    HU_ASSERT_EQ(hu_graph_ground_compose_turn(NULL, "alice", 5, "hi", 2, 0, &out, &len, &st),
                 HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ((long)len, 0L);
    HU_ASSERT_EQ((long)st.matched_entities, 0L);
}
```

Outside the `#ifdef HU_ENABLE_SQLITE` (next to `test_contact_fallback_mode_parse`):

```c
static void test_turn_flags_from_env(void) {
    unsetenv("HU_GRAPH_GROUNDING_CONTACT_FALLBACK");
    unsetenv("HU_GRAPH_GROUNDING_SELF_FACTS");
    unsetenv("HU_GRAPH_NAMES");
    HU_ASSERT_EQ((long)hu_graph_ground_turn_flags_from_env(), 0L);
    setenv("HU_GRAPH_GROUNDING_CONTACT_FALLBACK", "live", 1);
    setenv("HU_GRAPH_GROUNDING_SELF_FACTS", "shadow", 1);
    HU_ASSERT_EQ((long)hu_graph_ground_turn_flags_from_env(),
                 (long)(HU_GG_TURN_FALLBACK_LIVE | HU_GG_TURN_SELF_SHADOW));
    setenv("HU_GRAPH_GROUNDING_CONTACT_FALLBACK", "garbage", 1);
    setenv("HU_GRAPH_GROUNDING_SELF_FACTS", "on", 1);
    HU_ASSERT_EQ((long)hu_graph_ground_turn_flags_from_env(), (long)HU_GG_TURN_SELF_LIVE);
    unsetenv("HU_GRAPH_GROUNDING_CONTACT_FALLBACK");
    unsetenv("HU_GRAPH_GROUNDING_SELF_FACTS");
}
```

Register `HU_RUN_TEST(test_turn_flags_from_env);` next to `test_contact_fallback_mode_parse`. Add to the SQLite block: `test_loader_live_output_equals_compose_turn`, `test_compose_turn_golden_and_stats`, `test_compose_turn_reports_shadow_sub_gates_without_injecting`, `test_compose_turn_null_loader_is_failopen`.

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cmake --build "$W/build" --target human_tests -j8 2>&1 | grep -E 'error' | head
```
Expected: `unknown type name 'hu_graph_ground_turn_stats_t'`.

- [ ] **Step 3: Declare in `include/human/agent/graph_grounding.h`** (before the final `#endif`)

```c
/* ── Turn composition (spec 2026-09-29 §4.6) ───────────────────────────────
 * The lexical -> contact-fallback -> owner-facts composition that
 * hu_agent_load_graph_grounding injects (before its tier gate), with no agent
 * dependency, so `human memory ground --full` measures the real path. Gates
 * arrive as a bitmask (hu_graph_ground_turn_flags_from_env) so tests need no
 * setenv; SHADOW sub-gates are reported in stats and never injected, and the
 * caller logs them. */
#define HU_GG_TURN_FALLBACK_SHADOW 0x01u
#define HU_GG_TURN_FALLBACK_LIVE   0x02u
#define HU_GG_TURN_SELF_SHADOW     0x04u
#define HU_GG_TURN_SELF_LIVE       0x08u

typedef struct hu_graph_ground_turn_stats {
    size_t matched_entities; /* lexical seeds of the contact block */
    bool via_fallback;       /* the contact fallback supplied the block */
    bool via_self;           /* an "About you:" owner block was appended */
    bool fallback_shadow;    /* fallback composed in SHADOW (not injected) */
    size_t fallback_shadow_bytes;
    uint32_t fallback_shadow_fp;
    bool self_shadow; /* owner facts composed in SHADOW (not injected) */
    size_t self_shadow_bytes;
    uint32_t self_shadow_fp;
} hu_graph_ground_turn_stats_t;

/* HU_GRAPH_GROUNDING_CONTACT_FALLBACK and HU_GRAPH_GROUNDING_SELF_FACTS ->
 * HU_GG_TURN_* bits (unset/off/unknown -> no bit). */
unsigned hu_graph_ground_turn_flags_from_env(void);

/* Fail-open (always HU_OK). *out is NULL/0 when nothing composes; the caller
 * frees it via loader->alloc (len + 1). `stats` may be NULL. */
hu_error_t hu_graph_ground_compose_turn(hu_memory_loader_t *loader, const char *contact_id,
                                        size_t contact_id_len, const char *msg, size_t msg_len,
                                        unsigned turn_flags, char **out, size_t *out_len,
                                        hu_graph_ground_turn_stats_t *stats);
```

- [ ] **Step 4: Implement it — legacy loader untouched**

In `src/agent/graph_grounding.c`, directly after `gg_append_labeled` and before the comment above `hu_agent_load_graph_grounding`, add:

```c
unsigned hu_graph_ground_turn_flags_from_env(void) {
    unsigned f = 0;
    switch (hu_graph_grounding_contact_fallback_mode()) {
    case HU_GG_FALLBACK_LIVE:
        f |= HU_GG_TURN_FALLBACK_LIVE;
        break;
    case HU_GG_FALLBACK_SHADOW:
        f |= HU_GG_TURN_FALLBACK_SHADOW;
        break;
    default:
        break;
    }
    hu_gate_mode_t self = hu_graph_grounding_self_facts_mode();
    if (self == HU_GATE_LIVE)
        f |= HU_GG_TURN_SELF_LIVE;
    else if (self == HU_GATE_SHADOW)
        f |= HU_GG_TURN_SELF_SHADOW;
    return f;
}

hu_error_t hu_graph_ground_compose_turn(hu_memory_loader_t *loader, const char *contact_id,
                                        size_t contact_id_len, const char *msg, size_t msg_len,
                                        unsigned turn_flags, char **out, size_t *out_len,
                                        hu_graph_ground_turn_stats_t *stats) {
    hu_graph_ground_turn_stats_t local;
    hu_graph_ground_turn_stats_t *st = stats ? stats : &local;
    memset(st, 0, sizeof(*st));
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    if (!loader || !loader->alloc || !out || !out_len)
        return HU_OK; /* fail-open */
    hu_allocator_t *a = loader->alloc;
    hu_graph_ground_compose(loader, contact_id, contact_id_len, msg, msg_len, 0, out, out_len,
                            &st->matched_entities);
    /* Contact-anchored fallback on a lexical miss. Activation gated on a blind
     * A/B: SHADOW measures and drops, LIVE adopts (the caller's tier gate
     * still applies). Default OFF. */
    if (*out_len == 0 && (turn_flags & (HU_GG_TURN_FALLBACK_SHADOW | HU_GG_TURN_FALLBACK_LIVE))) {
        char *fb = NULL;
        size_t fb_len = 0;
        hu_graph_ground_compose_ex(loader, contact_id, contact_id_len, msg, msg_len, 0,
                                   HU_GG_CONTACT_FALLBACK, &fb, &fb_len, NULL);
        if (turn_flags & HU_GG_TURN_FALLBACK_LIVE) {
            if (fb) {
                *out = fb;
                *out_len = fb_len;
                st->via_fallback = true;
            }
        } else {
            st->fallback_shadow = true;
            st->fallback_shadow_bytes = fb_len;
            st->fallback_shadow_fp = hu_graph_ground_fingerprint(fb, fb_len);
            if (fb)
                a->free(a->ctx, fb, fb_len + 1);
        }
    }
    /* Owner ("self") facts the message names in full. Activation gated on a
     * blind A/B, default OFF: SHADOW measures and drops; LIVE appends them
     * under "About you:" so the model never mistakes them for the contact's. */
    if (turn_flags & (HU_GG_TURN_SELF_SHADOW | HU_GG_TURN_SELF_LIVE)) {
        char *sf = NULL;
        size_t sf_len = 0;
        hu_graph_ground_compose_ex(loader, "self", 4, msg, msg_len, 0, HU_GG_REQUIRE_FULL_NAME,
                                   &sf, &sf_len, NULL);
        if (turn_flags & HU_GG_TURN_SELF_LIVE) {
            if (sf)
                st->via_self = gg_append_labeled(loader, out, out_len, "About you:\n", sf, sf_len);
        } else {
            st->self_shadow = true;
            st->self_shadow_bytes = sf_len;
            st->self_shadow_fp = hu_graph_ground_fingerprint(sf, sf_len);
            if (sf)
                a->free(a->ctx, sf, sf_len + 1);
        }
    }
    return HU_OK;
}
```

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cmake --build "$W/build" --target human human_tests -j8 2>&1 | grep -E 'error|warning' | head
cd "$W" && ./build/human_tests --suite=GraphRAG 2>&1 | tail -3
```
Expected: 0 failed. The equality test now compares the new function against the **untouched legacy loader**. That is the byte-identity proof. Do not commit yet: the duplicated logic would fail the clone ratchet.

- [ ] **Step 5: Rewire the loader onto compose_turn**

Replace `gg_log_shadow_and_free` with:

```c
/* SHADOW contract for grounding sub-gates: size + fingerprint only (never text). */
static void gg_log_shadow(const char *what, size_t len, uint32_t fp, int tier) {
    hu_log_info("graph_grounding", NULL, "%s shadow: %zu bytes tier=%d fp=%08x (not injected)",
                what, len, tier, (unsigned)fp);
}
```

In `hu_agent_load_graph_grounding`, replace everything from `size_t matched_entities = 0;` down to (not including) `const char *drop_reason = NULL;` with:

```c
    /* Lexical -> contact fallback -> owner facts, each behind its own gate
     * (activation gated on a blind A/B, default OFF). The SAME composition
     * backs `human memory ground --full`, so the probe measures exactly what
     * this turn would inject before the tier gate below. */
    hu_graph_ground_turn_stats_t st;
    hu_graph_ground_compose_turn(loader, agent->memory_session_id, agent->memory_session_id_len,
                                 msg, msg_len, hu_graph_ground_turn_flags_from_env(), graph_ctx,
                                 graph_ctx_len, &st);
    if (st.fallback_shadow)
        gg_log_shadow("contact_fallback", st.fallback_shadow_bytes, st.fallback_shadow_fp,
                      agent->turn_tier);
    if (st.self_shadow)
        gg_log_shadow("self_facts", st.self_shadow_bytes, st.self_shadow_fp, agent->turn_tier);
    size_t matched_entities = st.matched_entities;
    bool via_fallback = st.via_fallback, via_self = st.via_self;
```

Everything from `const char *drop_reason = NULL;` to the end of the function is unchanged. The log lines keep their exact format and order: fallback, then self, then the main shadow/live line.

- [ ] **Step 6: Run to verify pass**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cmake --build "$W/build" --target human human_tests -j8 2>&1 | grep -E 'error|warning' | head
cd "$W" && ./build/human_tests --suite=GraphRAG 2>&1 | tail -3
```
Expected: 0 failed, including the unchanged `test_load_grounding_*` tests and both goldens.

- [ ] **Step 7: Gates, full suite, commit**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
bash "$W/scripts/check-clone-ratchet.sh" && bash "$W/scripts/check-agent-core-boundary.sh"
cd "$W" && ./build/human_tests 2>&1 | grep -E 'Results:'
git -C "$W" add include/human/agent/graph_grounding.h src/agent/graph_grounding.c tests/test_graph_grounding.c
git -C "$W" commit -m "refactor(grounding): extract hu_graph_ground_compose_turn

The lexical -> fallback -> self composition lived inside the agent loader,
so 'human memory ground' measured a different path than the live turn.
Pure extraction: a golden pinned through the untouched loader first, then
loader == compose_turn for every sub-gate combination. Logs unchanged.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: `HU_GRAPH_NAMES` reader gate — names-first seeding, typed bonus, topic line, SHADOW

**Files:**
- Modify: `include/human/memory/graph.h`, `src/memory/graph.c` (`hu_graph_list_recent_entities_of_type` next to `hu_graph_list_entities` ~:1732, stub in the `#else` ~:2004)
- Modify: `include/human/agent/graph_grounding.h`, `src/agent/graph_grounding.c`
- Test: `tests/test_graph.c`, `tests/test_graph_grounding.c`

**Interfaces:**
- Consumes: `hu_name_entity_is_nameable` (Task 3); `hu_graph_ground_compose_turn`, `hu_graph_ground_turn_stats_t`, `HU_GG_TURN_*` (Task 4).
- Produces:
  - `hu_error_t hu_graph_list_recent_entities_of_type(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id, size_t contact_id_len, hu_entity_type_t type, size_t limit, hu_graph_entity_t **out, size_t *out_count);` It orders by `last_seen DESC, id DESC` and returns `HU_ERR_INVALID_ARGUMENT` for `limit == 0`.
  - `#define HU_GG_NAMES 0x4u`, `#define HU_GG_TYPED_NAME_BONUS 0.5`, `#define HU_GG_TOPIC_LINE_MAX 3`
  - `#define HU_GG_TURN_NAMES_SHADOW 0x10u`, `#define HU_GG_TURN_NAMES_LIVE 0x20u`
  - New stats fields (appended): `bool names_shadow; size_t names_off_bytes; size_t names_live_bytes; size_t names_live_typed; size_t typed_names;`
  - `hu_gate_mode_t hu_graph_names_mode(void);` (`HU_GRAPH_NAMES`, default OFF)
  - `size_t hu_graph_ground_count_typed_names(const char *block, size_t len);` It counts lines `- <name> (person|place|organization|event)`.

- [ ] **Step 1: Write the failing tests**

In `tests/test_graph.c` (SQLite block, after the Task 1 tests):

```c
static void graph_list_recent_entities_of_type_orders_by_last_seen(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(&alloc, "x", 1, &g), HU_OK);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "c1", 2, "a", 1, HU_ENTITY_TOPIC, NULL, &id), HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "c1", 2, "b", 1, HU_ENTITY_TOPIC, NULL, &id), HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "c1", 2, "c", 1, HU_ENTITY_TOPIC, NULL, &id), HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "c1", 2, "p", 1, HU_ENTITY_PERSON, NULL, &id), HU_OK);
    HU_ASSERT_EQ(sqlite3_exec(hu_graph_sqlite_connection(g),
                              "UPDATE entities SET last_seen = CASE name WHEN 'a' THEN 3000"
                              " WHEN 'b' THEN 1000 WHEN 'c' THEN 2000 ELSE 9000 END",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    hu_graph_entity_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_graph_list_recent_entities_of_type(g, &alloc, "c1", 2, HU_ENTITY_TOPIC, 2,
                                                       &out, &n),
                 HU_OK);
    HU_ASSERT_EQ((long)n, 2L);
    HU_ASSERT_STR_EQ(out[0].name, "a");
    HU_ASSERT_STR_EQ(out[1].name, "c");
    hu_graph_entities_free(&alloc, out, n);
    HU_ASSERT_EQ(hu_graph_list_recent_entities_of_type(g, &alloc, "c1", 2, HU_ENTITY_TOPIC, 0,
                                                       &out, &n),
                 HU_ERR_INVALID_ARGUMENT);
    hu_graph_close(g, &alloc);
}
```
Register `HU_RUN_TEST(graph_list_recent_entities_of_type_orders_by_last_seen);` in the SQLite `run_graph_tests`.

In `tests/test_graph_grounding.c`, outside the SQLite block:

```c
static void test_count_typed_names(void) {
    const char *b = "- Salim (person)\n"
                    "  - Salim knows Priya (person)\n" /* a relation line, not an entity line */
                    "- Zed Corp\n"
                    "- sailboat (topic)\n"
                    "- Tampa (place)\n"
                    "- Acme (organization)\n"
                    "- Coachella (event)\n"
                    "Been talking about: a, b\n"
                    "- (person)\n"; /* no name */
    HU_ASSERT_EQ((long)hu_graph_ground_count_typed_names(b, strlen(b)), 4L);
    HU_ASSERT_EQ((long)hu_graph_ground_count_typed_names(NULL, 0), 0L);
    const char *tail = "- Salim (person)"; /* no trailing newline */
    HU_ASSERT_EQ((long)hu_graph_ground_count_typed_names(tail, strlen(tail)), 1L);
}

static void test_graph_names_mode_defaults_off(void) {
    unsetenv("HU_GRAPH_NAMES");
    HU_ASSERT_EQ((int)hu_graph_names_mode(), (int)HU_GATE_OFF);
    setenv("HU_GRAPH_NAMES", "shadow", 1);
    HU_ASSERT_EQ((int)hu_graph_names_mode(), (int)HU_GATE_SHADOW);
    HU_ASSERT_EQ((long)(hu_graph_ground_turn_flags_from_env() & HU_GG_TURN_NAMES_SHADOW),
                 (long)HU_GG_TURN_NAMES_SHADOW);
    setenv("HU_GRAPH_NAMES", "live", 1);
    HU_ASSERT_EQ((int)hu_graph_names_mode(), (int)HU_GATE_LIVE);
    HU_ASSERT_EQ((long)(hu_graph_ground_turn_flags_from_env() & HU_GG_TURN_NAMES_LIVE),
                 (long)HU_GG_TURN_NAMES_LIVE);
    setenv("HU_GRAPH_NAMES", "garbage", 1);
    HU_ASSERT_EQ((int)hu_graph_names_mode(), (int)HU_GATE_OFF);
    unsetenv("HU_GRAPH_NAMES");
}
```

Inside the SQLite block, after the Task 4 tests:

```c
/* carol mixes typed names, a Capitalized UNKNOWN, a lowercase UNKNOWN phrase,
 * two TOPICs and an EMOTION (the prod graph's shape, spec §1). */
static void seed_carol_names(gg_fixture_t *fx) {
    int64_t id = 0;
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(fx->graph, "carol", 5, "Salim", 5, HU_ENTITY_PERSON, NULL, &id),
        HU_OK);
    for (int i = 0; i < 5; i++)
        HU_ASSERT_EQ(hu_graph_upsert_entity(fx->graph, "carol", 5, "Zed Corp", 8,
                                            HU_ENTITY_UNKNOWN, NULL, &id),
                     HU_OK);
    for (int i = 0; i < 3; i++)
        HU_ASSERT_EQ(hu_graph_upsert_entity(fx->graph, "carol", 5, "different direction", 19,
                                            HU_ENTITY_UNKNOWN, NULL, &id),
                     HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(fx->graph, "carol", 5, "lake house", 10, HU_ENTITY_TOPIC,
                                        NULL, &id),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_upsert_entity(fx->graph, "carol", 5, "pickleball", 10, HU_ENTITY_TOPIC,
                                        NULL, &id),
                 HU_OK);
    for (int i = 0; i < 2; i++)
        HU_ASSERT_EQ(hu_graph_upsert_entity(fx->graph, "carol", 5, "heartbreak", 10,
                                            HU_ENTITY_EMOTION, NULL, &id),
                     HU_OK);
}

static char *compose_carol(gg_fixture_t *fx, const char *msg, unsigned flags, size_t *len) {
    char *out = NULL;
    *len = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose_ex(&fx->loader, "carol", 5, msg, strlen(msg), 0, flags,
                                            &out, len, NULL),
                 HU_OK);
    return out;
}

static void test_names_live_lexical_drops_topics_and_renders_topic_line(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    seed_carol_names(&fx);
    const char *msg = "did salim like the lake house";
    size_t off_len = 0, live_len = 0;
    char *off = compose_carol(&fx, msg, 0, &off_len);
    char *live = compose_carol(&fx, msg, HU_GG_NAMES, &live_len);
    HU_ASSERT_NOT_NULL(off);
    HU_ASSERT_STR_CONTAINS(off, "- lake house (topic)\n"); /* today: a topic seeds */
    HU_ASSERT_NOT_NULL(live);
    HU_ASSERT_STR_CONTAINS(live, "- Salim (person)\n");
    HU_ASSERT_STR_NOT_CONTAINS(live, "- lake house");
    const char *topics = strstr(live, "Been talking about: ");
    HU_ASSERT_NOT_NULL(topics);
    HU_ASSERT_TRUE(topics > strstr(live, "- Salim (person)\n")); /* after the entity lines */
    HU_ASSERT_STR_CONTAINS(topics, "lake house");
    HU_ASSERT_STR_CONTAINS(topics, "pickleball");
    fx.alloc.free(fx.alloc.ctx, off, off_len + 1);
    fx.alloc.free(fx.alloc.ctx, live, live_len + 1);
    gg_fixture_close(&fx);
}

static void test_names_live_fallback_prefers_typed_names(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    seed_carol_names(&fx);
    const char *msg = "wanna grab tacos tonight";
    size_t off_len = 0, live_len = 0;
    char *off = compose_carol(&fx, msg, HU_GG_CONTACT_FALLBACK, &off_len);
    char *live = compose_carol(&fx, msg, HU_GG_CONTACT_FALLBACK | HU_GG_NAMES, &live_len);
    HU_ASSERT_NOT_NULL(off);
    HU_ASSERT_STR_CONTAINS(off, "- Zed Corp\n");            /* most mentioned */
    HU_ASSERT_STR_CONTAINS(off, "- different direction\n"); /* today: phrases seed */
    HU_ASSERT_NOT_NULL(live);
    HU_ASSERT_TRUE(strncmp(live, "- Salim (person)\n", 17) == 0); /* typed bonus: first */
    HU_ASSERT_STR_CONTAINS(live, "- Zed Corp\n");                  /* Capitalized UNKNOWN */
    HU_ASSERT_STR_NOT_CONTAINS(live, "different direction");
    HU_ASSERT_STR_NOT_CONTAINS(live, "heartbreak");
    HU_ASSERT_STR_NOT_CONTAINS(live, "- pickleball");
    fx.alloc.free(fx.alloc.ctx, off, off_len + 1);
    fx.alloc.free(fx.alloc.ctx, live, live_len + 1);
    gg_fixture_close(&fx);
}

/* Review Focus 3: a legacy lowercase UNKNOWN "salim" beside the typed "Salim"
 * renders once under LIVE. */
static void test_names_live_renders_one_line_for_a_lowercase_duplicate(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    int64_t id = 0;
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(fx.graph, "dana", 4, "salim", 5, HU_ENTITY_UNKNOWN, NULL, &id),
        HU_OK);
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(fx.graph, "dana", 4, "Salim", 5, HU_ENTITY_PERSON, NULL, &id),
        HU_OK);
    const char *msg = "did salim call";
    char *off = NULL, *live = NULL;
    size_t off_len = 0, live_len = 0;
    HU_ASSERT_EQ(hu_graph_ground_compose_ex(&fx.loader, "dana", 4, msg, strlen(msg), 0, 0, &off,
                                            &off_len, NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_ground_compose_ex(&fx.loader, "dana", 4, msg, strlen(msg), 0,
                                            HU_GG_NAMES, &live, &live_len, NULL),
                 HU_OK);
    HU_ASSERT_STR_CONTAINS(off, "- salim\n"); /* the fixture really has the duplicate */
    HU_ASSERT_STR_CONTAINS(live, "- Salim (person)\n");
    HU_ASSERT_STR_NOT_CONTAINS(live, "- salim\n");
    fx.alloc.free(fx.alloc.ctx, off, off_len + 1);
    fx.alloc.free(fx.alloc.ctx, live, live_len + 1);
    gg_fixture_close(&fx);
}

static void test_names_shadow_injects_the_off_block_and_measures_live(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    seed_carol_names(&fx);
    const char *msg = "did salim like the lake house";
    char *off = NULL, *shadow = NULL, *live = NULL;
    size_t off_len = 0, shadow_len = 0, live_len = 0;
    hu_graph_ground_turn_stats_t st_off, st_shadow, st_live;
    HU_ASSERT_EQ(hu_graph_ground_compose_turn(&fx.loader, "carol", 5, msg, strlen(msg), 0, &off,
                                              &off_len, &st_off),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_ground_compose_turn(&fx.loader, "carol", 5, msg, strlen(msg),
                                              HU_GG_TURN_NAMES_SHADOW, &shadow, &shadow_len,
                                              &st_shadow),
                 HU_OK);
    HU_ASSERT_EQ(hu_graph_ground_compose_turn(&fx.loader, "carol", 5, msg, strlen(msg),
                                              HU_GG_TURN_NAMES_LIVE, &live, &live_len, &st_live),
                 HU_OK);
    HU_ASSERT_EQ((long)shadow_len, (long)off_len);
    HU_ASSERT_TRUE(memcmp(shadow, off, off_len) == 0);
    HU_ASSERT_TRUE(st_shadow.names_shadow);
    HU_ASSERT_EQ((long)st_shadow.names_off_bytes, (long)off_len);
    HU_ASSERT_EQ((long)st_shadow.names_live_bytes, (long)live_len);
    HU_ASSERT_EQ((long)st_shadow.names_live_typed, 1L);
    HU_ASSERT_FALSE(st_off.names_shadow);
    HU_ASSERT_EQ((long)st_live.typed_names, 1L);
    fx.alloc.free(fx.alloc.ctx, off, off_len + 1);
    fx.alloc.free(fx.alloc.ctx, shadow, shadow_len + 1);
    fx.alloc.free(fx.alloc.ctx, live, live_len + 1);
    gg_fixture_close(&fx);
}

/* OFF byte-identity (spec §2) through the real loader: an explicit
 * HU_GRAPH_NAMES=off still yields Task 4's golden, and SHADOW injects the same. */
static void test_names_off_and_shadow_leave_the_golden_unchanged(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    int64_t id = 0;
    HU_ASSERT_EQ(
        hu_graph_upsert_entity(fx.graph, "self", 4, "tampa bay", 9, HU_ENTITY_PLACE, NULL, &id),
        HU_OK);
    const char *msg = "hows the sailboat down in tampa bay";
    const char *modes[] = {"off", "shadow"};
    for (size_t m = 0; m < 2; m++) {
        set_turn_env(NULL, "live");
        setenv("HU_GRAPH_NAMES", modes[m], 1);
        size_t len = 0;
        char *ctx = loader_ctx(&fx, "alice", msg, &len);
        clear_turn_env();
        HU_ASSERT_NOT_NULL(ctx);
        HU_ASSERT_STR_EQ(ctx, k_golden_lexical_self);
        fx.alloc.free(fx.alloc.ctx, ctx, len + 1);
    }
    gg_fixture_close(&fx);
}

static void test_names_live_reaches_the_loader(void) {
    gg_fixture_t fx;
    gg_fixture_open(&fx);
    seed_carol_names(&fx);
    set_turn_env("live", NULL);
    setenv("HU_GRAPH_NAMES", "live", 1);
    size_t len = 0;
    char *ctx = loader_ctx(&fx, "carol", "wanna grab tacos tonight", &len);
    clear_turn_env();
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(strncmp(ctx, "- Salim (person)\n", 17) == 0);
    fx.alloc.free(fx.alloc.ctx, ctx, len + 1);
    gg_fixture_close(&fx);
}
```

Register: `test_count_typed_names` and `test_graph_names_mode_defaults_off` next to `test_turn_flags_from_env`. In the SQLite block, add `test_names_live_lexical_drops_topics_and_renders_topic_line`, `test_names_live_fallback_prefers_typed_names`, `test_names_live_renders_one_line_for_a_lowercase_duplicate`, `test_names_shadow_injects_the_off_block_and_measures_live`, `test_names_off_and_shadow_leave_the_golden_unchanged`, `test_names_live_reaches_the_loader`.

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cmake --build "$W/build" --target human_tests -j8 2>&1 | grep -E 'error' | head
```
Expected: undeclared `HU_GG_NAMES`, `hu_graph_names_mode`, `hu_graph_list_recent_entities_of_type`, …

- [ ] **Step 2: Graph query**

`include/human/memory/graph.h`, after `hu_graph_list_entities`:

```c
/* The contact's `limit` most recent entities of `type` (last_seen DESC, id
 * DESC). Used for grounding's "Been talking about:" topic line.
 * HU_ERR_INVALID_ARGUMENT on limit == 0. Free with hu_graph_entities_free. */
hu_error_t hu_graph_list_recent_entities_of_type(hu_graph_t *g, hu_allocator_t *alloc,
                                                 const char *contact_id, size_t contact_id_len,
                                                 hu_entity_type_t type, size_t limit,
                                                 hu_graph_entity_t **out, size_t *out_count);
```

`src/memory/graph.c`, directly after `hu_graph_list_entities` (inside its `#ifdef HU_ENABLE_SQLITE`):

```c
hu_error_t hu_graph_list_recent_entities_of_type(hu_graph_t *g, hu_allocator_t *alloc,
                                                 const char *contact_id, size_t contact_id_len,
                                                 hu_entity_type_t type, size_t limit,
                                                 hu_graph_entity_t **out, size_t *out_count) {
    if (!g || !g->db || !alloc || !out || !out_count || limit == 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(g->db,
                           "SELECT id, name, type, first_seen, last_seen, mention_count FROM"
                           " entities WHERE contact_id = ?1 AND type = ?2"
                           " ORDER BY last_seen DESC, id DESC LIMIT ?3",
                           -1, &q, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(q, 1, contact_id ? contact_id : "", contact_id ? (int)contact_id_len : 0,
                      SQLITE_STATIC);
    sqlite3_bind_int(q, 2, (int)type);
    sqlite3_bind_int64(q, 3, (int64_t)limit);
    return collect_entities(q, alloc, limit, out, out_count); /* finalizes q */
}
```

and in the matching `#else` block (after the `hu_graph_list_entities` stub):

```c
hu_error_t hu_graph_list_recent_entities_of_type(hu_graph_t *g, hu_allocator_t *alloc,
                                                 const char *contact_id, size_t contact_id_len,
                                                 hu_entity_type_t type, size_t limit,
                                                 hu_graph_entity_t **out, size_t *out_count) {
    (void)type;
    (void)out_count;
    (void)out;
    (void)limit;
    (void)contact_id_len;
    (void)contact_id;
    (void)alloc;
    (void)g;
    return HU_ERR_NOT_SUPPORTED;
}
```

- [ ] **Step 3: Declare the gate in `include/human/agent/graph_grounding.h`**

After `#define HU_GG_REQUIRE_FULL_NAME 0x2u` add:

```c
/* Names-first seeding (HU_GRAPH_NAMES LIVE, spec 2026-09-29 §4.6): seed only
 * entities hu_name_entity_is_nameable accepts (PERSON/PLACE/ORGANIZATION/
 * EVENT, or a Capitalized UNKNOWN), add HU_GG_TYPED_NAME_BONUS to typed
 * names, and after the entity lines render the contact's
 * HU_GG_TOPIC_LINE_MAX most recent TOPICs on one "Been talking about: a, b, c"
 * line (only on a non-empty block). EMOTION never seeds. */
#define HU_GG_NAMES            0x4u
#define HU_GG_TYPED_NAME_BONUS 0.5
#define HU_GG_TOPIC_LINE_MAX   3
```

After `#define HU_GG_TURN_SELF_LIVE 0x08u` add:

```c
#define HU_GG_TURN_NAMES_SHADOW 0x10u
#define HU_GG_TURN_NAMES_LIVE   0x20u
```

Append to the end of `hu_graph_ground_turn_stats_t`:

```c
    bool names_shadow;       /* HU_GRAPH_NAMES=shadow: LIVE composed, OFF injected */
    size_t names_off_bytes;  /* the injected (OFF) block */
    size_t names_live_bytes; /* what LIVE would have injected */
    size_t names_live_typed; /* typed-name lines LIVE would have injected */
    size_t typed_names;      /* typed-name lines in the returned block */
```

After `hu_graph_ground_turn_flags_from_env`, update its comment to mention `HU_GRAPH_NAMES`, and add:

```c
/* Reads HU_GRAPH_NAMES per hu_gate_mode_parse; unset -> OFF. */
hu_gate_mode_t hu_graph_names_mode(void);

/* Entity lines naming a typed entity: "- <name> (person|place|organization|
 * event)" at line start (relation lines are indented). Pure. */
size_t hu_graph_ground_count_typed_names(const char *block, size_t len);
```

- [ ] **Step 4: Implement in `src/agent/graph_grounding.c`**

Add `#include "human/memory/name_extract.h"` after `#include "human/memory/graph_state.h"`.

After `hu_graph_grounding_self_facts_mode`:

```c
hu_gate_mode_t hu_graph_names_mode(void) {
    /* HU_GRAPH_NAMES activation gated on scripts/eval_name_grounding.py (live
     * >= 15/40 real moments with a typed name, OFF unchanged) AND Seth's
     * go-ahead (blind A/B or an explicit override): do not flip the default
     * without that measurement. SHADOW composes both and injects OFF. */
    return hu_gate_mode_from_env("HU_GRAPH_NAMES", HU_GATE_OFF);
}
```

After `hu_graph_ground_is_placeholder_name` (outside the SQLite block):

```c
size_t hu_graph_ground_count_typed_names(const char *block, size_t len) {
    static const char *const k_typed[] = {" (person)", " (place)", " (organization)",
                                          " (event)"};
    size_t count = 0, start = 0;
    while (block && start < len) {
        const char *nl = memchr(block + start, '\n', len - start);
        size_t end = nl ? (size_t)(nl - block) : len;
        size_t line_len = end - start;
        if (line_len > 2 && block[start] == '-' && block[start + 1] == ' ') {
            for (size_t k = 0; k < sizeof(k_typed) / sizeof(k_typed[0]); k++) {
                size_t sl = strlen(k_typed[k]);
                if (line_len > 2 + sl && memcmp(block + end - sl, k_typed[k], sl) == 0) {
                    count++;
                    break;
                }
            }
        }
        start = end + 1;
    }
    return count;
}
```

Inside the SQLite static section, after `gg_pick_top_k` and before `#endif /* HU_ENABLE_SQLITE */`:

```c
/* HU_GG_NAMES scoring: a non-nameable candidate never seeds; typed names
 * outrank untyped ones at equal coverage. */
static double gg_names_adjust(const hu_graph_entity_t *e, double score) {
    if (!hu_name_entity_is_nameable(e->type, e->name, e->name_len))
        return 0.0;
    return e->type == HU_ENTITY_UNKNOWN ? score : score + HU_GG_TYPED_NAME_BONUS;
}

/* "Been talking about: a, b, c\n" from the contact's most recent TOPICs; the
 * whole line or nothing within the cap. */
static void gg_append_topic_line(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                                 size_t contact_id_len, char *buf, size_t max_chars,
                                 size_t *pos) {
    hu_graph_entity_t *topics = NULL;
    size_t tn = 0;
    if (hu_graph_list_recent_entities_of_type(g, alloc, contact_id, contact_id_len,
                                              HU_ENTITY_TOPIC, HU_GG_TOPIC_LINE_MAX, &topics,
                                              &tn) != HU_OK)
        tn = 0;
    static const char k_label[] = "Been talking about: ";
    size_t start = *pos, written = 0;
    bool ok = tn > 0 && gg_append(buf, max_chars, pos, k_label, sizeof(k_label) - 1);
    for (size_t i = 0; ok && i < tn; i++) {
        if (!topics[i].name || topics[i].name_len == 0)
            continue;
        if (written > 0)
            ok = gg_append(buf, max_chars, pos, ", ", 2);
        ok = ok && gg_append(buf, max_chars, pos, topics[i].name, topics[i].name_len);
        written++;
    }
    ok = ok && written > 0 && gg_append(buf, max_chars, pos, "\n", 1);
    if (!ok)
        *pos = start;
    if (topics)
        hu_graph_entities_free(alloc, topics, tn);
}
```

In `hu_graph_ground_compose_ex`, lexical scoring loop, after the `scores[i] = partial ? … ;` statement (inside the loop), add:

```c
        if ((flags & HU_GG_NAMES) && scores[i] > 0.0)
            scores[i] = gg_names_adjust(e, scores[i]);
```

In the fallback loop, after the `scores[i] = eligible ? … : 0.0;` statement (inside the loop), add the same two lines:

```c
            if ((flags & HU_GG_NAMES) && scores[i] > 0.0)
                scores[i] = gg_names_adjust(e, scores[i]);
```

Replace:

```c
    hu_graph_entities_free(alloc, cands, cand_count);

    if (pos == 0) {
```

with:

```c
    if ((flags & HU_GG_NAMES) && pos > 0)
        gg_append_topic_line(g, alloc, contact_id, contact_id_len, buf, max_chars, &pos);
    hu_graph_entities_free(alloc, cands, cand_count);

    if (pos == 0) {
```

In `hu_graph_ground_turn_flags_from_env`, before `return f;`:

```c
    hu_gate_mode_t names = hu_graph_names_mode();
    if (names == HU_GATE_LIVE)
        f |= HU_GG_TURN_NAMES_LIVE;
    else if (names == HU_GATE_SHADOW)
        f |= HU_GG_TURN_NAMES_SHADOW;
```

Replace the whole `hu_graph_ground_compose_turn` from Task 4 with a pass helper plus the new entry point:

```c
/* One composition pass. `seed_flags` (0 or HU_GG_NAMES) applies to the two
 * contact composes, never to the owner facts, which keep their full-name rule. */
static void gg_compose_turn_pass(hu_memory_loader_t *loader, const char *contact_id,
                                 size_t contact_id_len, const char *msg, size_t msg_len,
                                 unsigned turn_flags, unsigned seed_flags, char **out,
                                 size_t *out_len, hu_graph_ground_turn_stats_t *st) {
    hu_allocator_t *a = loader->alloc;
    hu_graph_ground_compose_ex(loader, contact_id, contact_id_len, msg, msg_len, 0, seed_flags,
                               out, out_len, &st->matched_entities);
    /* Contact-anchored fallback on a lexical miss. Activation gated on a blind
     * A/B: SHADOW measures and drops, LIVE adopts (the caller's tier gate
     * still applies). Default OFF. */
    if (*out_len == 0 && (turn_flags & (HU_GG_TURN_FALLBACK_SHADOW | HU_GG_TURN_FALLBACK_LIVE))) {
        char *fb = NULL;
        size_t fb_len = 0;
        hu_graph_ground_compose_ex(loader, contact_id, contact_id_len, msg, msg_len, 0,
                                   HU_GG_CONTACT_FALLBACK | seed_flags, &fb, &fb_len, NULL);
        if (turn_flags & HU_GG_TURN_FALLBACK_LIVE) {
            if (fb) {
                *out = fb;
                *out_len = fb_len;
                st->via_fallback = true;
            }
        } else {
            st->fallback_shadow = true;
            st->fallback_shadow_bytes = fb_len;
            st->fallback_shadow_fp = hu_graph_ground_fingerprint(fb, fb_len);
            if (fb)
                a->free(a->ctx, fb, fb_len + 1);
        }
    }
    /* Owner ("self") facts the message names in full. Activation gated on a
     * blind A/B, default OFF: SHADOW measures and drops; LIVE appends them
     * under "About you:" so the model never mistakes them for the contact's. */
    if (turn_flags & (HU_GG_TURN_SELF_SHADOW | HU_GG_TURN_SELF_LIVE)) {
        char *sf = NULL;
        size_t sf_len = 0;
        hu_graph_ground_compose_ex(loader, "self", 4, msg, msg_len, 0, HU_GG_REQUIRE_FULL_NAME,
                                   &sf, &sf_len, NULL);
        if (turn_flags & HU_GG_TURN_SELF_LIVE) {
            if (sf)
                st->via_self = gg_append_labeled(loader, out, out_len, "About you:\n", sf, sf_len);
        } else {
            st->self_shadow = true;
            st->self_shadow_bytes = sf_len;
            st->self_shadow_fp = hu_graph_ground_fingerprint(sf, sf_len);
            if (sf)
                a->free(a->ctx, sf, sf_len + 1);
        }
    }
}

hu_error_t hu_graph_ground_compose_turn(hu_memory_loader_t *loader, const char *contact_id,
                                        size_t contact_id_len, const char *msg, size_t msg_len,
                                        unsigned turn_flags, char **out, size_t *out_len,
                                        hu_graph_ground_turn_stats_t *stats) {
    hu_graph_ground_turn_stats_t local;
    hu_graph_ground_turn_stats_t *st = stats ? stats : &local;
    memset(st, 0, sizeof(*st));
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    if (!loader || !loader->alloc || !out || !out_len)
        return HU_OK; /* fail-open */
    /* HU_GRAPH_NAMES activation gated on scripts/eval_name_grounding.py plus
     * Seth's go-ahead (see hu_graph_names_mode). */
    bool names_live = (turn_flags & HU_GG_TURN_NAMES_LIVE) != 0;
    gg_compose_turn_pass(loader, contact_id, contact_id_len, msg, msg_len, turn_flags,
                         names_live ? HU_GG_NAMES : 0u, out, out_len, st);
    if (!names_live && (turn_flags & HU_GG_TURN_NAMES_SHADOW)) {
        hu_graph_ground_turn_stats_t live_st;
        memset(&live_st, 0, sizeof(live_st));
        char *live = NULL;
        size_t live_len = 0;
        gg_compose_turn_pass(loader, contact_id, contact_id_len, msg, msg_len, turn_flags,
                             HU_GG_NAMES, &live, &live_len, &live_st);
        st->names_shadow = true;
        st->names_off_bytes = *out_len;
        st->names_live_bytes = live_len;
        st->names_live_typed = hu_graph_ground_count_typed_names(live, live_len);
        if (live)
            loader->alloc->free(loader->alloc->ctx, live, live_len + 1);
    }
    st->typed_names = hu_graph_ground_count_typed_names(*out, *out_len);
    return HU_OK;
}
```

In `hu_agent_load_graph_grounding`, after the `if (st.self_shadow) gg_log_shadow(...)` lines, add:

```c
    if (st.names_shadow)
        hu_log_info("graph_grounding", NULL,
                    "names shadow: off=%zu live=%zu bytes names=%zu (not injected)",
                    st.names_off_bytes, st.names_live_bytes, st.names_live_typed);
```

- [ ] **Step 5: Run to verify pass**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cmake --build "$W/build" --target human human_tests -j8 2>&1 | grep -E 'error|warning' | head
cd "$W" && ./build/human_tests --suite=graph 2>&1 | tail -3
```
Expected: 0 failed across `graph`, `graph_ingest` and `GraphRAG grounding`, including both goldens (OFF byte-identity).

- [ ] **Step 6: Gates, full suite, commit**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
bash "$W/scripts/check-clone-ratchet.sh" && bash "$W/scripts/check-agent-core-boundary.sh"
bash "$W/scripts/check-sqlite-includer-ratchet.sh"
HU_DEAD_STRIP_STRICT=1 bash "$W/scripts/check-dead-strip-ratchet.sh"
cd "$W" && ./build/human_tests 2>&1 | grep -E 'Results:'
git -C "$W" add include/human/memory/graph.h src/memory/graph.c include/human/agent/graph_grounding.h src/agent/graph_grounding.c tests/test_graph.c tests/test_graph_grounding.c
git -C "$W" commit -m "feat(grounding): HU_GRAPH_NAMES names-first seeding (default OFF)

468/572 entities are UNKNOWN and 308 lowercase phrases outrank real names,
so grounding rarely names anyone. LIVE seeds only typed names and
Capitalized UNKNOWNs (typed +0.5), and moves TOPICs to one 'Been talking
about:' line. SHADOW composes both, injects OFF and logs the sizes. OFF is
byte-identical (golden).

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: `human memory ground --full`

**Files:**
- Modify: `include/human/cli_commands.h`
- Modify: `src/app/cli_commands.c` (`memory_ground_probe` ~:512-550; the `cmd_memory` arity check ~:596 and the dispatch ~:873)
- Test: `tests/test_cli_commands_memory_print.c`

**Interfaces:**
- Consumes: `hu_graph_ground_compose_turn`, `hu_graph_ground_turn_flags_from_env`, `hu_graph_ground_turn_stats_t` (Tasks 4 and 5).
- Produces:
  - `bool hu_cli_parse_ground_args(int argc, char **argv, const char **contact_out, const char **msg_out, bool *full_out);`
  - `void hu_cli_memory_ground_emit(FILE *out, bool full, size_t matched, const struct hu_graph_ground_turn_stats *stats, const char *ctx, size_t ctx_len);`
  - CLI contract that Task 9 parses: `human memory ground --full <contact> <message>` prints `matched=<n> bytes=<b> fallback=<0|1> self=<0|1> names=<n>\n` then the block, if any, followed by `\n`. `HU_GRAPH_DB` overrides the graph path. Gates come from the environment.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_cli_commands_memory_print.c` (includes: add `#include "human/agent/graph_grounding.h"` and `#include <stdio.h>`):

```c
static void test_ground_args_plain_and_full(void) {
    const char *contact = NULL, *msg = NULL;
    bool full = true;
    char *a1[] = {"human", "memory", "ground", "+15550000001", "hi there"};
    HU_ASSERT_TRUE(hu_cli_parse_ground_args(5, a1, &contact, &msg, &full));
    HU_ASSERT_STR_EQ(contact, "+15550000001");
    HU_ASSERT_STR_EQ(msg, "hi there");
    HU_ASSERT_FALSE(full);
    char *a2[] = {"human", "memory", "ground", "--full", "+15550000001", "hi there"};
    HU_ASSERT_TRUE(hu_cli_parse_ground_args(6, a2, &contact, &msg, &full));
    HU_ASSERT_TRUE(full);
    HU_ASSERT_STR_EQ(contact, "+15550000001");
    HU_ASSERT_STR_EQ(msg, "hi there");
    HU_ASSERT_FALSE(hu_cli_parse_ground_args(5, a2, &contact, &msg, &full)); /* --full, no msg */
    HU_ASSERT_FALSE(hu_cli_parse_ground_args(4, a1, &contact, &msg, &full));
    char *a3[] = {"human", "memory", "ground", "", "hi"};
    HU_ASSERT_FALSE(hu_cli_parse_ground_args(5, a3, &contact, &msg, &full));
}

static void read_all(FILE *f, char *buf, size_t cap) {
    rewind(f);
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
}

static void test_ground_emit_full_prints_turn_stats_then_block(void) {
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_graph_ground_turn_stats_t st;
    memset(&st, 0, sizeof(st));
    st.matched_entities = 2;
    st.via_fallback = true;
    st.typed_names = 1;
    const char *blk = "- Salim (person)\n";
    hu_cli_memory_ground_emit(f, true, 0, &st, blk, strlen(blk));
    char buf[256];
    read_all(f, buf, sizeof(buf));
    fclose(f);
    HU_ASSERT_STR_EQ(buf, "matched=2 bytes=17 fallback=1 self=0 names=1\n- Salim (person)\n\n");
}

static void test_ground_emit_plain_keeps_the_old_shape(void) {
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_cli_memory_ground_emit(f, false, 3, NULL, NULL, 0);
    char buf[64];
    read_all(f, buf, sizeof(buf));
    fclose(f);
    HU_ASSERT_STR_EQ(buf, "matched=3 bytes=0\n");
}
```
Register the three tests in `run_cli_commands_memory_print_tests`.

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cmake --build "$W/build" --target human_tests -j8 2>&1 | grep -E 'error' | head
```
Expected: implicit declaration of `hu_cli_parse_ground_args`.

- [ ] **Step 2: Declare in `include/human/cli_commands.h`** (before the final `#endif`)

```c
/* `human memory ground [--full] <contact> <message>` (argv[3..]). False when
 * the contact is missing/empty or the message is missing. Pure. */
bool hu_cli_parse_ground_args(int argc, char **argv, const char **contact_out,
                              const char **msg_out, bool *full_out);

struct hu_graph_ground_turn_stats;
/* `human memory ground` output. Plain (lexical compose only, unchanged):
 *   "matched=<n> bytes=<b>"
 * --full (hu_graph_ground_compose_turn, the live turn's composition):
 *   "matched=<n> bytes=<b> fallback=<0|1> self=<0|1> names=<typed lines>"
 * then the block, if any. scripts/eval_name_grounding.py parses this shape. */
void hu_cli_memory_ground_emit(FILE *out, bool full, size_t matched,
                               const struct hu_graph_ground_turn_stats *stats, const char *ctx,
                               size_t ctx_len);
```

- [ ] **Step 3: Implement in `src/app/cli_commands.c`**

Directly above `memory_ground_probe`, add:

```c
bool hu_cli_parse_ground_args(int argc, char **argv, const char **contact_out,
                              const char **msg_out, bool *full_out) {
    *contact_out = NULL;
    *msg_out = NULL;
    *full_out = false;
    if (!argv || argc < 5)
        return false;
    int i = 3;
    if (argv[3] && strcmp(argv[3], "--full") == 0) {
        *full_out = true;
        i = 4;
    }
    if (argc < i + 2 || !argv[i] || !argv[i + 1] || !argv[i][0])
        return false;
    *contact_out = argv[i];
    *msg_out = argv[i + 1];
    return true;
}

void hu_cli_memory_ground_emit(FILE *out, bool full, size_t matched,
                               const hu_graph_ground_turn_stats_t *stats, const char *ctx,
                               size_t ctx_len) {
    if (!out)
        return;
    if (full && stats)
        fprintf(out, "matched=%zu bytes=%zu fallback=%d self=%d names=%zu\n",
                stats->matched_entities, ctx_len, stats->via_fallback ? 1 : 0,
                stats->via_self ? 1 : 0, stats->typed_names);
    else
        fprintf(out, "matched=%zu bytes=%zu\n", matched, ctx_len);
    if (ctx && ctx_len)
        fprintf(out, "%.*s\n", (int)ctx_len, ctx);
}
```

Replace `memory_ground_probe`'s signature, comment and the `if (err == HU_OK) { … }` block:

```c
/* human memory ground [--full] <contact> <message> — the proof probe.
 * Plain: lexical compose only (the 2026-09-01 backfill probe). --full: the
 * live turn's composition (hu_graph_ground_compose_turn) under the SAME env
 * gates, so scripts/eval_name_grounding.py measures the real path. */
static hu_error_t memory_ground_probe(hu_allocator_t *alloc, hu_memory_t *mem, const char *contact,
                                      const char *msg, bool full) {
```

```c
    if (err == HU_OK) {
        hu_memory_loader_set_facade(&loader, facade);
        char *ctx = NULL;
        size_t ctx_len = 0, matched = 0;
        hu_graph_ground_turn_stats_t st;
        memset(&st, 0, sizeof(st));
        if (full)
            err = hu_graph_ground_compose_turn(&loader, contact, strlen(contact), msg,
                                               strlen(msg), hu_graph_ground_turn_flags_from_env(),
                                               &ctx, &ctx_len, &st);
        else
            err = hu_graph_ground_compose(&loader, contact, strlen(contact), msg, strlen(msg), 0,
                                          &ctx, &ctx_len, &matched);
        hu_cli_memory_ground_emit(stdout, full, matched, &st, ctx, ctx_len);
        if (ctx)
            alloc->free(alloc->ctx, ctx, ctx_len + 1);
    }
```

In `cmd_memory`, directly after `const char *sub = argv[2];`, add:

```c
    const char *ground_contact = NULL, *ground_msg = NULL;
    bool ground_full = false;
```

Replace the arity check:

```c
    if (strcmp(sub, "ground") == 0 && argc < 5) {
        fprintf(stderr, "Usage: human memory ground <contact> <message>\n");
        return HU_ERR_INVALID_ARGUMENT;
    }
```

with:

```c
    if (strcmp(sub, "ground") == 0 &&
        !hu_cli_parse_ground_args(argc, argv, &ground_contact, &ground_msg, &ground_full)) {
        fprintf(stderr, "Usage: human memory ground [--full] <contact> <message>\n");
        return HU_ERR_INVALID_ARGUMENT;
    }
```

and the dispatch `err = memory_ground_probe(alloc, &mem, argv[3], argv[4]);` with:

```c
        err = memory_ground_probe(alloc, &mem, ground_contact, ground_msg, ground_full);
```

- [ ] **Step 4: Run to verify pass**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cmake --build "$W/build" --target human human_tests -j8 2>&1 | grep -E 'error|warning' | head
cd "$W" && ./build/human_tests --suite=cli_commands_memory_print 2>&1 | tail -3
grep -n 'hu_graph_ground_compose_turn\|hu_cli_memory_ground_emit(stdout' "$W/src/app/cli_commands.c"   # the live CLI callers
```
Expected: 0 failed; the grep shows both call sites in `memory_ground_probe`.

- [ ] **Step 5: Gates, full suite, commit**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
bash "$W/scripts/check-clone-ratchet.sh"
cd "$W" && ./build/human_tests 2>&1 | grep -E 'Results:'
git -C "$W" add include/human/cli_commands.h src/app/cli_commands.c tests/test_cli_commands_memory_print.c
git -C "$W" commit -m "feat(cli): memory ground --full measures the live turn's composition

The probe ran lexical compose only, so the 2026-09-27 0/40 recall number
could not see the fallback, the owner facts or the names gate. --full calls
hu_graph_ground_compose_turn under the same env gates and prints its stats.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Nightly typed-name pass (`insight_stream.py --names`)

**Files:**
- Create: `scripts/curator_names.py`
- Modify: `scripts/insight_stream.py` (imports ~:691; `write_manifest` ~:723; `run_wide` refusals ~:990-1010; new functions before `WIDE_REFUSED_FLAGS`; `main` ~:1005)
- Test: `tests/test_insight_stream_names_pass.py` (the name `tests/test_insight_stream_names.py` is taken; the CI glob `tests/test_insight_stream_*.py` covers this file)

**Interfaces:**
- Consumes: `curator_evidence` (`chat_turn_rows`, `number_rows`, `parse_evidence`, `name_said`), `curator_population` (`eligible_handles`, `exclusion_reason`, `load_suppressed`, `load_never`, `load_loopback_handles`), `insight_stream.call_model`, `resolve_deadline`, and the Task 2 CLI output `{"entities": E}`.
- Produces (`curator_names`): `TYPES`, `SOURCE="names:nightly"`, `CONFIDENCE=0.8`, `build_prompt(lines) -> (system, user)`, `parse_names(text) -> list[dict]`, `drop_names(display_name) -> set[str]`, `canonical_name(name, ntype) -> str`, `verify_names(proposed, cite, drop) -> (kept, rejected)`, `entity_lines(handle, kept, source=SOURCE, confidence=CONFIDENCE, retype_only=False) -> list[dict]`, `write_jsonl_private(path, lines) -> path`, `parse_import_output(stdout) -> int|None`, `run_import(human_bin, graph_db, path, timeout=600) -> (int|None, returncode)`. Task 8 reuses the last four.
- Produces (`insight_stream`): `preflight(a) -> (rc|None, loopback)`, `names_eligible(att, now, names_days, window_days, exclude)`, `names_pass(a, contacts, att, now, deadline=None, exclude=()) -> (manifest, lines)`, `run_names(a, contacts) -> int`, `write_manifest(..., prefix="curator-manifest")`. New flags: `--names`, `--names-days`, `--names-dir`, `--graph-db`, `--human-bin`.

- [ ] **Step 1: Write the failing tests** — `tests/test_insight_stream_names_pass.py`

```python
"""--names: nightly typed-name pass (spec 2026-09-29 §4.4).

Hermetic like test_insight_stream_wide.py: MEMORY_DB, CURATOR_STATE and
HUMAN_CONFIG point into tmp_path (autouse fixture imported below); the model,
chat.db attribution and the health probe are monkeypatched; the `human`
binary is a fake script in tmp_path that records its argv and env.
"""
import datetime as dt
import json
import os
import sqlite3
import stat
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
sys.path.insert(0, str(Path(__file__).parent))
import curator_evidence as ce  # noqa: E402
import curator_names as cn  # noqa: E402
import insight_stream as ins  # noqa: E402
from test_insight_stream_wide import A, H, NOW, timeline, _hermetic_memory_db  # noqa: E402,F401

LABELS = {**{f"me{i}": "seth" for i in range(6)}, "bot": "huuman"}
MODEL = json.dumps([
    {"name": "priya", "type": "person", "evidence": ["t0"]},
    {"name": "Marcus", "type": "person", "evidence": ["d0"]},
    {"name": "Dana", "type": "person", "evidence": ["t1"]},
    {"name": "Sam", "type": "person", "evidence": ["t0"]},
    {"name": "surgery", "type": "topic", "evidence": ["t0"]},
    {"name": "Tuesday", "type": "weird", "evidence": ["t0"]},
])

FAKE_BIN = """#!{py}
import json, os, sys
with open(os.environ["FAKE_LOG"], "w") as f:
    json.dump({{"argv": sys.argv[1:], "graph": os.environ.get("HU_GRAPH_DB"),
               "lines": open(sys.argv[3]).read().splitlines()}}, f)
print(json.dumps({{"imported": 0, "entities": int(os.environ.get("FAKE_ENTITIES", "2")),
                  "skipped": 0, "graph": "x"}}))
sys.exit(int(os.environ.get("FAKE_RC", "0")))
"""


class NA(A):
    names_days = 2
    window_days = 30
    deadline = None
    write = True


def _fake_bin(tmp_path, monkeypatch):
    p = tmp_path / "human"
    p.write_text(FAKE_BIN.format(py=sys.executable))
    p.chmod(0o755)
    monkeypatch.setenv("FAKE_LOG", str(tmp_path / "fake.json"))
    return str(p)


def _args(tmp_path, monkeypatch, write=True):
    a = NA()
    a.write = write
    a.names_dir = str(tmp_path / "names")
    a.manifest_dir = str(tmp_path / "manifests")
    a.graph_db = str(tmp_path / "graph.db")
    a.human_bin = _fake_bin(tmp_path, monkeypatch)
    chat = tmp_path / "chat.db"
    c = sqlite3.connect(chat)
    c.execute("CREATE TABLE message (rowid INTEGER)")
    c.commit(); c.close()
    a.chat_db = str(chat)
    monkeypatch.setattr(ins, "_utc_now", lambda: NOW)  # run_names' clock; timeline() is NOW-relative
    monkeypatch.setattr(ins.urllib.request, "urlopen", lambda *x, **k: None)
    import eval_conversation_quality as cq
    monkeypatch.setattr(cq, "attribute",
                        lambda *x, **k: {"timelines": {H: timeline()}, "labels": LABELS})
    return a


def _cite():
    rows = ce.chat_turn_rows(timeline(), LABELS, 80, NOW - dt.timedelta(days=2))
    return ce.number_rows(rows)[1]


def test_parse_names_drops_malformed():
    assert cn.parse_names("no json") == []
    assert cn.parse_names('[{"name": ""}, "x", {"name": "Priya", "type": "Person"}]') == [
        {"name": "Priya", "type": "person", "evidence_tokens": []}]


def test_verify_names_keeps_only_said_cited_names():
    kept, rejected = cn.verify_names(cn.parse_names(MODEL), _cite(), cn.drop_names("Sam"))
    assert kept == [{"name": "Priya", "type": "person"}, {"name": "surgery", "type": "topic"}]
    # Marcus cites a daemon row, Dana is not said in t1, Sam is dropped, Tuesday's type is bad
    assert rejected == 4


def test_verify_names_capitalizes_name_types():
    """Review Focus 2: texting-lowercase names land on the capitalized row."""
    assert cn.canonical_name("priya", "person") == "Priya"
    assert cn.canonical_name("st pete", "place") == "St Pete"
    assert cn.canonical_name("the lake house", "topic") == "the lake house"
    assert cn.canonical_name("McKinsey", "org") == "McKinsey"


def test_drop_names_covers_seth_and_the_contact():
    d = cn.drop_names("Sam Rivera")
    assert {"seth", "seth ford", "sam rivera", "sam"} <= d


def test_write_jsonl_private_is_0600(tmp_path):
    p = cn.write_jsonl_private(str(tmp_path / "d" / "n.jsonl"), [{"a": 1}])
    assert stat.S_IMODE(os.stat(p).st_mode) == 0o600
    assert json.loads(open(p).read()) == {"a": 1}


def test_parse_import_output():
    assert cn.parse_import_output('log\n{"imported": 0, "entities": 3, "skipped": 1}\n') == 3
    assert cn.parse_import_output("garbage") is None


def test_names_pass_includes_persona_contacts_and_counts_only(monkeypatch):
    monkeypatch.setattr(ins, "call_model", lambda *x, **k: MODEL)
    # The contact IS Priya: her own display name must never become an entity.
    man, lines = ins.names_pass(NA(), {H: {"name": "Priya Shah"}},
                                {"timelines": {H: timeline()}, "labels": LABELS}, NOW)
    assert man["eligible"] == 1 and man["contacts"] == 1  # a persona contact is NOT skipped
    assert man["names_proposed"] == 6
    assert man["names_kept"] == 1 and man["names_rejected"] == 5
    assert man["by_type"]["topic"] == 1 and man["by_type"]["person"] == 0
    assert lines == [{"kind": "entity", "contact": H, "name": "surgery", "type": "topic",
                      "source": "names:nightly", "confidence": 0.8}]
    blob = json.dumps(man)
    assert "surgery" not in blob and H not in blob


def test_quiet_contact_is_not_eligible(monkeypatch):
    old = [dict(m, t=m["t"] - dt.timedelta(days=5)) for m in timeline()]
    monkeypatch.setattr(ins, "call_model", lambda *x, **k: pytest.fail("model called"))
    man, lines = ins.names_pass(NA(), {}, {"timelines": {H: old}, "labels": LABELS}, NOW)
    assert man["eligible"] == 0 and lines == []


def test_model_error_on_a_contact_is_counted_not_fatal(monkeypatch):
    def boom(*x, **k):
        raise TimeoutError("down")
    monkeypatch.setattr(ins, "call_model", boom)
    man, lines = ins.names_pass(NA(), {}, {"timelines": {H: timeline()}, "labels": LABELS}, NOW)
    assert man["model_errors"] == 1 and man["contacts"] == 0 and lines == []


def test_deadline_stops_before_the_next_contact(monkeypatch):
    monkeypatch.setattr(ins, "call_model", lambda *x, **k: pytest.fail("model called"))
    man, _ = ins.names_pass(NA(), {}, {"timelines": {H: timeline()}, "labels": LABELS}, NOW,
                            deadline=NOW - dt.timedelta(minutes=1))
    assert man["stopped_at_deadline"] == 1 and man["unreached_at_deadline"] == 1


def test_run_names_write_imports_through_the_binary(tmp_path, monkeypatch):
    a = _args(tmp_path, monkeypatch)
    monkeypatch.setattr(ins, "call_model", lambda *x, **k: MODEL)
    assert ins.run_names(a, {}) == 0
    log = json.load(open(tmp_path / "fake.json"))
    assert log["argv"][:2] == ["memory", "import-facts"]
    assert log["graph"] == a.graph_db
    assert [json.loads(ln)["name"] for ln in log["lines"]] == ["Priya", "surgery"]
    jsonl = log["argv"][2]
    assert stat.S_IMODE(os.stat(jsonl).st_mode) == 0o600
    man = json.load(open(next((tmp_path / "manifests").glob("names-manifest-*.json"))))
    assert man["import_entities"] == 2 and man["import_failed"] == 0 and not man["dry_run"]


def test_run_names_dry_run_never_imports(tmp_path, monkeypatch):
    a = _args(tmp_path, monkeypatch, write=False)
    monkeypatch.setattr(ins, "call_model", lambda *x, **k: MODEL)
    assert ins.run_names(a, {}) == 0
    assert not (tmp_path / "fake.json").exists()
    assert list((tmp_path / "manifests").glob("names-manifest-*-dryrun.json"))


def test_run_names_import_failure_exits_2(tmp_path, monkeypatch):
    a = _args(tmp_path, monkeypatch)
    monkeypatch.setenv("FAKE_RC", "1")
    monkeypatch.setattr(ins, "call_model", lambda *x, **k: MODEL)
    assert ins.run_names(a, {}) == 2
    man = json.load(open(next((tmp_path / "manifests").glob("names-manifest-*.json"))))
    assert man["import_failed"] == 1 and man["import_entities"] == 0


def test_run_names_every_contact_errored_exits_3(tmp_path, monkeypatch):
    a = _args(tmp_path, monkeypatch)
    def boom(*x, **k):
        raise TimeoutError("down")
    monkeypatch.setattr(ins, "call_model", boom)
    assert ins.run_names(a, {}) == 3
    assert not (tmp_path / "fake.json").exists()


def test_run_names_refuses_without_the_binary(tmp_path, monkeypatch):
    a = _args(tmp_path, monkeypatch)
    a.human_bin = str(tmp_path / "missing")
    monkeypatch.setattr(ins, "call_model", lambda *x, **k: pytest.fail("model called"))
    assert ins.run_names(a, {}) == 2


def test_main_refuses_names_with_wide(monkeypatch, capsys):
    def no_persona():
        raise AssertionError("persona touched")
    monkeypatch.setattr(ins, "load_persona", no_persona)
    assert ins.main(["--names", "--population", "wide"]) == 2
    assert "--names" in capsys.readouterr().err
```

`run_names` reads its clock through `_utc_now()` so the NOW-relative fixtures stay eligible whatever the date. The helper exists already and `_args` patches it.

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cd "$W" && python3 -m pytest -q tests/test_insight_stream_names_pass.py 2>&1 | tail -3
```
Expected: `ModuleNotFoundError: No module named 'curator_names'`.

- [ ] **Step 2: Create `scripts/curator_names.py`**

```python
"""Nightly typed-name pass helpers (spec 2026-09-29-named-entity-extraction §4.4).

Pure over curator_evidence rows, so every decision is testable without chat.db,
a model or the human binary. Only [tN] rows (the contact's texts and Seth's own)
are evidence; a name is kept only when it is said, word-bounded and
case-insensitively, in a row it cites. Python never writes graph.db: kept names
become entity lines for `human memory import-facts`.
"""
import json
import os
import re
import subprocess

import curator_evidence as ce

TYPES = ("person", "place", "org", "event", "topic")
SOURCE = "names:nightly"
CONFIDENCE = 0.8
MAX_NAME_LEN = 60
SELF_NAMES = frozenset({"seth", "seth ford"})

SYSTEM = (
    "You are reading Seth Ford's recent texts with one person. List the specific names "
    "the texts mention: people, places, organizations, named events (a trip, a wedding, a "
    "game), and recurring topics (a short lowercase phrase, e.g. \"the lake house\"). Only "
    "names written in a [tN] text; never a [dN] text. Never Seth himself and never the "
    "person he is texting.\n\n"
    "Output ONLY a JSON array of objects: {\"name\": the name exactly as written, "
    "\"type\": \"person\"|\"place\"|\"org\"|\"event\"|\"topic\", \"evidence\": [the N of "
    "each [tN] text that contains the name]}. No prose before or after."
)


def build_prompt(lines):
    return SYSTEM, "texts (oldest first):\n" + "\n".join(lines)


def parse_names(text):
    """Model output -> [{"name", "type", "evidence_tokens"}]; malformed items dropped."""
    m = re.search(r"\[[\s\S]*\]", text or "")
    if not m:
        return []
    try:
        arr = json.loads(m.group(0))
    except ValueError:
        return []
    out = []
    for o in arr if isinstance(arr, list) else []:
        if not isinstance(o, dict):
            continue
        name = str(o.get("name") or "").strip()
        ev = o.get("evidence") if isinstance(o.get("evidence"), list) else []
        if name:
            out.append({"name": name, "type": str(o.get("type") or "").strip().lower(),
                        "evidence_tokens": [str(e) for e in ev]})
    return out


def drop_names(display_name):
    """Lowercased names never written for this contact: Seth's and the contact's own."""
    out = set(SELF_NAMES)
    dn = (display_name or "").strip().lower()
    if dn:
        out.add(dn)
        out.add(dn.split()[0])
    return out


def canonical_name(name, ntype):
    """Name types are stored with capitalized words ("priya" -> "Priya", "st pete" ->
    "St Pete"), so lowercase texting lands on the same row as the per-turn catcher's
    Capitalized names. Topics keep the model's spelling."""
    if ntype == "topic":
        return name
    return " ".join(w[:1].upper() + w[1:] if w[:1].islower() else w for w in name.split())


def verify_names(proposed, cite, drop):
    """-> (kept [{"name", "type"}], rejected count). Kept only when the type is known,
    the name is 2-60 chars and not dropped, it cites >= 1 [tN] row and no [dN] row, and
    it is said in a cited row. One entry per canonical name (first wins)."""
    kept, seen, rejected = [], set(), 0
    for p in proposed:
        name, ntype = p["name"], p["type"]
        canon = canonical_name(name, ntype)
        if canon.lower() in seen:
            continue
        t_idx, daemon = ce.parse_evidence(p["evidence_tokens"])
        rows = [cite[i] for i in t_idx if i in cite]
        ok = (ntype in TYPES and 2 <= len(name) <= MAX_NAME_LEN and name.lower() not in drop
              and not daemon and bool(rows) and ce.name_said(name, [r[3] for r in rows]))
        if not ok:
            rejected += 1
            continue
        seen.add(canon.lower())
        kept.append({"name": canon, "type": ntype})
    return kept, rejected


def entity_lines(handle, kept, source=SOURCE, confidence=CONFIDENCE, retype_only=False):
    lines = []
    for k in kept:
        line = {"kind": "entity", "contact": handle, "name": k["name"], "type": k["type"],
                "source": source, "confidence": confidence}
        if retype_only:
            line["retype_only"] = True
        lines.append(line)
    return lines


def write_jsonl_private(path, lines):
    """0600 from creation: the file holds contact handles and names."""
    os.makedirs(os.path.dirname(path), mode=0o700, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.fchmod(fd, 0o600)
    with os.fdopen(fd, "w") as f:
        for line in lines:
            f.write(json.dumps(line, sort_keys=True) + "\n")
    return path


def parse_import_output(stdout):
    """`human memory import-facts` prints one JSON object -> its "entities", else None."""
    for line in reversed((stdout or "").splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                return int(json.loads(line).get("entities", 0))
            except (ValueError, TypeError, AttributeError):
                return None
    return None


def run_import(human_bin, graph_db, path, timeout=600):
    """-> (entities imported or None, returncode). Every graph write goes through the
    C importer; HU_GRAPH_DB pins which graph it writes."""
    env = {**os.environ, "HU_GRAPH_DB": graph_db}
    try:
        r = subprocess.run([human_bin, "memory", "import-facts", path], env=env,
                           capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None, -1
    return parse_import_output(r.stdout), r.returncode
```

- [ ] **Step 3: Wire `--names` into `scripts/insight_stream.py`**

a) Next to `import curator_population as cp  # noqa: E402`, add:

```python
import curator_names as cn  # noqa: E402
```

b) Change `write_manifest`'s signature and path line:

```python
def write_manifest(manifest_dir, now, man, prefix="curator-manifest"):
```
```python
    path = os.path.join(manifest_dir, f"{prefix}-{now.strftime('%Y%m%d')}{suffix}.json")
```

c) Above `def run_wide(`, add `preflight`:

```python
def preflight(a):
    """Refusals shared by the chat.db passes -> (exit code or None, loopback handles).
    chat.db unreadable, model server down, or a malformed never-file/config.json."""
    try:
        con = sqlite3.connect(f"file:{a.chat_db}?mode=ro", uri=True)
        con.execute("SELECT 1 FROM message LIMIT 1").fetchall()
        con.close()
    except sqlite3.Error as e:
        print(f"refusing: chat.db unreadable ({e}); grant Full Disk Access to this "
              "python for the launchd job", file=sys.stderr)
        return 2, set()
    try:
        urllib.request.urlopen(a.url.rsplit("/v1/", 1)[0] + "/health", timeout=5)
    except Exception as e:
        print(f"refusing: model server down ({e})", file=sys.stderr)
        return 2, set()
    try:
        cp.load_never(a.never_path)
        return None, cp.load_loopback_handles(HUMAN_CONFIG)
    except (ValueError, json.JSONDecodeError) as e:
        print(f"refusing: {e}", file=sys.stderr)
        return 2, set()
```

In `run_wide`, replace the three `try:` blocks (chat.db, health, never/loopback) with:

```python
    rc, loopback = preflight(a)
    if rc is not None:
        return rc
```

(The messages are identical, so the existing wide tests stay green.)

d) Directly above `WIDE_REFUSED_FLAGS = (`, add the names pass:

```python
# ---- nightly typed-name pass (spec 2026-09-29 named-entity-extraction §4.4) ----
NAMES_COUNTERS = (
    "eligible", "contacts", "excluded_suppressed", "excluded_never", "skipped_no_text",
    "model_errors", "names_proposed", "names_kept", "names_rejected", "import_entities",
    "import_failed", "unreached_at_deadline", "stopped_at_deadline")
NAMES_DIR = os.path.join(HOME, ".human/names")
GRAPH_DB = os.path.join(HOME, ".human/graph.db")
HUMAN_BIN = os.path.join(HOME, ".local/bin/human-daemon")
NAMES_MAX_TOKENS = 1200


def names_eligible(att, now, names_days, window_days, exclude):
    """The wide pass's enumeration WITHOUT its persona skip (persona contacts are the
    ones the daemon replies to), narrowed to handles with any message, either
    direction, in the last names_days."""
    handles = cp.eligible_handles(att["timelines"], set(), now, window_days=window_days,
                                  exclude=exclude)
    cutoff = now - dt.timedelta(days=names_days)
    return [h for h in handles if any(m["t"] >= cutoff for m in att["timelines"][h])]


def names_contact(a, rows, display_name):
    """One contact: one model call (thinking suppressed by call_model), deterministic
    verification. -> (kept, proposed count, rejected count). Raises on a model error."""
    lines, cite = ce.number_rows(rows)
    system, user = cn.build_prompt(lines)
    proposed = cn.parse_names(call_model(a.url, a.model, system, user,
                                         max_tokens=NAMES_MAX_TOKENS))
    kept, rejected = cn.verify_names(proposed, cite, cn.drop_names(display_name))
    return kept, len(proposed), rejected


def names_pass(a, contacts, att, now, deadline=None, exclude=()):
    """-> (counts-only manifest, entity lines). Every eligible handle lands in exactly
    one of excluded_suppressed, excluded_never, skipped_no_text, contacts,
    model_errors, unreached_at_deadline."""
    man = dict.fromkeys(NAMES_COUNTERS, 0)
    man["by_type"] = dict.fromkeys(cn.TYPES, 0)
    man["dry_run"] = not a.write
    suppressed = cp.load_suppressed(MEMORY_DB)
    never = cp.load_never(getattr(a, "never_path", cp.NEVER_PATH))
    handles = names_eligible(att, now, a.names_days, a.window_days, exclude)
    man["eligible"] = len(handles)
    cutoff = now - dt.timedelta(days=a.names_days)
    lines = []
    for i, h in enumerate(handles):
        if deadline is not None and _utc_now() >= deadline:
            man["stopped_at_deadline"] = 1
            man["unreached_at_deadline"] = len(handles) - i
            break
        why = cp.exclusion_reason(h, suppressed, never)
        if why:
            man[f"excluded_{why}"] += 1
            continue
        rows = ce.chat_turn_rows(att["timelines"][h], att["labels"], a.turns, cutoff)
        if not any(r[2] != "daemon" for r in rows):
            man["skipped_no_text"] += 1
            continue
        try:
            kept, proposed, rejected = names_contact(a, rows, (contacts.get(h) or {}).get("name"))
        except Exception:  # one contact's model failure must not abort the night
            man["model_errors"] += 1
            continue
        man["contacts"] += 1
        man["names_proposed"] += proposed
        man["names_rejected"] += rejected
        man["names_kept"] += len(kept)
        for k in kept:
            man["by_type"][k["type"]] += 1
        lines.extend(cn.entity_lines(h, kept))
    return man, lines


def run_names(a, contacts):
    """The --names dispatch. Order of effects:
    1. deadline window closed -> exit 0, nothing written;
    2. refusals, exit 2, nothing written: chat.db unreadable, model server down,
       malformed never-file/config.json, --write without an executable
       --human-bin, 0 eligible contacts;
    3. the pass; kept names -> a 0600 JSONL in --names-dir;
    4. --write only: `human memory import-facts` (HU_GRAPH_DB=--graph-db);
    5. counts-only manifest (dry runs as ...-dryrun.json);
    6. exit 3 if every attempted contact hit a model error; exit 2 if the import
       failed (nothing written)."""
    import eval_conversation_quality as cq
    deadline = None
    if a.deadline:
        deadline = resolve_deadline(a.deadline, _local_now())
        if deadline is None:
            print(f"window closed: deadline {a.deadline} passed less than "
                  f"{DEADLINE_ROLL_HOURS}h ago; nothing to do", file=sys.stderr)
            return 0
    rc, loopback = preflight(a)
    if rc is not None:
        return rc
    if a.write and not os.access(a.human_bin, os.X_OK):
        print(f"refusing: human binary not executable ({a.human_bin})", file=sys.stderr)
        return 2
    now = _utc_now()
    att = cq.attribute(a.chat_db, MEMORY_DB, now - dt.timedelta(days=a.window_days))
    if not names_eligible(att, now, a.names_days, a.window_days, loopback):
        print("refusing: 0 eligible contacts (no manifest written)", file=sys.stderr)
        return 2
    t0 = time.monotonic()
    try:
        man, lines = names_pass(a, contacts, att, now, deadline, exclude=loopback)
    except sqlite3.Error as e:
        print(f"refusing: memory.db unreadable ({e})", file=sys.stderr)
        return 2
    rc = 0
    if lines:
        path = cn.write_jsonl_private(
            os.path.join(a.names_dir, f"names-{now.strftime('%Y%m%d')}.jsonl"), lines)
        if a.write:
            entities, code = cn.run_import(a.human_bin, a.graph_db, path)
            if code != 0 or entities is None:
                man["import_failed"] = 1
                print("import failed: `human memory import-facts` did not succeed; nothing "
                      "written", file=sys.stderr)
                rc = 2
            else:
                man["import_entities"] = entities
    man["elapsed_s"] = round(time.monotonic() - t0, 3)
    wrc = write_manifest(a.manifest_dir, now, man, prefix="names-manifest")
    attempted = man["contacts"] + man["model_errors"]
    if attempted and man["model_errors"] == attempted:
        print(f"every attempted contact ({attempted}) hit a model error; see the manifest",
              file=sys.stderr)
        return 3
    return rc or wrc
```

e) In `main`, add the flags after `--never-path`:

```python
    ap.add_argument("--names", action="store_true",
                    help="nightly typed-name pass: the local model lists the names in each "
                         "eligible contact's last --names-days of texts, each verified against "
                         "a cited text, imported into graph.db via `human memory import-facts` "
                         "(--write); dry-run otherwise")
    ap.add_argument("--names-days", type=int, default=2)
    ap.add_argument("--names-dir", default=NAMES_DIR)
    ap.add_argument("--graph-db", default=GRAPH_DB)
    ap.add_argument("--human-bin", default=HUMAN_BIN)
```

Directly after `a = ap.parse_args(argv)`, before the `if a.population == "wide":` refusal:

```python
    if a.names:
        bad = [flag for attr, flag in WIDE_REFUSED_FLAGS if getattr(a, attr)]
        if a.population == "wide":
            bad.append("--population wide")
        if bad:
            print(f"refusing: {', '.join(bad)} cannot be combined with --names",
                  file=sys.stderr)
            return 2
```

Directly after `identity, contacts = load_persona()` (before `db = sqlite3.connect(MEMORY_DB)`):

```python
    if a.names:
        return run_names(a, contacts)
```

Add to the module docstring's usage block:
`scripts/insight_stream.py --names [--names-days 2] [--deadline HH:MM] [--write]`.

- [ ] **Step 4: Run to verify pass (and the whole insight/curator suite)**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cd "$W" && python3 -m pytest -q tests/test_insight_stream_names_pass.py 2>&1 | tail -3
cd "$W" && python3 -m pytest -q tests/test_insight_stream_*.py tests/test_curator_*.py tests/test_second_opinion_*.py 2>&1 | tail -3
```
Expected: all pass. The wide tests must stay green after the `preflight` extraction.

- [ ] **Step 5: Commit**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
git -C "$W" add scripts/curator_names.py scripts/insight_stream.py tests/test_insight_stream_names_pass.py
git -C "$W" commit -m "feat(names): nightly typed-name pass (insight_stream.py --names)

Only 2 contact entities were created in 30 days. One local GLM call per
recently active 1:1 contact lists typed names; a name is kept only if it is
said in a cited human text, then imported as names:nightly entity lines via
the C importer. Counts-only manifest; persona contacts included.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: One-time migration (`scripts/graph_retype_entities.py`)

**Files:**
- Create: `scripts/graph_retype_entities.py`
- Test: `tests/test_graph_retype.py`
- Modify: `.github/workflows/ci.yml:1229` (append `tests/test_graph_retype.py` to the pytest line)

**Interfaces:**
- Consumes: `curator_names.TYPES/write_jsonl_private/run_import/entity_lines` (Task 7), `insight_stream.call_model`, the Task 2 importer (`retype_only` → NO_TOUCH).
- Produces: CLI `graph_retype_entities.py (--dry-run | --write) [--graph-db] [--backup-dir] [--work-dir] [--human-bin] [--url] [--model]`. Functions `unknown_entities(con) -> {contact: [name]}`, `batches(names, size=40)`, `parse_types(text, batch) -> {stored name: type}`, `lock_probe(path) -> str|None`, `backup(path, backup_dir, now) -> backup path`, `main(argv) -> int`.

- [ ] **Step 1: Write the failing tests** — `tests/test_graph_retype.py`

```python
"""graph_retype_entities.py (spec 2026-09-29 §4.5): backup before any write, refuse on a
failed backup or a locked graph, retype-only lines through the C importer.
Hermetic: temp sqlite, fake model, fake `human` binary; never ~/.human."""
import hashlib
import json
import os
import sqlite3
import stat
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
import graph_retype_entities as rt  # noqa: E402

C = "+15550000042"

FAKE_BIN = """#!{py}
import json, os, sys
lines = open(sys.argv[3]).read().splitlines()
with open(os.environ["FAKE_LOG"], "w") as f:
    json.dump({{"argv": sys.argv[1:], "graph": os.environ.get("HU_GRAPH_DB"), "lines": lines}}, f)
print(json.dumps({{"imported": 0, "entities": len(lines), "skipped": 0, "graph": "x"}}))
sys.exit(int(os.environ.get("FAKE_RC", "0")))
"""


def make_graph(path):
    con = sqlite3.connect(path)
    con.execute("CREATE TABLE entities (id INTEGER PRIMARY KEY AUTOINCREMENT, contact_id TEXT"
                " NOT NULL, name TEXT NOT NULL, type INTEGER NOT NULL DEFAULT 6, provenance TEXT)")
    con.executemany("INSERT INTO entities (contact_id, name, type) VALUES (?, ?, ?)", [
        (C, "Salim", 6), (C, "the lake house", 6), (C, "+15551234567", 6), (C, C, 6),
        (C, "Tampa", 1), ("self", "Vanguard", 6)])
    con.commit()
    con.close()


def digest(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


@pytest.fixture
def env(tmp_path, monkeypatch):
    g = tmp_path / "graph.db"
    make_graph(g)
    fake = tmp_path / "human"
    fake.write_text(FAKE_BIN.format(py=sys.executable))
    fake.chmod(0o755)
    monkeypatch.setenv("FAKE_LOG", str(tmp_path / "fake.json"))
    monkeypatch.setattr(rt.urllib.request, "urlopen", lambda *a, **k: None)
    argv = ["--graph-db", str(g), "--backup-dir", str(tmp_path / "backups"),
            "--work-dir", str(tmp_path / "work"), "--human-bin", str(fake)]
    return tmp_path, g, argv


def answer(url, model, system, user, **kw):
    names = user.split("\n")
    typed = {"Salim": "person", "the lake house": "topic", "Vanguard": "org"}
    return json.dumps([{"name": n, "type": typed[n]} for n in names if n in typed]
                      + [{"name": "Invented", "type": "person"}])


def test_unknown_entities_skips_placeholders_and_typed_rows(env):
    _, g, _ = env
    con = sqlite3.connect(g)
    assert rt.unknown_entities(con) == {C: ["Salim", "the lake house"], "self": ["Vanguard"]}


def test_parse_types_accepts_only_batch_names_and_known_types():
    raw = ('[{"name":"salim","type":"Person"},{"name":"Nobody","type":"person"},'
           '{"name":"the lake house","type":"planet"}]')
    assert rt.parse_types(raw, ["Salim", "the lake house"]) == {"Salim": "person"}
    assert rt.parse_types("nope", ["Salim"]) == {}


def test_batches_are_40():
    assert [len(b) for b in rt.batches([str(i) for i in range(85)])] == [40, 40, 5]


def test_dry_run_counts_only(env, monkeypatch, capsys):
    tmp_path, g, argv = env
    monkeypatch.setattr(rt, "call_model", lambda *a, **k: pytest.fail("model called"))
    assert rt.main(argv + ["--dry-run"]) == 0
    out = json.loads(capsys.readouterr().out)
    assert out == {"unknown_entities": 3, "contacts": 2, "batches": 2}
    assert not (tmp_path / "backups").exists()


def test_write_backs_up_first_then_imports_retype_only_lines(env, monkeypatch, capsys):
    tmp_path, g, argv = env
    order = []
    real_backup = rt.backup
    monkeypatch.setattr(rt, "backup", lambda *a: order.append("backup") or real_backup(*a))
    monkeypatch.setattr(rt, "call_model", lambda *a, **k: order.append("model") or answer(*a, **k))
    before = digest(g)
    assert rt.main(argv + ["--write"]) == 0
    assert order[0] == "backup" and "model" in order
    assert digest(g) == before  # Python never writes graph.db
    bk = list((tmp_path / "backups").glob("graph.db.bak-retype-*"))
    assert len(bk) == 1 and stat.S_IMODE(os.stat(bk[0]).st_mode) == 0o600
    assert sqlite3.connect(bk[0]).execute("PRAGMA integrity_check").fetchone()[0] == "ok"
    log = json.load(open(tmp_path / "fake.json"))
    assert log["graph"] == str(g)
    lines = [json.loads(ln) for ln in log["lines"]]
    assert {(ln["contact"], ln["name"], ln["type"]) for ln in lines} == {
        (C, "Salim", "person"), (C, "the lake house", "topic"), ("self", "Vanguard", "org")}
    assert all(ln["retype_only"] and ln["source"] == "names:migrate" for ln in lines)
    out = json.loads(capsys.readouterr().out.strip().splitlines()[-1])
    assert out["applied"] == 3 and out["unanswered"] == 0


def test_backup_failure_refuses_before_any_model_call(env, monkeypatch):
    tmp_path, _, argv = env
    def broken(*a):
        raise OSError("disk full")
    monkeypatch.setattr(rt, "backup", broken)
    monkeypatch.setattr(rt, "call_model", lambda *a, **k: pytest.fail("model called"))
    assert rt.main(argv + ["--write"]) == 2
    assert not (tmp_path / "fake.json").exists()


def test_locked_graph_refuses(env, monkeypatch):
    tmp_path, g, argv = env
    holder = sqlite3.connect(g)
    holder.execute("BEGIN IMMEDIATE")  # the daemon mid-write
    monkeypatch.setattr(rt, "call_model", lambda *a, **k: pytest.fail("model called"))
    try:
        assert rt.main(argv + ["--write"]) == 2
    finally:
        holder.rollback()
        holder.close()
    assert not (tmp_path / "backups").exists()  # refused before the backup


def test_every_batch_errored_exits_3_and_imports_nothing(env, monkeypatch):
    tmp_path, _, argv = env
    def boom(*a, **k):
        raise TimeoutError("down")
    monkeypatch.setattr(rt, "call_model", boom)
    assert rt.main(argv + ["--write"]) == 3
    assert not (tmp_path / "fake.json").exists()


def test_import_failure_exits_2(env, monkeypatch):
    _, _, argv = env
    monkeypatch.setenv("FAKE_RC", "1")
    monkeypatch.setattr(rt, "call_model", answer)
    assert rt.main(argv + ["--write"]) == 2


def test_refuses_a_remote_model(env):
    _, _, argv = env
    assert rt.main(argv + ["--write", "--url", "https://example.com/v1/chat/completions"]) == 2
```

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cd "$W" && python3 -m pytest -q tests/test_graph_retype.py 2>&1 | tail -3
```
Expected: `ModuleNotFoundError: No module named 'graph_retype_entities'`.

- [ ] **Step 2: Create `scripts/graph_retype_entities.py`**

```python
#!/usr/bin/env python3
"""One-time retype of UNKNOWN graph entities (spec 2026-09-29 named-entity-extraction §4.5).

Backs graph.db up with the SQLite online backup API (0600) BEFORE anything can
change it, asks the LOCAL model to type each contact's UNKNOWN names in batches
of 40, and writes the answers through `human memory import-facts` as
retype-only entity lines (source names:migrate). Python never writes graph.db.
Nothing is deleted and nothing is created: an unanswered name stays UNKNOWN,
and a name that vanished since the read is skipped by the importer.
Idempotent: a re-run only sees what is still UNKNOWN.

  --dry-run  counts only: no model, no backup, no import
  --write    lock probe -> backup -> classify -> import

Restore: stop the daemon, then `cp <backup> ~/.human/graph.db`.
"""
import argparse
import contextlib
import datetime as dt
import json
import os
import re
import sqlite3
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import curator_names as cn  # noqa: E402
from insight_stream import call_model  # noqa: E402,F401  (module attr so tests can patch)

HOME = os.path.expanduser("~")
UNKNOWN = 6  # HU_ENTITY_UNKNOWN in include/human/memory/graph.h
BATCH = 40
SOURCE = "names:migrate"
CONFIDENCE = 0.6
DEFAULT_URL = "http://127.0.0.1:8741/v1/chat/completions"
DEFAULT_MODEL = "GLM-4.5-Air-4bit"
PHONE_RE = re.compile(r"[+()\-. \d]{7,}")

SYSTEM = (
    "Each line below is a name or phrase from Seth Ford's texts with one person. Type "
    "each one: person, place, org (a company, school, team or group), event (a named "
    "occasion: a trip, a wedding, a game), or topic (anything that is not a proper name). "
    "Skip a line you cannot tell.\n\n"
    "Output ONLY a JSON array of objects: {\"name\": the line exactly as given, \"type\": "
    "\"person\"|\"place\"|\"org\"|\"event\"|\"topic\"}. No prose before or after."
)


def unknown_entities(con):
    """{contact: [name, ...]} of UNKNOWN rows, skipping the contact's own id and
    phone-number-shaped names (placeholders, not names)."""
    out = {}
    for cid, name in con.execute(
            "SELECT contact_id, name FROM entities WHERE type = ? ORDER BY contact_id, id",
            (UNKNOWN,)):
        if not name or name == cid or PHONE_RE.fullmatch(name):
            continue
        out.setdefault(cid, []).append(name)
    return out


def batches(names, size=BATCH):
    return [names[i:i + size] for i in range(0, len(names), size)]


def parse_types(text, batch):
    """Model answers -> {stored name: type}: only names of THIS batch (matched
    case-insensitively back to every stored spelling) with one of the five types."""
    by_lower = {}
    for n in batch:
        by_lower.setdefault(n.lower(), []).append(n)
    m = re.search(r"\[[\s\S]*\]", text or "")
    if not m:
        return {}
    try:
        arr = json.loads(m.group(0))
    except ValueError:
        return {}
    out = {}
    for o in arr if isinstance(arr, list) else []:
        if not isinstance(o, dict):
            continue
        t = str(o.get("type") or "").strip().lower()
        for stored in by_lower.get(str(o.get("name") or "").strip().lower(), []):
            if t in cn.TYPES and stored not in out:
                out[stored] = t
    return out


def lock_probe(path):
    """None when no other connection holds a write lock, else the error text.
    BEGIN IMMEDIATE + ROLLBACK takes and releases the lock without writing."""
    try:
        con = sqlite3.connect(path, timeout=1.0)
        try:
            con.execute("BEGIN IMMEDIATE")
            con.rollback()
        finally:
            con.close()
        return None
    except sqlite3.Error as e:
        return str(e)


def backup(path, backup_dir, now):
    """Online backup to <dir>/graph.db.bak-retype-<ts> (0600), verified by
    integrity_check and an equal entity count. Raises (partial file removed)."""
    os.makedirs(backup_dir, mode=0o700, exist_ok=True)
    dst_path = os.path.join(backup_dir, f"graph.db.bak-retype-{now.strftime('%Y%m%d-%H%M%S')}")
    os.close(os.open(dst_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600))
    try:
        src = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        dst = sqlite3.connect(dst_path)
        try:
            src.backup(dst)
            ok = dst.execute("PRAGMA integrity_check").fetchone()[0]
            n_src = src.execute("SELECT COUNT(*) FROM entities").fetchone()[0]
            n_dst = dst.execute("SELECT COUNT(*) FROM entities").fetchone()[0]
        finally:
            dst.close()
            src.close()
        if ok != "ok" or n_src != n_dst:
            raise RuntimeError(f"backup verification failed (integrity={ok}, "
                               f"entities {n_dst}/{n_src})")
        os.chmod(dst_path, 0o600)
        return dst_path
    except Exception:
        with contextlib.suppress(OSError):
            os.remove(dst_path)
        raise


def refuse(msg):
    print(f"refusing: {msg}; nothing written", file=sys.stderr)
    return 2


def main(argv=None):
    ap = argparse.ArgumentParser(description="Retype UNKNOWN graph entities (one-time).")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--write", action="store_true")
    ap.add_argument("--graph-db", default=os.path.join(HOME, ".human/graph.db"))
    ap.add_argument("--backup-dir", default=os.path.join(HOME, ".human/backups"))
    ap.add_argument("--work-dir", default=os.path.join(HOME, ".human/names"))
    ap.add_argument("--human-bin", default=os.path.join(HOME, ".local/bin/human-daemon"))
    ap.add_argument("--url", default=DEFAULT_URL)
    ap.add_argument("--model", default=DEFAULT_MODEL)
    a = ap.parse_args(argv)
    if not a.url.startswith(("http://127.0.0.1", "http://localhost")):
        return refuse("the migration reads real names and only talks to a local model")
    try:
        con = sqlite3.connect(f"file:{a.graph_db}?mode=ro", uri=True)
        todo = unknown_entities(con)
        con.close()
    except sqlite3.Error as e:
        return refuse(f"graph.db unreadable ({e})")
    counts = {"unknown_entities": sum(len(v) for v in todo.values()), "contacts": len(todo),
              "batches": sum(len(batches(v)) for v in todo.values())}
    if a.dry_run:
        print(json.dumps(counts, sort_keys=True))
        return 0
    if not os.access(a.human_bin, os.X_OK):
        return refuse(f"human binary not executable ({a.human_bin})")
    try:
        urllib.request.urlopen(a.url.rsplit("/v1/", 1)[0] + "/health", timeout=5)
    except Exception as e:
        return refuse(f"model server down ({e})")
    locked = lock_probe(a.graph_db)
    if locked:
        return refuse(f"graph.db is locked ({locked}); retry when the daemon is idle")
    now = dt.datetime.now()
    try:
        counts["backup"] = backup(a.graph_db, a.backup_dir, now)
    except Exception as e:
        return refuse(f"backup failed ({e})")
    lines, errors, by_type = [], 0, dict.fromkeys(cn.TYPES, 0)
    for cid, names in todo.items():
        for batch in batches(names):
            try:
                types = parse_types(call_model(a.url, a.model, SYSTEM, "\n".join(batch),
                                               max_tokens=2000), batch)
            except Exception:
                errors += 1
                continue
            kept = [{"name": n, "type": t} for n, t in types.items()]
            for k in kept:
                by_type[k["type"]] += 1
            lines.extend(cn.entity_lines(cid, kept, source=SOURCE, confidence=CONFIDENCE,
                                         retype_only=True))
    counts.update(answered=len(lines), unanswered=counts["unknown_entities"] - len(lines),
                  model_errors=errors, by_type=by_type, applied=0)
    if counts["batches"] and errors == counts["batches"]:
        print(json.dumps(counts, sort_keys=True))
        print("every batch hit a model error; nothing imported", file=sys.stderr)
        return 3
    if lines:
        path = cn.write_jsonl_private(
            os.path.join(a.work_dir, f"retype-{now.strftime('%Y%m%d-%H%M%S')}.jsonl"), lines)
        applied, code = cn.run_import(a.human_bin, a.graph_db, path)
        if code != 0 or applied is None:
            print(json.dumps(counts, sort_keys=True))
            return refuse("`human memory import-facts` failed")
        counts["applied"] = applied
    print(json.dumps(counts, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Add the test to CI**

In `.github/workflows/ci.yml`, change the `Insight-stream + curator + second-opinion pins` run line to:

```yaml
        run: python3 -m pytest -q tests/test_insight_stream_*.py tests/test_curator_*.py tests/test_second_opinion_*.py tests/test_graph_retype.py
```

- [ ] **Step 4: Run to verify pass**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cd "$W" && python3 -m pytest -q tests/test_graph_retype.py 2>&1 | tail -3
```
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
git -C "$W" add scripts/graph_retype_entities.py tests/test_graph_retype.py .github/workflows/ci.yml
git -C "$W" commit -m "feat(names): one-time retype migration for UNKNOWN graph entities

468 of 572 entities are UNKNOWN. The migration backs graph.db up with the
online backup API (0600) before anything else, refuses on a failed backup
or a locked graph, types names in batches of 40 with the local model, and
applies retype-only lines through the C importer. Nothing is deleted.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: Measurement harness, CI, and operator docs

**Files:**
- Create: `scripts/eval_name_grounding.py`
- Test: `tests/test_eval_name_grounding.py`
- Modify: `.github/workflows/ci.yml:1229` (append `tests/test_eval_name_grounding.py`)
- Create: `docs/guides/named-entities.md`

**Interfaces:**
- Consumes: the `human memory ground --full` output contract (Task 6); `eval_conversation_quality._load_messages` (chat.db 1:1 reader, read-only); `curator_population.is_valid_handle/normalize_handle/normalize_all/load_loopback_handles`.
- Produces: CLI `eval_name_grounding.py [--chat-db] [--graph-db] [--human-bin] [--out-dir] [--n 40] [--days 14] [--per-contact 8] [--fallback live] [--self-facts live]`. Output: `<out-dir>/name-grounding-<YYYYmmdd-HHMMSS>.json` (0600), shaped `{"n", "days", "per_contact_cap", "gates", "modes": {"off"|"live": {"nonempty_blocks", "typed_name_blocks", "distinct_typed_names", "bytes_total"}}, "target_live_typed_name_blocks": 15}`. Functions: `sample_moments(per_contact, now, n, days, per_contact_cap, exclude)`, `typed_names(block)`, `parse_probe(stdout) -> (bytes, block) | None`, `summarize(results)`.

- [ ] **Step 1: Write the failing tests** — `tests/test_eval_name_grounding.py`

```python
"""eval_name_grounding.py (spec 2026-09-29 §4.7): the real composition, off vs live, on a
private copy of graph.db; counts only; refuses with < 40 moments or any failed probe.
Hermetic: synthetic chat rows, temp sqlite, fake `human` binary."""
import datetime as dt
import json
import os
import sqlite3
import stat
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
import eval_name_grounding as eg  # noqa: E402

NOW = dt.datetime(2026, 9, 29, 12, 0, tzinfo=dt.timezone.utc)

FAKE_GROUND = """#!{py}
import os, sys
assert sys.argv[1:4] == ["memory", "ground", "--full"], sys.argv
if os.environ.get("FAKE_FAIL"):
    sys.exit(1)
with open(os.environ["FAKE_LOG"], "a") as f:
    f.write(os.environ["HU_GRAPH_DB"] + "\\n")
if os.environ["HU_GRAPH_NAMES"] == "live" and "salim" in sys.argv[5].lower():
    print("matched=1 bytes=17 fallback=0 self=0 names=1")
    print("- Salim (person)\\n")
else:
    print("matched=0 bytes=0 fallback=0 self=0 names=0")
"""


def msg(t_hours, text, from_me=False, atype=0):
    return {"t": NOW - dt.timedelta(hours=t_hours), "text": text, "from_me": from_me,
            "atype": atype}


def corpus():
    """6 contacts x 8 inbound; the newest 40 are contacts 0-4, half mention salim."""
    out = {}
    for k in range(6):
        h = f"+1555000{k:04d}"
        out[h] = [msg(k * 8 + j, f"did salim call {k}{j}" if j % 2 == 0 else f"hey {k}{j}")
                  for j in range(8)]
        out[h].append(msg(0.5, "from me", from_me=True))
        out[h].append(msg(0.2, "loved", atype=2000))
    out["12345"] = [msg(0.1, "short code spam")]
    return out


def setup(tmp_path, monkeypatch, per_contact):
    g = tmp_path / "graph.db"
    con = sqlite3.connect(g)
    con.execute("CREATE TABLE entities (id INTEGER PRIMARY KEY, name TEXT)")
    con.commit(); con.close()
    fake = tmp_path / "human"
    fake.write_text(FAKE_GROUND.format(py=sys.executable))
    fake.chmod(0o755)
    monkeypatch.setenv("FAKE_LOG", str(tmp_path / "fake.log"))
    monkeypatch.setattr(eg, "load_inbound", lambda *a, **k: per_contact)
    monkeypatch.setattr(eg, "HUMAN_CONFIG", str(tmp_path / "absent-config.json"))
    monkeypatch.setattr(eg, "_now", lambda: NOW)
    return ["--graph-db", str(g), "--human-bin", str(fake), "--out-dir", str(tmp_path / "out"),
            "--chat-db", str(tmp_path / "chat.db")]


def test_sample_moments_caps_filters_and_orders():
    m = eg.sample_moments(corpus(), NOW, n=40, days=14, per_contact_cap=8)
    assert len(m) == 40
    assert all(h != "12345" for h, _ in m)                   # short code
    assert all(t not in ("from me", "loved") for _, t in m)  # outbound + reactions
    assert m[0] == ("+15550000000", "did salim call 00")     # newest first
    big = {"+15559999999": [msg(i, f"x{i}") for i in range(20)]}
    assert len(eg.sample_moments(big, NOW, n=40, days=14, per_contact_cap=8)) == 8
    old = {"+15559999999": [msg(24 * 20, "old")]}
    assert eg.sample_moments(old, NOW, n=40, days=14, per_contact_cap=8) == []


def test_typed_names_and_parse_probe():
    b = "- Salim (person)\n  - Salim knows Bob (person)\n- sailboat (topic)\n- Acme (organization)\n"
    assert eg.typed_names(b) == ["Salim", "Acme"]
    assert eg.parse_probe("matched=1 bytes=17 fallback=0 self=0 names=1\n- Salim (person)\n\n") == (
        17, "- Salim (person)\n\n")
    assert eg.parse_probe("error: nope") is None


def test_end_to_end_counts_only_on_a_private_copy(tmp_path, monkeypatch, capsys):
    argv = setup(tmp_path, monkeypatch, corpus())
    assert eg.main(argv) == 0
    files = list((tmp_path / "out").glob("name-grounding-*.json"))
    assert len(files) == 1 and stat.S_IMODE(os.stat(files[0]).st_mode) == 0o600
    r = json.load(open(files[0]))
    assert r["n"] == 40
    assert r["modes"]["off"] == {"nonempty_blocks": 0, "typed_name_blocks": 0,
                                 "distinct_typed_names": 0, "bytes_total": 0}
    assert r["modes"]["live"] == {"nonempty_blocks": 20, "typed_name_blocks": 20,
                                  "distinct_typed_names": 1, "bytes_total": 340}
    assert r["gates"] == {"HU_GRAPH_GROUNDING_CONTACT_FALLBACK": "live",
                          "HU_GRAPH_GROUNDING_SELF_FACTS": "live"}
    assert "salim" not in json.dumps(r).lower()  # counts only
    probed = set(open(tmp_path / "fake.log").read().split())
    assert len(probed) == 1 and str(tmp_path / "graph.db") not in probed  # a copy, never the source
    assert not os.path.exists(probed.pop())  # and the copy is gone


def test_refuses_with_fewer_than_40_moments(tmp_path, monkeypatch):
    few = {"+15550000001": [msg(i, f"hi {i}") for i in range(5)]}
    argv = setup(tmp_path, monkeypatch, few)
    assert eg.main(argv) == 2
    assert not (tmp_path / "out").exists()


def test_refuses_when_a_probe_fails(tmp_path, monkeypatch):
    argv = setup(tmp_path, monkeypatch, corpus())
    monkeypatch.setenv("FAKE_FAIL", "1")
    assert eg.main(argv) == 2
    assert not (tmp_path / "out").exists()  # nothing written on a partial run


def test_refuses_without_the_binary(tmp_path, monkeypatch):
    argv = setup(tmp_path, monkeypatch, corpus())
    argv[argv.index("--human-bin") + 1] = str(tmp_path / "missing")
    assert eg.main(argv) == 2
```

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cd "$W" && python3 -m pytest -q tests/test_eval_name_grounding.py 2>&1 | tail -3
```
Expected: `ModuleNotFoundError: No module named 'eval_name_grounding'`.

- [ ] **Step 2: Create `scripts/eval_name_grounding.py`**

```python
#!/usr/bin/env python3
"""Name-grounding E2E measurement (spec 2026-09-29 named-entity-extraction §2, §4.7).

For the last 40 inbound 1:1 texts (14 days, <= 8 per contact), run the REAL
composition — `human memory ground --full <contact> <text>`, i.e.
hu_graph_ground_compose_turn — under HU_GRAPH_NAMES=off and =live against a
private 0600 copy of graph.db, and count per mode: non-empty blocks, blocks
naming >= 1 typed entity, distinct typed names, bytes. Counts only. Refuses
(exit 2, writes nothing) with fewer than 40 moments or when any probe fails:
a partial run is not a measurement. Target (spec §2): live >= 15/40.
"""
import argparse
import contextlib
import datetime as dt
import json
import os
import re
import sqlite3
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import curator_population as cp  # noqa: E402

HOME = os.path.expanduser("~")
HUMAN_CONFIG = os.path.join(HOME, ".human/config.json")
REACTIONS = range(2000, 4000)
TYPED_LINE = re.compile(r"^- (.+) \((person|place|organization|event)\)$")
HEADER = re.compile(r"^matched=\d+ bytes=(\d+)")
MODES = ("off", "live")
TARGET = 15


def _now():
    return dt.datetime.now(dt.timezone.utc)


def load_inbound(chat_db, since):
    """chat.db 1:1 rows per handle, read-only — the conversation-quality metric's reader."""
    import eval_conversation_quality as cq
    return cq._load_messages(chat_db, since)


def sample_moments(per_contact, now, n=40, days=14, per_contact_cap=8, exclude=()):
    """[(handle, text)] newest first: inbound, non-reaction, non-empty texts from valid
    1:1 handles within `days`, at most `per_contact_cap` per handle, at most `n`."""
    cutoff = now - dt.timedelta(days=days)
    excluded = cp.normalize_all(exclude)
    pool = []
    for h, msgs in per_contact.items():
        if not cp.is_valid_handle(h) or cp.normalize_handle(h) in excluded:
            continue
        mine = [m for m in msgs if not m["from_me"] and m["t"] >= cutoff
                and m.get("atype", 0) not in REACTIONS and (m.get("text") or "").strip()]
        mine.sort(key=lambda m: m["t"], reverse=True)
        pool.extend((m["t"], h, m["text"].strip()) for m in mine[:per_contact_cap])
    pool.sort(key=lambda x: x[0], reverse=True)
    return [(h, text) for _, h, text in pool[:n]]


def typed_names(block):
    return [m.group(1) for m in (TYPED_LINE.match(ln) for ln in (block or "").splitlines()) if m]


def parse_probe(stdout):
    """`ground --full` stdout -> (bytes from the header, block) or None."""
    head, _, block = (stdout or "").partition("\n")
    m = HEADER.match(head)
    return (int(m.group(1)), block) if m else None


def probe(human_bin, graph_copy, mode, contact, text, gates, timeout=60):
    env = {**os.environ, **gates, "HU_GRAPH_DB": graph_copy, "HU_GRAPH_NAMES": mode}
    try:
        r = subprocess.run([human_bin, "memory", "ground", "--full", contact, text], env=env,
                           capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return parse_probe(r.stdout) if r.returncode == 0 else None


def summarize(results):
    names, typed, nonempty, total = set(), 0, 0, 0
    for nbytes, block in results:
        found = typed_names(block)
        nonempty += 1 if nbytes > 0 else 0
        typed += 1 if found else 0
        names.update(n.lower() for n in found)
        total += nbytes
    return {"nonempty_blocks": nonempty, "typed_name_blocks": typed,
            "distinct_typed_names": len(names), "bytes_total": total}


def private_copy(src):
    """Consistent 0600 copy via the online backup API (includes WAL content)."""
    fd, path = tempfile.mkstemp(prefix="name-grounding-", suffix=".db")
    os.close(fd)
    os.chmod(path, 0o600)
    s = sqlite3.connect(f"file:{src}?mode=ro", uri=True)
    d = sqlite3.connect(path)
    try:
        s.backup(d)
    finally:
        d.close()
        s.close()
    return path


def write_result(out_dir, now, result):
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"name-grounding-{now.strftime('%Y%m%d-%H%M%S')}.json")
    tmp = path + ".tmp"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(result, f, indent=1, sort_keys=True)
    os.replace(tmp, path)
    return path


def refuse(msg):
    print(f"refusing: {msg}; nothing written", file=sys.stderr)
    return 2


def main(argv=None):
    ap = argparse.ArgumentParser(description="Name-grounding E2E measurement (off vs live).")
    ap.add_argument("--chat-db", default=os.path.join(HOME, "Library/Messages/chat.db"))
    ap.add_argument("--graph-db", default=os.path.join(HOME, ".human/graph.db"))
    ap.add_argument("--human-bin", default=os.path.join(HOME, ".local/bin/human-daemon"))
    ap.add_argument("--out-dir", default=os.path.join(HOME, ".human/logs"))
    ap.add_argument("--n", type=int, default=40)
    ap.add_argument("--days", type=int, default=14)
    ap.add_argument("--per-contact", type=int, default=8)
    ap.add_argument("--fallback", choices=["off", "shadow", "live"], default="live",
                    help="HU_GRAPH_GROUNDING_CONTACT_FALLBACK for the probe (prod: live)")
    ap.add_argument("--self-facts", choices=["off", "shadow", "live"], default="live",
                    help="HU_GRAPH_GROUNDING_SELF_FACTS for the probe (prod: live)")
    a = ap.parse_args(argv)
    if not os.access(a.human_bin, os.X_OK):
        return refuse(f"human binary not executable ({a.human_bin})")
    now = _now()
    try:
        per_contact = load_inbound(a.chat_db, now - dt.timedelta(days=a.days))
        loopback = cp.load_loopback_handles(HUMAN_CONFIG)
    except (sqlite3.Error, ValueError, json.JSONDecodeError) as e:
        return refuse(f"cannot read inputs ({e})")
    moments = sample_moments(per_contact, now, a.n, a.days, a.per_contact, loopback)
    if len(moments) < a.n:
        return refuse(f"{len(moments)} moments < {a.n}")
    gates = {"HU_GRAPH_GROUNDING_CONTACT_FALLBACK": a.fallback,
             "HU_GRAPH_GROUNDING_SELF_FACTS": a.self_facts}
    try:
        copy = private_copy(a.graph_db)
    except sqlite3.Error as e:
        return refuse(f"graph.db unreadable ({e})")
    try:
        results = {mode: [] for mode in MODES}
        for contact, text in moments:
            for mode in MODES:
                r = probe(a.human_bin, copy, mode, contact, text, gates)
                if r is None:
                    return refuse(f"a {mode} probe failed")
                results[mode].append(r)
    finally:
        with contextlib.suppress(OSError):
            os.remove(copy)
    result = {"n": len(moments), "days": a.days, "per_contact_cap": a.per_contact,
              "gates": gates, "modes": {m: summarize(results[m]) for m in MODES},
              "target_live_typed_name_blocks": TARGET}
    path = write_result(a.out_dir, now, result)
    print(json.dumps(result["modes"], sort_keys=True))
    print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Add the test to CI**

In `.github/workflows/ci.yml`, append ` tests/test_eval_name_grounding.py` to the same pytest run line edited in Task 8:

```yaml
        run: python3 -m pytest -q tests/test_insight_stream_*.py tests/test_curator_*.py tests/test_second_opinion_*.py tests/test_graph_retype.py tests/test_eval_name_grounding.py
```

- [ ] **Step 4: Write `docs/guides/named-entities.md`**

````markdown
---
title: Named entities — gates, nightly pass, migration and measurement
created: 2026-09-29
status: operator-facing
spec: docs/superpowers/specs/2026-09-29-named-entity-extraction-design.md
---

# Named entities

How contacts' graphs learn the named people, places, orgs and events they talk
about, and how grounding names them. Design:
[spec](../superpowers/specs/2026-09-29-named-entity-extraction-design.md);
build: [plan](../superpowers/plans/2026-09-29-named-entity-extraction.md).

## Gates (daemon plist `EnvironmentVariables`)

| Variable | Values | Default | Effect |
|---|---|---|---|
| `HU_NAME_CATCH` | off / shadow / live | off | Per-turn zero-model catcher on the contact's inbound text. SHADOW logs `name_catch shadow: known=… new=… (not written)`; LIVE bumps known names and records new Capitalized names (`names:turn`, 0.3). |
| `HU_GRAPH_NAMES` | off / shadow / live | off | Grounding seeds only typed names and Capitalized UNKNOWNs (typed +0.5), and topics move to one `Been talking about:` line. SHADOW logs `names shadow: off=… live=… bytes names=…` and injects the OFF block. |

`HU_GRAPH_NAMES=live` changes what is sent as Seth. It needs the harness result below
**and** Seth's go-ahead (blind A/B, or an explicit override).

## Nightly pass

```bash
python3 scripts/insight_stream.py --names --deadline 07:30 --write
```

Lists names for every eligible 1:1 contact with texts in the last `--names-days` (2),
keeps a name only if a cited human text says it, and imports `names:nightly` lines with
`human memory import-facts`. Without `--write` it stops before the import. The manifest
is `~/.human/logs/names-manifest-YYYYMMDD[-dryrun].json` (counts only); the JSONL
(0600) is in `~/.human/names/`.

## One-time migration

```bash
python3 scripts/graph_retype_entities.py --dry-run   # counts only
python3 scripts/graph_retype_entities.py --write     # backup -> classify -> import
```

`--write` backs up to `~/.human/backups/graph.db.bak-retype-<ts>` (0600) first and
refuses if the backup fails or graph.db is locked. Restore: stop the daemon, then
`cp <backup> ~/.human/graph.db`.

## Measurement

```bash
python3 scripts/eval_name_grounding.py
```

Probes `human memory ground --full` for 40 recent inbound texts under
`HU_GRAPH_NAMES=off` and `=live` on a private copy of graph.db, then writes
`~/.human/logs/name-grounding-<ts>.json`. Target: `modes.live.typed_name_blocks >= 15`,
with `off` unchanged from the baseline run.

## Rollout (spec §6)

1. Build-prod, `scripts/install-human-daemon.sh`, then `scripts/verify-deploy.sh <commit>`.
2. Plist: `HU_NAME_CATCH=live`, `HU_GRAPH_NAMES=shadow`.
3. Baseline: `eval_name_grounding.py` before any new write.
4. Migration `--write`, then one manual `insight_stream.py --names --write`.
5. Re-run the harness: `live` must reach 15/40 with `off` unchanged.
6. Append `&& /opt/homebrew/bin/python3 scripts/insight_stream.py --names --deadline 07:30 --write`
   to `ai.human.insight-nightly` (back up the plist first; bootout + bootstrap).
7. `HU_GRAPH_NAMES=live` only with the harness result and Seth's go-ahead.
````

- [ ] **Step 5: Run to verify pass**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
cd "$W" && python3 -m pytest -q tests/test_eval_name_grounding.py tests/test_graph_retype.py tests/test_insight_stream_*.py tests/test_curator_*.py tests/test_second_opinion_*.py 2>&1 | tail -3
cd "$W" && bash scripts/check-docs-frontmatter.sh && bash scripts/check-terminology.sh
cd "$W" && MARKDOWN_LINK_SCAN_ALL=1 bash scripts/check-docs-relative-links.sh 2>&1 | tail -3
```
Expected: all tests pass, and the frontmatter, terminology and link checks are clean.

- [ ] **Step 6: Commit**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/named-entities
git -C "$W" add scripts/eval_name_grounding.py tests/test_eval_name_grounding.py .github/workflows/ci.yml docs/guides/named-entities.md
git -C "$W" commit -m "feat(names): name-grounding E2E harness + operator guide

The 2026-09-27 0/40 recall number came from a lexical-only probe. The
harness runs the live composition (ground --full) under HU_GRAPH_NAMES
off and live on a private graph copy, counts typed-name blocks against the
15/40 target, and refuses rather than report a partial run.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## After this plan (operator, after merge — not in the repo)

Rollout per spec §6 and `docs/guides/named-entities.md`: deploy with
`scripts/install-human-daemon.sh` and `scripts/verify-deploy.sh`; set `HU_NAME_CATCH=live` and
`HU_GRAPH_NAMES=shadow` in the plist; run the harness baseline, then the migration,
then one manual `--names --write`, then re-run the harness; append `--names` to the
nightly chain. Flipping `HU_GRAPH_NAMES=live` needs the harness result **and** Seth's
go-ahead.

Out of scope (spec §8): re-enabling the per-turn LLM deep-extract, `backfill_facts.py`'s
`self` attribution, alias/merge ("Sal" vs "Salim"; Review Focus 3 only keeps a lowercase
duplicate out of LIVE rendering), and the curator's `HU_INSIGHT_GRAPH_MIRROR` step C.
