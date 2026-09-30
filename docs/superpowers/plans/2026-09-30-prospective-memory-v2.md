---
title: Prospective memory v2 — implementation plan
date: 2026-09-30
status: draft (awaiting review)
spec: docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md
---

# Prospective Memory v2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One typed intention store in `prospective_memories`, with a PIS-style fire-time check (Filter in code → Decide by the local model). Keyword cues and time cues fire at the right moment and never for a resolved intention. Everything ships OFF → SHADOW → LIVE behind `HU_PROSPECTIVE` and `HU_PROSPECTIVE_TIME`, and with both gates off the daemon is byte-identical to today.

**Architecture:** Five layers, each one a small file.
1. `prospective_repo_sqlite.c` migrates the table additively and owns every new SQL statement.
2. `prospective_policy.c` holds the pure decisions: Filter, verdict parse, Decide, done-after-evidence, and rendering.
3. `prospective_v2.c` runs Filter → Decide → transitions against an injected judge.
4. `daemon_prospective.c` (reactive, `HU_PROSPECTIVE`) and `daemon_prospective_time.c` (proactive tick, `HU_PROSPECTIVE_TIME`) are the gate dispatchers. The judge is a thinking-off one-shot call through the agent's provider (GLM on :8741 in prod).
5. `human prospective probe|init|backfill` exposes the same functions. `scripts/pm_bench_local.py`, `prospective_backfill.py`, `prospective_shadow_report.py` and `prospective_spot_check.py` drive it and measure it.

**Tech Stack:** C11 (`-Wall -Wextra -Wpedantic -Werror`, `tests/test_framework.h`), SQLite through `src/memory/repos/` only, Python 3 stdlib + pytest (hermetic), local GLM through the configured provider.

**Spec:** `docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md` (binding). Executors read both.

## Global Constraints

- **Worktree.** Implement in `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2`. Create it once, before Task 1:
  `git -C /Users/sethford/Projects/h-uman worktree add .claude/worktrees/prospective-v2 -b feat/prospective-v2 docs/prospective-v2-spec`.
  Then build it:
  `cd "$W" && cmake --preset dev && cmake --build "$W/build" --target human human_tests -j8`.
  Every command block below starts with `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2`, because `cd` does not persist between tool calls. Commit with `git -C "$W"`.
- **Never** touch `~/.human`, `~/Library/Messages/chat.db`, ports 8741/8743, launchd plists, or the running daemon. Tests never do either: C tests use `:memory:` SQLite, and Python tests use `tmp_path`, a fake `human` binary and a monkeypatched `HOME`.
- **Gates.** `HU_PROSPECTIVE=off|shadow|live` controls the reactive keyword path. `HU_PROSPECTIVE_TIME=off|shadow|live` controls time cues that initiate a message. Both are parsed with `hu_gate_mode_from_env` and default **OFF**.
  - `off` is today's behavior, byte-identical: fire on match, mark `fired=1`. Pinned by `directive_build_legacy_bytes_are_pinned` (Task 2) and `directive_off_is_byte_identical_to_legacy_and_never_judges` (Task 7).
  - `shadow` keeps today's output and also runs Filter + Decide read-only. It logs `prospective shadow: candidates=%zu fire=%zu resolved=%zu cancel=%zu not_now=%zu parse_fail=%zu judge_err=%zu expired=%zu capped=%zu`, plus one `prospective shadow item: id=%lld verdict=%s` line per judged item.
  - Each gate logs one banner line per process (`hu_prospective_gate_banner`). When off, the banner names the env key and the values that enable it.
- **Status mapping (spec §4.1).** `fired` 0→`pending`, 1→`done`, 2→`canceled`, 3→`expired`. `surfaced` writes `fired=0`. A trigger derives `status` whenever a legacy writer changes `fired`. v2 writers set both columns in one `UPDATE`.
- **Units.** Every time value, including the new `due_at` and `surfaced_at`, is unix **seconds**, the unit of every existing column in `prospective_memories`, `commitments` and `delayed_followups` (see "Spec gaps" #1).
- **Constants** live in `include/human/memory/prospective_policy.h`:
  - `HU_PROSPECTIVE_RENDER_CAP 3`
  - `HU_PROSPECTIVE_JUDGE_CAP 3` (Decide calls per turn)
  - `HU_PROSPECTIVE_MAX_ATTEMPTS 2`
  - `HU_PROSPECTIVE_TIME_GRACE_S (3 * 86400)`
  - `HU_PROSPECTIVE_BACKFILL_EXPIRE_S (14 * 86400)`
  - `HU_PROSPECTIVE_HISTORY_TURNS 20`
- **Decide** makes one call per candidate with temperature 0, `max_tokens` 16 and `thinking_budget` 0, through `hu_provider_chat_oneshot`.
  - A model error or parse failure keeps the item pending and is counted (`judge_err` / `parse_fail`). The system fails toward silence.
  - LIVE directive text: `[PROSPECTIVE MEMORY: If it fits naturally, you could bring up: <action> | …]`.
- **Done only after evidence.** Rendering marks the intention `surfaced` with `surfaced_at`. It becomes `done` only when the delivered reply contains at least half of the action's key terms. Otherwise it returns to `pending` with `attempts+1`, and after 2 attempts it is `expired`. An intention surfaced but never confirmed by a delivered reply is settled the same way at the contact's next pass.
- **Eligibility.** Group chats and self-chat are never eligible. Self-chat means `hu_share_is_owner`, the persona contacts with relationship `"test"`.
- **Rules in force:**
  - **sqlite-includer ratchet.** No new `#include <sqlite3.h>` under `src/` outside `src/memory/repos/` and `src/memory/engines/`. New SQL lives in `src/memory/repos/prospective_repo_sqlite.c`. Other files may use the `sqlite3 *` type through headers.
  - **File-size ceiling.** `src/daemon.c` and `src/agent/agent_turn.c` are both at the 10,420-line ceiling. Task 5 moves ~50 lines out of `daemon.c` before Tasks 7 and 9 add three. No task touches `agent_turn.c`. Check with `wc -l "$W/src/daemon.c"` (must stay ≤ 10420).
  - **Dead-strip ratchet.** Every new `.c` has a product caller in the same commit: repo←engine, policy←`prospective.c`, oneshot←`init_proposer.c`, time module←`daemon.c`, v2←`cli_prospective.c`, `daemon_prospective.c`←reactive prompt + message router. Every new `hu_` symbol is reachable or referenced by a test.
  - **Clone ratchet.** Run `bash "$W/scripts/check-clone-ratchet.sh"` before each C commit. If it grows, restructure (a shared static helper, different shape); never copy-paste.
  - **Test/source gate symmetry.** New test files are registered unconditionally and wrap SQLite bodies in `#ifdef HU_ENABLE_SQLITE` with a `#else` stub runner. Pure tests stay outside the `#ifdef`.
  - **Test references production symbol.** `tests/test_<name>.c` calls `hu_*` symbols from `src/**/<name>.c`.
  - **Agent-core boundary.** No `src/agent/` file includes `human/providers/factory.h`. Only `src/app/cli_prospective.c` does.
  - **State paths.** C code builds no `~/.human` path. The probe requires `--db`.
  - **Security-predicate extraction.** Filter, Decide and the after-delivery rule are pure predicates in `prospective_policy.c`, each with a truth-table test.
- **Build and test.**
  - Build: `cmake --build "$W/build" --target human human_tests -j8`.
  - Run tests from the worktree root, because some tests read `src/…` by relative path: `cd "$W" && ./build/human_tests --suite=prospective`.
  - Before every commit, run the full suite: `cd "$W" && ./build/human_tests 2>&1 | grep -E 'Results:'`. It must show 0 failed, with no ASan report.
  - Python: `cd "$W" && python3 -m pytest -q tests/test_<name>.py`.
- **Commits.** Conventional (`feat(prospective): …`, `refactor(daemon): …`, `test(prospective): …`), each ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Implementers on other models substitute their own trailer. Never `--no-verify`.
- **Python outputs** are counts and rates only: no action, cue, contact or message text. They are written 0600 under `~/.human/logs/` (the spot-check sheet goes to its own 0700 directory). A script that cannot measure exits non-zero and writes nothing (`no-number-without-a-measurement`).

## Review Focus

1. **The judge answers in Seth's voice** ("lol yeah def bring it up"). The serving model carries the persona adapter, so a one-word classifier answer is not guaranteed. Expected: `PARSE_FAIL`, counted, the item stays pending, nothing is surfaced, and a harness run whose judge mostly failed is refused rather than scored. Pinned in Task 2 by `parse_verdict_truth_table` (chatty cases) and in Task 11 by `test_judge_failures_make_the_run_inconclusive`.
2. **One message cues many intentions** ("alpha beta gamma delta epsilon", five open intentions). Expected: at most 3 Decide calls (bounded latency on the reply path, SHADOW included), at most 3 rendered, and the rest untouched and still pending. Pinned in Task 6 by `v2_judges_at_most_three_intentions_per_turn`.
3. **The reply never goes out as text** (voice memo, tapback, skipped turn). The after-delivery hook never sees it. Expected: the intention is not stuck in `surfaced`. The next pass counts it as a failed attempt, and after 2 it is `expired`. Pinned in Task 6 by `v2_undelivered_surfacing_is_reclaimed_as_an_attempt`.
4. **A cue arrives in a group chat or the owner's self-chat.** Expected: never judged (no model call), never surfaced, no row written. Pinned in Task 6 by `v2_group_and_self_chat_are_never_judged`.
5. **One promise, two ledger rows.** The promise keeper stores a dated commitment *and* schedules its delayed follow-up. Expected: one time intention, one reminder. When it is done, both ledger rows are retired so the legacy readers never resurface it. Pinned in Task 8 by `superhuman_dated_commitment_and_its_followup_mirror_once`, in Task 10 by `v2_backfill_imports_expires_reanchors_and_dedupes`, and in Task 6 by `v2_time_done_retires_ledger_twins`.

## Spec gaps and conflicts resolved in this plan

1. **Units.** §4.1 says `due_at`/`surfaced_at` are ms. Every existing column in the three tables is seconds: `created_at`/`expires_at` are written as `now_ms // 1000` by the curator, and `deadline`/`scheduled_at` come from `time(NULL)`. The plan uses **seconds** throughout, so no conversion is needed at any join.
2. **`surfaced` has no legacy `fired` value.** It writes `fired=0`. The curator (Python) only writes `fired`, so a trigger derives `status` from `fired` on every legacy update. The migration maps existing rows once.
3. **14-day backfill rule vs 3-day grace.** An item 3–14 days overdue would be imported `pending` and then expired by the Filter on first sight, which makes the spec's 14-day line meaningless. Resolution: items overdue ≤ 14 days are imported `pending` with `due_at` **re-anchored to the backfill time**, so they get one grace window. Items overdue more than 14 days are imported `expired`. The manifest reports `reanchored`. *Owner may veto:* dropping re-anchoring is a one-line change in `pm_backfill_one`.
4. **Double mirroring.** Both commitment writers (`daemon_promise_keeper.c:205/215`, `daemon.c:4096/4106`) store a commitment **and** a delayed follow-up for the same promise. Mirroring both would make two reminders. `hu_prospective_repo_upsert_time` treats a row with the same source key **or** the same contact + action + `due_at` as already present.
5. **`after_event`** has no v1 source and no Filter rule in §4.2. It is accepted as a column value and is never eligible.
6. **"The action's key terms"** is undefined. It is defined here as content words of ≥ 4 characters, minus function words and generic intention verbs ("ask", "remember", "check", "send"…). "Used" means at least half of them appear as whole words, with plural-tolerant matching. An action with no key terms can never be proven used, so it expires after 2 attempts, which is silent.
7. **"Last 20 turns" on the time path.** The proactive tick has no conversation loaded. The time module loads 20 entries through the channel's `load_conversation_history` for the send target.
8. **Where time cues are evaluated.** Moving the proactive loop's `should_checkin` gate is out of scope. v2 time cues are considered where the legacy producers ran: on ticks where the contact passed the check-in gate. The shadow report measures what that misses (`time_misses`).
9. **SHADOW calls the model.** §4.4 requires Filter + Decide to run in shadow. That is up to 3 short calls on a *cued* turn before the reply. They are bounded by `HU_PROSPECTIVE_JUDGE_CAP` and `max_tokens=16`, and apply only when a cue matched.
10. **SHADOW metrics (§3) have no reader, and the rating-sheet flow is A/B-preference only.** `prospective_shadow_report.py` (Task 12) computes the seven §3 numbers from the dated log lines plus a read-only DB. `prospective_spot_check.py` (Task 13) builds a blind yes/no sheet that mixes would-fires with held items, and scores precision on ≥ 30 would-fires.
11. **Curator changes.** The curator's extraction prompt has no date field, so "write cue_kind/due_at when a date is present" is a no-op today. The new columns' defaults (`cue_kind='keyword'`, `status='pending'`, `source='extractor'`) cover every curator insert. `insight_stream.py` is unchanged.
12. **Probe safety.** `human prospective probe` requires `--db`. It never opens `~/.human/memory.db` by default.
13. **The shadow log line.** The spec's fields come first, verbatim. `parse_fail`, `judge_err`, `expired` and `capped` are appended, because §4.3 says parse failures are "counted in the log".
14. **Group detection.** `hu_reactive_turn_ctx_t` has no group flag. The plan adds `rt.is_group` (one line in `daemon.c`).
15. **No public thinking-off helper existed.** `init_proposer_call_llm` was `static`. Task 4 extracts it to `hu_provider_chat_oneshot` unchanged, and Decide reuses it.
16. **Judge routing.** The judge uses the agent's own provider, which prod configures with a Gemini reliability fallback. When GLM is down, a Decide call can go to the same cloud fallback the reply itself would use. It carries no more context than that reply.

## File map

| File | Responsibility | Task |
|---|---|---|
| `include/human/memory/prospective_repo.h`, `src/memory/repos/prospective_repo_sqlite.c` | migration, typed reads/writes, ledger sync | 1, 3 |
| `src/memory/engines/sqlite.c` | calls the migration at open | 1 |
| `include/human/memory/prospective_policy.h`, `src/memory/prospective_policy.c` | pure Filter / Decide / evidence / render / judge prompt / gates | 2 |
| `src/memory/prospective.c` | legacy render moved onto `hu_prospective_render` (byte-identical) | 2 |
| `include/human/providers/chat_oneshot.h`, `src/providers/chat_oneshot.c` | thinking-off one-shot call | 4 |
| `src/agent/init_proposer.c` | uses the one-shot helper | 4 |
| `include/human/daemon/prospective_time.h`, `src/daemon/daemon_prospective_time.c` | proactive time producers (moved), then the time gate | 5, 9 |
| `include/human/memory/prospective_v2.h`, `src/memory/prospective_v2.c` | Filter → Decide → transitions; after-delivery; backfill | 6, 10 |
| `include/human/cli_prospective.h`, `src/app/cli_prospective.c` | `human prospective init/probe/backfill` | 6, 7, 10 |
| `include/human/daemon/prospective.h`, `src/daemon/daemon_prospective.c` | reactive gate, judge adapter, history, delivery hook | 7 |
| `src/daemon/daemon_reactive_prompt.c`, `include/human/daemon/reactive_turn.h`, `src/daemon/daemon_message_router.c`, `src/daemon.c` | call sites | 5, 7, 9 |
| `src/memory/superhuman.c` | writers mirror dated items as time rows | 8 |
| `scripts/prospective_backfill.py` | backup + backfill + manifest | 10 |
| `scripts/pm_bench_local.py` | scripted multi-day harness | 11 |
| `scripts/prospective_shadow_report.py` | the seven §3 SHADOW numbers | 12 |
| `scripts/prospective_spot_check.py` | blind yes/no precision sheet | 13 |
| `docs/guides/prospective-memory.md`, `.github/workflows/ci.yml` | operator guide, CI step | 14 |

---

### Task 1: Schema migration and status mapping

**Files:**
- Create: `include/human/memory/prospective_repo.h`
- Create: `src/memory/repos/prospective_repo_sqlite.c`
- Modify: `src/memory/engines/sqlite.c` (include block ~:18-23; after the "P4-1 / P4-2 …" `ALTER` block that ends ~:1783, before `hu_sqlite_memory_t *self = …`)
- Modify: `CMakeLists.txt` (SQLite block: after `src/memory/repos/contact_optout_repo_sqlite.c` ~:1670; test list: after `tests/test_prospective.c` ~:4240)
- Modify: `tests/test_main.c` (declaration after `void run_prospective_tests(void);` ~:808; call after `run_prospective_tests();` ~:1849)
- Test: `tests/test_prospective_repo_sqlite.c`

**Interfaces:**
- Consumes: `hu_repo_exec_ddl(sqlite3 *, const char *)` (`include/human/memory/repo_util.h`); `hu_sqlite_memory_create`, `hu_sqlite_memory_get_db` (`include/human/memory.h`).
- Produces: `hu_error_t hu_prospective_repo_ensure_schema(sqlite3 *db);`. It returns `HU_ERR_INVALID_ARGUMENT` for NULL, `HU_ERR_NOT_FOUND` when `prospective_memories` does not exist, `HU_ERR_MEMORY_STORE` when DDL fails, and HU_OK otherwise. After it runs, the table has the columns `cue_kind TEXT NOT NULL DEFAULT 'keyword'`, `due_at INTEGER`, `status TEXT NOT NULL DEFAULT 'pending'`, `surfaced_at INTEGER`, `attempts INTEGER NOT NULL DEFAULT 0`, `outcome TEXT` and `source TEXT NOT NULL DEFAULT 'extractor'`, the index `idx_prospective_status(contact_id, cue_kind, status)` and the trigger `trg_prospective_fired_status`.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_prospective_repo_sqlite.c`:

```c
/* tests/test_prospective_repo_sqlite.c
 *
 * Prospective memory v2 typed store (spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.1).
 * Task 1 pins the additive migration: v2 columns on a pre-v2 table, the
 * fired -> status mapping for existing rows, the trigger that keeps status in
 * step when a legacy writer (the nightly curator) changes fired, idempotence,
 * and that every engine-opened database is migrated. */
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/prospective_repo.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

/* The table exactly as src/memory/engines/sqlite.c created it before v2. */
static sqlite3 *legacy_db(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "CREATE TABLE prospective_memories(id INTEGER PRIMARY KEY "
                              "AUTOINCREMENT,trigger_type TEXT NOT NULL,trigger_value TEXT NOT "
                              "NULL,action TEXT NOT NULL,contact_id TEXT,expires_at INTEGER,fired "
                              "INTEGER DEFAULT 0,created_at INTEGER NOT NULL)",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    return db;
}

static void legacy_insert(sqlite3 *db, const char *action, int fired) {
    char sql[256];
    snprintf(sql, sizeof(sql),
             "INSERT INTO prospective_memories(trigger_type,trigger_value,action,contact_id,"
             "expires_at,fired,created_at) VALUES('keyword','kw','%s','+15550000001',0,%d,100)",
             action, fired);
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
}

static int64_t q_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    int64_t v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static void q_text(sqlite3 *db, const char *sql, char *buf, size_t cap) {
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    const unsigned char *t = sqlite3_column_text(st, 0);
    snprintf(buf, cap, "%s", t ? (const char *)t : "(null)");
    sqlite3_finalize(st);
}

static int64_t column_count(sqlite3 *db) {
    return q_int(db, "SELECT COUNT(*) FROM pragma_table_info('prospective_memories')");
}

static void ensure_schema_adds_typed_columns_and_maps_fired(void) {
    sqlite3 *db = legacy_db();
    legacy_insert(db, "open", 0);
    legacy_insert(db, "fired", 1);
    legacy_insert(db, "pruned", 2);
    legacy_insert(db, "expired", 3);
    HU_ASSERT_EQ(column_count(db), (int64_t)8);

    HU_ASSERT_EQ(hu_prospective_repo_ensure_schema(db), HU_OK);

    HU_ASSERT_EQ(column_count(db), (int64_t)15);
    static const char *const cols[] = {"cue_kind",    "due_at",   "status", "surfaced_at",
                                       "attempts",    "outcome",  "source"};
    for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
        char sql[160];
        snprintf(sql, sizeof(sql),
                 "SELECT COUNT(*) FROM pragma_table_info('prospective_memories') WHERE name='%s'",
                 cols[i]);
        HU_ASSERT_EQ(q_int(db, sql), (int64_t)1);
    }
    char s[32];
    q_text(db, "SELECT status FROM prospective_memories WHERE action='open'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending");
    q_text(db, "SELECT status FROM prospective_memories WHERE action='fired'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "done");
    q_text(db, "SELECT status FROM prospective_memories WHERE action='pruned'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "canceled");
    q_text(db, "SELECT status FROM prospective_memories WHERE action='expired'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "expired");
    /* existing rows get the defaults for the other columns */
    q_text(db, "SELECT cue_kind || '/' || source FROM prospective_memories WHERE action='open'",
           s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "keyword/extractor");
    HU_ASSERT_EQ(q_int(db, "SELECT attempts FROM prospective_memories WHERE action='open'"),
                 (int64_t)0);
    HU_ASSERT_EQ(
        q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE due_at IS NOT NULL"),
        (int64_t)0);
    sqlite3_close(db);
}

/* The curator (scripts/insight_stream.py) writes only the legacy columns:
 * its inserts must land as pending keyword rows, and its fired updates must
 * move status with them. */
static void legacy_writers_keep_status_in_step(void) {
    sqlite3 *db = legacy_db();
    HU_ASSERT_EQ(hu_prospective_repo_ensure_schema(db), HU_OK);
    legacy_insert(db, "curator row", 0);
    char s[32];
    q_text(db, "SELECT status || '/' || cue_kind FROM prospective_memories", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending/keyword");

    HU_ASSERT_EQ(sqlite3_exec(db, "UPDATE prospective_memories SET fired=2", NULL, NULL, NULL),
                 SQLITE_OK);
    q_text(db, "SELECT status FROM prospective_memories", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "canceled");
    HU_ASSERT_EQ(sqlite3_exec(db, "UPDATE prospective_memories SET fired=3", NULL, NULL, NULL),
                 SQLITE_OK);
    q_text(db, "SELECT status FROM prospective_memories", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "expired");
    HU_ASSERT_EQ(sqlite3_exec(db, "UPDATE prospective_memories SET fired=1", NULL, NULL, NULL),
                 SQLITE_OK);
    q_text(db, "SELECT status FROM prospective_memories", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "done");
    /* a v2 write of status alone (fired unchanged) is not overridden */
    HU_ASSERT_EQ(sqlite3_exec(db, "UPDATE prospective_memories SET fired=0, status='pending'",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_exec(db, "UPDATE prospective_memories SET status='surfaced'", NULL,
                              NULL, NULL),
                 SQLITE_OK);
    q_text(db, "SELECT status FROM prospective_memories", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "surfaced");
    sqlite3_close(db);
}

static void ensure_schema_is_idempotent(void) {
    sqlite3 *db = legacy_db();
    HU_ASSERT_EQ(hu_prospective_repo_ensure_schema(db), HU_OK);
    HU_ASSERT_EQ(hu_prospective_repo_ensure_schema(db), HU_OK);
    HU_ASSERT_EQ(column_count(db), (int64_t)15);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND "
                           "name='trg_prospective_fired_status'"),
                 (int64_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND "
                           "name='idx_prospective_status'"),
                 (int64_t)1);
    sqlite3_close(db);
}

static void ensure_schema_refuses_a_missing_table_and_null(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_prospective_repo_ensure_schema(db), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(hu_prospective_repo_ensure_schema(NULL), HU_ERR_INVALID_ARGUMENT);
    sqlite3_close(db);
}

/* The daemon never calls ensure_schema itself: the engine does at open. */
static void engine_open_migrates_prospective_memories(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_EQ(column_count(db), (int64_t)15);
    mem.vtable->deinit(mem.ctx);
}

void run_prospective_repo_sqlite_tests(void) {
    HU_TEST_SUITE("prospective repo");
    HU_RUN_TEST(ensure_schema_adds_typed_columns_and_maps_fired);
    HU_RUN_TEST(legacy_writers_keep_status_in_step);
    HU_RUN_TEST(ensure_schema_is_idempotent);
    HU_RUN_TEST(ensure_schema_refuses_a_missing_table_and_null);
    HU_RUN_TEST(engine_open_migrates_prospective_memories);
}

#else

void run_prospective_repo_sqlite_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
```

Register it. In `CMakeLists.txt`, in the test list directly after `    tests/test_prospective.c`, add:

```cmake
    tests/test_prospective_repo_sqlite.c
```

In `tests/test_main.c`, after `void run_prospective_tests(void);`:

```c
void run_prospective_repo_sqlite_tests(void);
```

and after `    run_prospective_tests();`:

```c
    run_prospective_repo_sqlite_tests();
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human_tests -j8 2>&1 | tail -5`
Expected: FAIL with `'human/memory/prospective_repo.h' file not found`.

- [ ] **Step 3: Write the repository header and the migration**

Create `include/human/memory/prospective_repo.h`:

```c
#ifndef HU_MEMORY_PROSPECTIVE_REPO_H
#define HU_MEMORY_PROSPECTIVE_REPO_H
/*
 * Prospective memory v2 — the typed intention store (spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.1).
 *
 * prospective_memories gains typed columns, additively: cue_kind, due_at,
 * status, surfaced_at, attempts, outcome, source. The legacy `fired` column
 * stays authoritative for every pre-v2 reader (the curator, the nightly eval,
 * the HU_PROSPECTIVE=off path); `status` is derived from it — 0 pending,
 * 1 done, 2 canceled, 3 expired — by a trigger whenever a legacy writer
 * changes `fired`, and v2 writers set both columns in one UPDATE.
 * `surfaced` writes fired=0. Every time value is unix SECONDS, like every
 * other column of these tables.
 *
 * Free functions over a borrowed `sqlite3 *db` from hu_sqlite_memory_get_db();
 * domain callers never include sqlite3.h (sqlite-includer ratchet).
 */
#include "human/core/error.h"

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

/* Idempotent migration of an existing prospective_memories table: adds the
 * missing v2 columns, maps fired -> status on rows still 'pending', creates
 * idx_prospective_status and the fired -> status trigger. HU_ERR_NOT_FOUND
 * when the table does not exist (the sqlite engine creates it first). */
hu_error_t hu_prospective_repo_ensure_schema(sqlite3 *db);

#endif /* HU_ENABLE_SQLITE */
#endif /* HU_MEMORY_PROSPECTIVE_REPO_H */
```

Create `src/memory/repos/prospective_repo_sqlite.c`:

```c
/*
 * src/memory/repos/prospective_repo_sqlite.c
 *
 * SQLite-backed typed intention store (prospective memory v2). Contract in
 * include/human/memory/prospective_repo.h; spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.1.
 */
#include "human/memory/prospective_repo.h"

#ifdef HU_ENABLE_SQLITE

#include "human/memory/repo_util.h"
#include <stdbool.h>
#include <string.h>

static bool pm_has_column(sqlite3 *db, const char *col) {
    sqlite3_stmt *st = NULL;
    bool found = false;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(prospective_memories)", -1, &st, NULL) !=
        SQLITE_OK)
        return false;
    while (!found && sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 1);
        found = name && strcmp(name, col) == 0;
    }
    sqlite3_finalize(st);
    return found;
}

static const struct {
    const char *name;
    const char *ddl;
} k_pm_columns[] = {
    {"cue_kind",
     "ALTER TABLE prospective_memories ADD COLUMN cue_kind TEXT NOT NULL DEFAULT 'keyword'"},
    {"due_at", "ALTER TABLE prospective_memories ADD COLUMN due_at INTEGER"},
    {"status",
     "ALTER TABLE prospective_memories ADD COLUMN status TEXT NOT NULL DEFAULT 'pending'"},
    {"surfaced_at", "ALTER TABLE prospective_memories ADD COLUMN surfaced_at INTEGER"},
    {"attempts",
     "ALTER TABLE prospective_memories ADD COLUMN attempts INTEGER NOT NULL DEFAULT 0"},
    {"outcome", "ALTER TABLE prospective_memories ADD COLUMN outcome TEXT"},
    {"source",
     "ALTER TABLE prospective_memories ADD COLUMN source TEXT NOT NULL DEFAULT 'extractor'"},
};

/* fired -> status for rows still at the default, the status index, and the
 * trigger that keeps status in step with any later legacy write of fired.
 * All idempotent, so it runs on every open (cheap: one indexed UPDATE). */
static const char k_pm_derived[] =
    "UPDATE prospective_memories SET status = CASE fired WHEN 1 THEN 'done' "
    "WHEN 2 THEN 'canceled' WHEN 3 THEN 'expired' ELSE status END "
    "WHERE status = 'pending' AND fired IN (1, 2, 3);"
    "CREATE INDEX IF NOT EXISTS idx_prospective_status ON "
    "prospective_memories(contact_id, cue_kind, status);"
    "CREATE TRIGGER IF NOT EXISTS trg_prospective_fired_status "
    "AFTER UPDATE OF fired ON prospective_memories WHEN NEW.fired IS NOT OLD.fired "
    "BEGIN UPDATE prospective_memories SET status = CASE NEW.fired WHEN 1 THEN 'done' "
    "WHEN 2 THEN 'canceled' WHEN 3 THEN 'expired' ELSE 'pending' END WHERE id = NEW.id; END;";

hu_error_t hu_prospective_repo_ensure_schema(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (!pm_has_column(db, "action"))
        return HU_ERR_NOT_FOUND;
    for (size_t i = 0; i < sizeof(k_pm_columns) / sizeof(k_pm_columns[0]); i++) {
        if (pm_has_column(db, k_pm_columns[i].name))
            continue;
        hu_error_t e = hu_repo_exec_ddl(db, k_pm_columns[i].ddl);
        if (e != HU_OK)
            return e;
    }
    return hu_repo_exec_ddl(db, k_pm_derived);
}

#endif /* HU_ENABLE_SQLITE */
```

Wire it into the engine. In `src/memory/engines/sqlite.c`, add to the `#include "human/memory/…"` group (~:18-23):

```c
#include "human/memory/prospective_repo.h"
```

and directly after the closing `}` of the "2026-05-16 P4-1 / P4-2 / P4-3 / P4-5" `ALTER` block (the one ending with `avoidance_patterns ADD COLUMN last_surfaced`), before `hu_sqlite_memory_t *self =`:

```c
    /* Prospective memory v2 (docs/superpowers/specs/2026-09-30-prospective-
     * memory-v2-design.md §4.1): additive typed columns + fired -> status. */
    if (hu_prospective_repo_ensure_schema(db) != HU_OK)
        hu_log_warn("memory.sqlite", NULL, "prospective_memories v2 migration failed");
```

In `CMakeLists.txt`, in the `if(HU_ENABLE_SQLITE)` source block, after `        src/memory/repos/contact_optout_repo_sqlite.c`:

```cmake
        src/memory/repos/prospective_repo_sqlite.c
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human human_tests -j8 && cd "$W" && ./build/human_tests --suite=prospective`
Expected: PASS. The `prospective repo` suite passes 5/5, and the existing `prospective memory triggers` suite still passes 8/8.

- [ ] **Step 5: Full suite, ratchets, commit**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cd "$W" && ./build/human_tests 2>&1 | grep -E 'Results:' && bash scripts/check-sqlite-includer-ratchet.sh && bash scripts/check-clone-ratchet.sh`
Expected: 0 failed, and both ratchets at or under their ceilings. The repo file is exempt from the includer count.

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2
git -C "$W" add include/human/memory/prospective_repo.h src/memory/repos/prospective_repo_sqlite.c \
  src/memory/engines/sqlite.c CMakeLists.txt tests/test_main.c tests/test_prospective_repo_sqlite.c
git -C "$W" commit -m "feat(prospective): additive typed columns and fired->status mapping

prospective_memories gains cue_kind, due_at, status, surfaced_at, attempts,
outcome and source (spec 2026-09-30 §4.1). fired stays authoritative for
the curator and the nightly eval; a trigger derives status from it, and the
engine migrates every database at open.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Pure policy — Filter, Decide, evidence, rendering, gates

**Files:**
- Create: `include/human/memory/prospective_policy.h`
- Create: `src/memory/prospective_policy.c`
- Modify: `src/memory/prospective.c` (`#define PROSPECTIVE_RENDER_CAP 3` and the render loop in `hu_prospective_directive_build`, ~:185-233)
- Modify: `CMakeLists.txt` (unconditional core list: after `    src/memory/superhuman.c` ~:976; test list after `tests/test_prospective_repo_sqlite.c`)
- Modify: `tests/test_main.c`
- Test: `tests/test_prospective_policy.c` (new), `tests/test_prospective.c` (one golden test)

**Interfaces:**
- Consumes: `hu_gate_mode_from_env` (`human/core/gate_mode.h`), `hu_str_contains_word_ci_n`, `hu_buf_appendf` (`human/core/string.h`).
- Produces: everything declared in `include/human/memory/prospective_policy.h` below. Later tasks use these exact names:
  - enums: `hu_prospective_cue_kind_t`, `hu_prospective_status_t`, `hu_prospective_outcome_t`, `hu_prospective_source_t`, `hu_prospective_filter_t`, `hu_prospective_verdict_t`, `hu_prospective_action_t`, `hu_prospective_render_style_t`
  - struct: `hu_prospective_filter_facts_t`
  - functions: `hu_prospective_filter`, `hu_prospective_parse_verdict`, `hu_prospective_verdict_str`, `hu_prospective_decide`, `hu_prospective_key_terms`, `hu_prospective_reply_uses_action`, `hu_prospective_after_delivery_status`, `hu_prospective_render`, `hu_prospective_judge_system`, `hu_prospective_judge_user`, `hu_prospective_local_day_start`, `hu_prospective_gate_mode`, `hu_prospective_time_gate_mode`, `hu_prospective_gate_banner`, plus the `*_str`/`*_parse` helpers and `hu_prospective_status_to_fired`.

- [ ] **Step 1: Pin the legacy directive bytes first (passes on today's code)**

Append to `tests/test_prospective.c`, before `void run_prospective_tests(void)`:

```c
/* Byte-for-byte pin of the pre-v2 directive: HU_PROSPECTIVE=off must stay
 * identical (spec 2026-09-30 §4.4). Four intentions cued at once, newest
 * first; the fourth is over the render cap and stays open. */
static void directive_build_legacy_bytes_are_pinned(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    int64_t now = (int64_t)time(NULL);
    seed_at(db, "keyword", "alpha", "ask about alpha", "+15550000001", 0, 0, now - 10);
    seed_at(db, "keyword", "beta", "ask about beta", "+15550000001", 0, 0, now - 20);
    seed_at(db, "keyword", "gamma", "ask about gamma", "+15550000001", 0, 0, now - 30);
    seed_at(db, "keyword", "delta", "ask about delta", "+15550000001", 0, 0, now - 40);
    static const char msg[] = "alpha beta gamma delta";
    static const char expected[] =
        "[PROSPECTIVE MEMORY: Remember to: ask about alpha (triggered by: alpha) | ask about "
        "beta (triggered by: beta) | ask about gamma (triggered by: gamma)]";
    size_t len = 0;
    char *d = hu_prospective_directive_build(&alloc, db, msg, sizeof(msg) - 1, "+15550000001",
                                             12, now, &len);
    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_STR_EQ(d, expected);
    HU_ASSERT_EQ(len, sizeof(expected) - 1);
    alloc.free(alloc.ctx, d, len + 1);
    HU_ASSERT_EQ(count_open_for(db, "+15550000001"), (int64_t)1); /* delta */
    mem.vtable->deinit(mem.ctx);
}
```

and register it last in `run_prospective_tests`:

```c
    HU_RUN_TEST(directive_build_legacy_bytes_are_pinned);
```

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human_tests -j8 && cd "$W" && ./build/human_tests --suite=prospective --filter=legacy_bytes`
Expected: PASS. It pins today's behavior before the refactor.

- [ ] **Step 2: Write the failing policy tests**

Create `tests/test_prospective_policy.c`:

```c
/* tests/test_prospective_policy.c
 *
 * Prospective memory v2 — the pure decisions (spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.2-4.4).
 * No database and no model: every predicate's truth table, per
 * .claude/rules/security-predicate-extraction.md. */
#include "test_framework.h"

#include "human/memory/prospective_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void policy_column_spellings_round_trip(void) {
    hu_prospective_cue_kind_t k = HU_PM_CUE_TIME;
    HU_ASSERT_STR_EQ(hu_prospective_cue_kind_str(HU_PM_CUE_KEYWORD), "keyword");
    HU_ASSERT_STR_EQ(hu_prospective_cue_kind_str(HU_PM_CUE_TIME), "time");
    HU_ASSERT_STR_EQ(hu_prospective_cue_kind_str(HU_PM_CUE_AFTER_EVENT), "after_event");
    HU_ASSERT_NULL(hu_prospective_cue_kind_str((hu_prospective_cue_kind_t)9));
    HU_ASSERT_TRUE(hu_prospective_cue_kind_parse("keyword", &k));
    HU_ASSERT_EQ(k, HU_PM_CUE_KEYWORD);
    HU_ASSERT_FALSE(hu_prospective_cue_kind_parse("Keyword", &k));
    HU_ASSERT_FALSE(hu_prospective_cue_kind_parse(NULL, &k));

    static const char *const names[] = {"pending", "surfaced", "done", "canceled", "expired"};
    hu_prospective_status_t s = HU_PM_PENDING;
    for (int i = 0; i < 5; i++) {
        HU_ASSERT_STR_EQ(hu_prospective_status_str((hu_prospective_status_t)i), names[i]);
        HU_ASSERT_TRUE(hu_prospective_status_parse(names[i], &s));
        HU_ASSERT_EQ(s, i);
    }
    HU_ASSERT_FALSE(hu_prospective_status_parse("fired", &s));
    HU_ASSERT_NULL(hu_prospective_status_str((hu_prospective_status_t)7));

    HU_ASSERT_NULL(hu_prospective_outcome_str(HU_PM_OUTCOME_NONE));
    HU_ASSERT_STR_EQ(hu_prospective_outcome_str(HU_PM_OUTCOME_USED), "used");
    HU_ASSERT_STR_EQ(hu_prospective_outcome_str(HU_PM_OUTCOME_IGNORED), "ignored");
    HU_ASSERT_STR_EQ(hu_prospective_outcome_str(HU_PM_OUTCOME_SUPPRESSED), "suppressed");
    HU_ASSERT_STR_EQ(hu_prospective_source_str(HU_PM_SOURCE_EXTRACTOR), "extractor");
    HU_ASSERT_STR_EQ(hu_prospective_source_str(HU_PM_SOURCE_PROMISE_KEEPER), "promise_keeper");
    HU_ASSERT_STR_EQ(hu_prospective_source_str(HU_PM_SOURCE_FOLLOWUP), "followup");
    HU_ASSERT_STR_EQ(hu_prospective_verdict_str(HU_PM_VERDICT_RESOLVED), "already_resolved");
    HU_ASSERT_STR_EQ(hu_prospective_verdict_str(HU_PM_VERDICT_PARSE_FAIL), "parse_fail");
}

static void fired_mapping_matches_spec(void) {
    HU_ASSERT_EQ(hu_prospective_status_to_fired(HU_PM_PENDING), 0);
    HU_ASSERT_EQ(hu_prospective_status_to_fired(HU_PM_SURFACED), 0);
    HU_ASSERT_EQ(hu_prospective_status_to_fired(HU_PM_DONE), 1);
    HU_ASSERT_EQ(hu_prospective_status_to_fired(HU_PM_CANCELED), 2);
    HU_ASSERT_EQ(hu_prospective_status_to_fired(HU_PM_EXPIRED), 3);
}

static void gates_default_off_and_banner_names_the_key(void) {
    unsetenv("HU_PROSPECTIVE");
    unsetenv("HU_PROSPECTIVE_TIME");
    HU_ASSERT_EQ(hu_prospective_gate_mode(), HU_GATE_OFF);
    HU_ASSERT_EQ(hu_prospective_time_gate_mode(), HU_GATE_OFF);
    setenv("HU_PROSPECTIVE", "shadow", 1);
    setenv("HU_PROSPECTIVE_TIME", "live", 1);
    HU_ASSERT_EQ(hu_prospective_gate_mode(), HU_GATE_SHADOW);
    HU_ASSERT_EQ(hu_prospective_time_gate_mode(), HU_GATE_LIVE);
    setenv("HU_PROSPECTIVE", "bogus", 1);
    HU_ASSERT_EQ(hu_prospective_gate_mode(), HU_GATE_OFF); /* unknown fails closed */
    unsetenv("HU_PROSPECTIVE");
    unsetenv("HU_PROSPECTIVE_TIME");
    HU_ASSERT_STR_CONTAINS(hu_prospective_gate_banner(HU_GATE_OFF, false),
                           "HU_PROSPECTIVE=shadow|live");
    HU_ASSERT_STR_CONTAINS(hu_prospective_gate_banner(HU_GATE_OFF, true),
                           "HU_PROSPECTIVE_TIME=shadow|live");
    HU_ASSERT_STR_CONTAINS(hu_prospective_gate_banner(HU_GATE_SHADOW, false), "SHADOW");
    HU_ASSERT_STR_CONTAINS(hu_prospective_gate_banner(HU_GATE_LIVE, true), "LIVE");
}

static hu_prospective_filter_facts_t facts(hu_prospective_cue_kind_t kind) {
    hu_prospective_filter_facts_t f;
    memset(&f, 0, sizeof(f));
    f.cue_kind = kind;
    f.status = HU_PM_PENDING;
    f.now = 1000000;
    f.grace_s = HU_PROSPECTIVE_TIME_GRACE_S;
    return f;
}

static void filter_keyword_truth_table(void) {
    hu_prospective_filter_facts_t f = facts(HU_PM_CUE_KEYWORD);
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_SKIP); /* not cued */
    f.keyword_in_text = true;
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_ELIGIBLE);
    f.expires_at = f.now + 1;
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_ELIGIBLE);
    f.expires_at = f.now; /* inclusive, like the legacy sweep */
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_EXPIRE);
    f.expires_at = 0;
    for (int st = HU_PM_SURFACED; st <= HU_PM_EXPIRED; st++) {
        f.status = (hu_prospective_status_t)st;
        HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_SKIP);
    }
    f.status = HU_PM_PENDING;
    f.is_group = true;
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_SKIP);
    f.expires_at = f.now - 1; /* a group turn never writes, not even an expiry */
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_SKIP);
    f.is_group = false;
    f.expires_at = 0;
    f.is_self = true;
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_SKIP);
    HU_ASSERT_EQ(hu_prospective_filter(NULL), HU_PM_FILTER_SKIP);
}

static void filter_time_due_grace_and_daily_cap(void) {
    hu_prospective_filter_facts_t f = facts(HU_PM_CUE_TIME);
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_SKIP); /* no due_at */
    f.due_at = f.now + 60;
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_SKIP); /* not due yet */
    f.due_at = f.now;
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_ELIGIBLE);
    f.due_at = f.now - f.grace_s; /* the last moment it may still fire */
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_ELIGIBLE);
    f.due_at = f.now - f.grace_s - 1;
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_EXPIRE); /* never fires late */
    f.due_at = f.now - 60;
    f.surfaced_today = 1;
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_CAPPED);
    f.surfaced_today = 0;
    f.keyword_in_text = true; /* irrelevant to a time cue */
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_ELIGIBLE);
    f.is_self = true;
    HU_ASSERT_EQ(hu_prospective_filter(&f), HU_PM_FILTER_SKIP);
    hu_prospective_filter_facts_t e = facts(HU_PM_CUE_AFTER_EVENT);
    e.keyword_in_text = true;
    e.due_at = e.now - 1;
    HU_ASSERT_EQ(hu_prospective_filter(&e), HU_PM_FILTER_SKIP); /* reserved, never eligible */
}

static void parse_verdict_truth_table(void) {
    static const struct {
        const char *raw;
        hu_prospective_verdict_t v;
    } cases[] = {
        {"fire", HU_PM_VERDICT_FIRE},
        {"Fire.", HU_PM_VERDICT_FIRE},
        {"  FIRE\n", HU_PM_VERDICT_FIRE},
        {"**fire**", HU_PM_VERDICT_FIRE},
        {"already_resolved", HU_PM_VERDICT_RESOLVED},
        {"Already resolved.", HU_PM_VERDICT_RESOLVED},
        {"resolved", HU_PM_VERDICT_RESOLVED},
        {"cancel", HU_PM_VERDICT_CANCEL},
        {"canceled", HU_PM_VERDICT_CANCEL},
        {"cancelled - they called it off", HU_PM_VERDICT_CANCEL},
        {"not_now", HU_PM_VERDICT_NOT_NOW},
        {"Not now.", HU_PM_VERDICT_NOT_NOW},
        {"not-now", HU_PM_VERDICT_NOT_NOW},
        {"<think>maybe fire</think>not_now", HU_PM_VERDICT_NOT_NOW},
        {"<think>fire is tempting", HU_PM_VERDICT_PARSE_FAIL},
        {"lol yeah def bring it up", HU_PM_VERDICT_PARSE_FAIL},
        {"The answer is fire", HU_PM_VERDICT_PARSE_FAIL},
        {"firework", HU_PM_VERDICT_PARSE_FAIL},
        {"not", HU_PM_VERDICT_PARSE_FAIL},
        {"", HU_PM_VERDICT_PARSE_FAIL},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        HU_ASSERT_EQ(hu_prospective_parse_verdict(cases[i].raw, strlen(cases[i].raw)),
                     cases[i].v);
    HU_ASSERT_EQ(hu_prospective_parse_verdict(NULL, 4), HU_PM_VERDICT_PARSE_FAIL);
    /* the length bounds the read: "fire" inside a longer buffer */
    HU_ASSERT_EQ(hu_prospective_parse_verdict("fireworks", 4), HU_PM_VERDICT_FIRE);
}

static void decide_fails_toward_silence(void) {
    HU_ASSERT_EQ(hu_prospective_decide(true, HU_PM_VERDICT_FIRE), HU_PM_ACT_SURFACE);
    HU_ASSERT_EQ(hu_prospective_decide(true, HU_PM_VERDICT_RESOLVED), HU_PM_ACT_MARK_DONE);
    HU_ASSERT_EQ(hu_prospective_decide(true, HU_PM_VERDICT_CANCEL), HU_PM_ACT_MARK_CANCELED);
    HU_ASSERT_EQ(hu_prospective_decide(true, HU_PM_VERDICT_NOT_NOW), HU_PM_ACT_KEEP_PENDING);
    HU_ASSERT_EQ(hu_prospective_decide(true, HU_PM_VERDICT_PARSE_FAIL), HU_PM_ACT_KEEP_PENDING);
    /* a model error never acts, whatever verdict value is lying around */
    HU_ASSERT_EQ(hu_prospective_decide(false, HU_PM_VERDICT_FIRE), HU_PM_ACT_KEEP_PENDING);
    HU_ASSERT_EQ(hu_prospective_decide(false, HU_PM_VERDICT_RESOLVED), HU_PM_ACT_KEEP_PENDING);
    HU_ASSERT_EQ(hu_prospective_decide(false, HU_PM_VERDICT_CANCEL), HU_PM_ACT_KEEP_PENDING);
}

static void key_terms_are_content_words(void) {
    char t[HU_PROSPECTIVE_KEY_TERMS_MAX][HU_PROSPECTIVE_KEY_TERM_LEN];
    size_t n = hu_prospective_key_terms("Ask how the new TACO place was", t,
                                        HU_PROSPECTIVE_KEY_TERMS_MAX);
    HU_ASSERT_EQ(n, (size_t)2);
    HU_ASSERT_STR_EQ(t[0], "taco");
    HU_ASSERT_STR_EQ(t[1], "place");
    n = hu_prospective_key_terms("send her the guitar teacher's number", t, 6);
    HU_ASSERT_EQ(n, (size_t)3);
    HU_ASSERT_STR_EQ(t[1], "teacher"); /* possessive stripped */
    n = hu_prospective_key_terms("don't forget the taco taco place", t, 6);
    HU_ASSERT_EQ(n, (size_t)2); /* contraction and stop word dropped, duplicate folded */
    HU_ASSERT_EQ(hu_prospective_key_terms("ask about it", t, 6), (size_t)0);
    HU_ASSERT_EQ(hu_prospective_key_terms("alpha bravo charlie delta echoes foxtrot golfs", t, 6),
                 (size_t)6);
    HU_ASSERT_EQ(hu_prospective_key_terms(NULL, t, 6), (size_t)0);
}

static void reply_uses_action_needs_half_the_key_terms(void) {
    HU_ASSERT_TRUE(hu_prospective_reply_uses_action("ask how the new taco place was",
                                                    "wait how was the TACO place??", 29));
    HU_ASSERT_FALSE(hu_prospective_reply_uses_action("ask how the new taco place was",
                                                     "how was your weekend", 20));
    static const char *act = "check if he booked the dentist appointment";
    HU_ASSERT_FALSE(hu_prospective_reply_uses_action(act, "did you book the dentist", 24));
    HU_ASSERT_TRUE(hu_prospective_reply_uses_action(act, "dentist appointment when", 24));
    /* plural-tolerant both ways */
    HU_ASSERT_TRUE(hu_prospective_reply_uses_action("send the lasagna recipes",
                                                    "here's the lasagna recipe", 25));
    /* no key terms (non-ASCII word splits short): never provable */
    HU_ASSERT_FALSE(hu_prospective_reply_uses_action("ask about the caf\xc3\xa9",
                                                     "how was the caf\xc3\xa9", 17));
    HU_ASSERT_FALSE(hu_prospective_reply_uses_action("send the lasagna recipe", NULL, 0));
}

static void after_delivery_status_table(void) {
    HU_ASSERT_EQ(hu_prospective_after_delivery_status(true, 0, 2), HU_PM_DONE);
    HU_ASSERT_EQ(hu_prospective_after_delivery_status(true, 1, 2), HU_PM_DONE);
    HU_ASSERT_EQ(hu_prospective_after_delivery_status(false, 0, 2), HU_PM_PENDING);
    HU_ASSERT_EQ(hu_prospective_after_delivery_status(false, 1, 2), HU_PM_EXPIRED);
    HU_ASSERT_EQ(hu_prospective_after_delivery_status(false, 5, 2), HU_PM_EXPIRED);
}

static void render_styles_are_exact(void) {
    const char *acts[] = {"ask how the taco place was", "send the lasagna recipe", "c", "d"};
    const char *cues[] = {"taco place", "lasagna", "c", "d"};
    char buf[1024];
    size_t len = 0;
    HU_ASSERT_EQ(hu_prospective_render(HU_PM_RENDER_SOFT, acts, NULL, 2, buf, sizeof(buf), &len),
                 (size_t)2);
    HU_ASSERT_STR_EQ(buf, "[PROSPECTIVE MEMORY: If it fits naturally, you could bring up: ask "
                          "how the taco place was | send the lasagna recipe]");
    HU_ASSERT_EQ(len, strlen(buf));
    HU_ASSERT_EQ(hu_prospective_render(HU_PM_RENDER_LEGACY, acts, cues, 1, buf, sizeof(buf), &len),
                 (size_t)1);
    HU_ASSERT_STR_EQ(buf, "[PROSPECTIVE MEMORY: Remember to: ask how the taco place was "
                          "(triggered by: taco place)]");
    HU_ASSERT_EQ(hu_prospective_render(HU_PM_RENDER_SOFT, acts, NULL, 4, buf, sizeof(buf), &len),
                 (size_t)3); /* render cap */
    HU_ASSERT_EQ(
        hu_prospective_render(HU_PM_RENDER_DUE_LIST, acts, NULL, 2, buf, sizeof(buf), &len),
        (size_t)2);
    HU_ASSERT_STR_EQ(buf, "- ask how the taco place was\n- send the lasagna recipe\n");
    /* legacy loop parity: an action that cannot fit renders nothing */
    char *huge = (char *)malloc(1100);
    HU_ASSERT_NOT_NULL(huge);
    memset(huge, 'x', 1099);
    huge[1099] = '\0';
    const char *big[] = {huge};
    HU_ASSERT_EQ(hu_prospective_render(HU_PM_RENDER_LEGACY, big, big, 1, buf, sizeof(buf), &len),
                 (size_t)0);
    HU_ASSERT_STR_EQ(buf, "");
    HU_ASSERT_EQ(len, (size_t)0);
    free(huge);
    HU_ASSERT_EQ(hu_prospective_render(HU_PM_RENDER_LEGACY, acts, NULL, 1, buf, sizeof(buf), &len),
                 (size_t)0); /* legacy needs cues */
    HU_ASSERT_EQ(hu_prospective_render(HU_PM_RENDER_SOFT, NULL, NULL, 1, buf, sizeof(buf), &len),
                 (size_t)0);
}

static void judge_prompt_carries_history_intention_and_cue(void) {
    size_t sl = 0;
    const char *sys = hu_prospective_judge_system(&sl);
    HU_ASSERT_EQ(sl, strlen(sys));
    HU_ASSERT_STR_CONTAINS(sys, "not_now");
    HU_ASSERT_STR_CONTAINS(sys, "already_resolved");

    char buf[2048];
    static const char hist[] = "them: lasagna night friday?\nme: yes!\n";
    size_t n = hu_prospective_judge_user(buf, sizeof(buf), hist, sizeof(hist) - 1,
                                         "send the lasagna recipe", "lasagna",
                                         HU_PM_CUE_KEYWORD, 0);
    HU_ASSERT_TRUE(n > 0 && n == strlen(buf));
    HU_ASSERT_STR_CONTAINS(buf, "conversation (oldest first):\nthem: lasagna night friday?\n");
    HU_ASSERT_STR_CONTAINS(buf, "intention: send the lasagna recipe");
    HU_ASSERT_STR_CONTAINS(buf, "cue: they just mentioned \"lasagna\"");
    HU_ASSERT_TRUE(strcmp(buf + n - 7, "answer:") == 0);
    n = hu_prospective_judge_user(buf, sizeof(buf), NULL, 0, "send the lasagna recipe", NULL,
                                  HU_PM_CUE_TIME, 2 * 86400 + 5);
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_CONTAINS(buf, "(none)");
    HU_ASSERT_STR_CONTAINS(buf, "it came due 2 day(s) ago");

    /* a long history keeps its most recent lines, cut at a line start */
    char *longh = (char *)malloc(6000);
    HU_ASSERT_NOT_NULL(longh);
    size_t pos = 0;
    for (int i = 0; pos + 40 < 6000; i++)
        pos += (size_t)snprintf(longh + pos, 6000 - pos, "them: line %04d of the history\n", i);
    char big[6144];
    n = hu_prospective_judge_user(big, sizeof(big), longh, pos, "act", "cue", HU_PM_CUE_KEYWORD,
                                  0);
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_NULL(strstr(big, "line 0000"));
    HU_ASSERT_STR_CONTAINS(big, "\nthem: line ");
    free(longh);
    HU_ASSERT_EQ(hu_prospective_judge_user(buf, 16, hist, sizeof(hist) - 1, "act", "cue",
                                           HU_PM_CUE_KEYWORD, 0),
                 (size_t)0); /* does not fit: nothing half-written is used */
}

static void local_day_start_is_a_stable_midnight(void) {
    int64_t now = 1790000000;
    int64_t d = hu_prospective_local_day_start(now);
    HU_ASSERT_TRUE(d <= now);
    HU_ASSERT_TRUE(now - d < 86400 + 3600);
    HU_ASSERT_EQ(hu_prospective_local_day_start(d), d);
}

void run_prospective_policy_tests(void) {
    HU_TEST_SUITE("prospective policy");
    HU_RUN_TEST(policy_column_spellings_round_trip);
    HU_RUN_TEST(fired_mapping_matches_spec);
    HU_RUN_TEST(gates_default_off_and_banner_names_the_key);
    HU_RUN_TEST(filter_keyword_truth_table);
    HU_RUN_TEST(filter_time_due_grace_and_daily_cap);
    HU_RUN_TEST(parse_verdict_truth_table);
    HU_RUN_TEST(decide_fails_toward_silence);
    HU_RUN_TEST(key_terms_are_content_words);
    HU_RUN_TEST(reply_uses_action_needs_half_the_key_terms);
    HU_RUN_TEST(after_delivery_status_table);
    HU_RUN_TEST(render_styles_are_exact);
    HU_RUN_TEST(judge_prompt_carries_history_intention_and_cue);
    HU_RUN_TEST(local_day_start_is_a_stable_midnight);
}
```

Register it: in `CMakeLists.txt` after `    tests/test_prospective_repo_sqlite.c` add `    tests/test_prospective_policy.c`. In `tests/test_main.c` add `void run_prospective_policy_tests(void);` and `    run_prospective_policy_tests();` next to the Task 1 lines.

- [ ] **Step 3: Run the build to verify it fails**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human_tests -j8 2>&1 | tail -3`
Expected: FAIL with `'human/memory/prospective_policy.h' file not found`.

- [ ] **Step 4: Write the policy header**

Create `include/human/memory/prospective_policy.h`:

```c
#ifndef HU_MEMORY_PROSPECTIVE_POLICY_H
#define HU_MEMORY_PROSPECTIVE_POLICY_H
/*
 * Prospective memory v2 — the pure decisions (spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.2-4.4).
 *
 * Every function here depends only on its arguments (the two gate readers
 * add getenv): Filter, Decide, the done-after-evidence rule, verdict parsing,
 * the renderers and the judge prompt. The store (prospective_repo.h) and the
 * orchestration (prospective_v2.h) call these; tests pin their truth tables
 * without a database or a model (.claude/rules/security-predicate-extraction.md).
 */
#include "human/core/gate_mode.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_PROSPECTIVE_RENDER_CAP 3   /* intentions per directive */
#define HU_PROSPECTIVE_JUDGE_CAP 3    /* Decide calls per turn */
#define HU_PROSPECTIVE_MAX_ATTEMPTS 2 /* surfaced-but-unused before expired */
#define HU_PROSPECTIVE_TIME_GRACE_S (3 * 86400)
#define HU_PROSPECTIVE_BACKFILL_EXPIRE_S (14 * 86400)
#define HU_PROSPECTIVE_HISTORY_TURNS 20
#define HU_PROSPECTIVE_KEY_TERMS_MAX 6
#define HU_PROSPECTIVE_KEY_TERM_LEN 32

typedef enum hu_prospective_cue_kind {
    HU_PM_CUE_KEYWORD = 0,
    HU_PM_CUE_TIME,
    HU_PM_CUE_AFTER_EVENT, /* reserved: no v1 source, never eligible */
} hu_prospective_cue_kind_t;

typedef enum hu_prospective_status {
    HU_PM_PENDING = 0,
    HU_PM_SURFACED,
    HU_PM_DONE,
    HU_PM_CANCELED,
    HU_PM_EXPIRED,
} hu_prospective_status_t;

typedef enum hu_prospective_outcome {
    HU_PM_OUTCOME_NONE = 0,
    HU_PM_OUTCOME_USED,
    HU_PM_OUTCOME_IGNORED,
    HU_PM_OUTCOME_SUPPRESSED,
} hu_prospective_outcome_t;

typedef enum hu_prospective_source {
    HU_PM_SOURCE_EXTRACTOR = 0,
    HU_PM_SOURCE_PROMISE_KEEPER,
    HU_PM_SOURCE_FOLLOWUP,
} hu_prospective_source_t;

/* Column spellings. *_str returns NULL for an out-of-range value; *_parse
 * returns false (and leaves *out alone) for NULL or an unknown string. */
const char *hu_prospective_cue_kind_str(hu_prospective_cue_kind_t k);
bool hu_prospective_cue_kind_parse(const char *s, hu_prospective_cue_kind_t *out);
const char *hu_prospective_status_str(hu_prospective_status_t s);
bool hu_prospective_status_parse(const char *s, hu_prospective_status_t *out);
const char *hu_prospective_outcome_str(hu_prospective_outcome_t o); /* NULL for NONE */
const char *hu_prospective_source_str(hu_prospective_source_t s);

/* The legacy `fired` value a status writes: pending/surfaced 0, done 1,
 * canceled 2, expired 3. (The reverse mapping is the migration's trigger.) */
int hu_prospective_status_to_fired(hu_prospective_status_t s);

/* Gates (feature-gate-requires-measurement): HU_PROSPECTIVE for the reactive
 * keyword path, HU_PROSPECTIVE_TIME for time cues that initiate a message.
 * Both default OFF. */
hu_gate_mode_t hu_prospective_gate_mode(void);
hu_gate_mode_t hu_prospective_time_gate_mode(void);
/* The one operator line per process for a gate state
 * (silent-config-gated-subsystems): OFF names the key and values that
 * enable it. */
const char *hu_prospective_gate_banner(hu_gate_mode_t mode, bool time_gate);

/* ── Filter (§4.2): rule and clock checks, in code ───────────────────── */
typedef enum hu_prospective_filter {
    HU_PM_FILTER_SKIP = 0, /* not cued / not due / not eligible here */
    HU_PM_FILTER_ELIGIBLE, /* goes to Decide */
    HU_PM_FILTER_EXPIRE,   /* past its window: retire, never fire late */
    HU_PM_FILTER_CAPPED,   /* time cue over the 1-per-contact-per-day cap */
} hu_prospective_filter_t;

typedef struct hu_prospective_filter_facts {
    hu_prospective_cue_kind_t cue_kind;
    hu_prospective_status_t status;
    bool is_group;         /* group chat: never eligible */
    bool is_self;          /* the owner's own handle: never eligible */
    bool keyword_in_text;  /* whole-word match of the cue in the inbound text */
    int64_t due_at;        /* time cues; 0 = none */
    int64_t expires_at;    /* 0 = never */
    int64_t now;
    int64_t grace_s;       /* time cues: due_at + grace_s is the last moment to fire */
    size_t surfaced_today; /* time intentions already surfaced for this contact today */
} hu_prospective_filter_facts_t;

hu_prospective_filter_t hu_prospective_filter(const hu_prospective_filter_facts_t *f);

/* ── Decide (§4.3): the fire-time check ──────────────────────────────── */
typedef enum hu_prospective_verdict {
    HU_PM_VERDICT_FIRE = 0,
    HU_PM_VERDICT_RESOLVED,
    HU_PM_VERDICT_CANCEL,
    HU_PM_VERDICT_NOT_NOW,
    HU_PM_VERDICT_PARSE_FAIL,
} hu_prospective_verdict_t;

typedef enum hu_prospective_action {
    HU_PM_ACT_KEEP_PENDING = 0,
    HU_PM_ACT_SURFACE,
    HU_PM_ACT_MARK_DONE,
    HU_PM_ACT_MARK_CANCELED,
} hu_prospective_action_t;

/* First word of the judge's answer, case-folded, after an optional leaked
 * "<think>…</think>" block: "fire"; "already_resolved" / "already resolved"
 * / "resolved"; "cancel" / "canceled" / "cancelled"; "not_now" / "not now" /
 * "not-now". Anything else is PARSE_FAIL. Reads at most `len` bytes. */
hu_prospective_verdict_t hu_prospective_parse_verdict(const char *raw, size_t len);
/* "fire" | "already_resolved" | "cancel" | "not_now" | "parse_fail"; NULL out of range. */
const char *hu_prospective_verdict_str(hu_prospective_verdict_t v);

/* judge_ok=false (model error), NOT_NOW and PARSE_FAIL keep the item
 * pending: the system fails toward silence. */
hu_prospective_action_t hu_prospective_decide(bool judge_ok, hu_prospective_verdict_t v);

/* ── Done only after evidence (§4.3) ─────────────────────────────────── */
/* Content words of an action (>= 4 chars; not a function word, not a
 * generic intention verb like "ask"/"remember"/"check"/"send"; no
 * contractions; possessive 's stripped), lowercased, de-duplicated, at most
 * `max`. Returns the count. */
size_t hu_prospective_key_terms(const char *action, char out[][HU_PROSPECTIVE_KEY_TERM_LEN],
                                size_t max);
/* The delivered reply carries the action: at least half of its key terms
 * (and at least one) appear as whole words, a trailing plural 's' tolerated
 * either way. An action with no key terms can never be proven used. */
bool hu_prospective_reply_uses_action(const char *action, const char *reply, size_t reply_len);
/* Status after a delivery: used -> DONE; else attempts_before + 1 >=
 * max_attempts -> EXPIRED; else back to PENDING. */
hu_prospective_status_t hu_prospective_after_delivery_status(bool used, int attempts_before,
                                                             int max_attempts);

/* ── Rendering ───────────────────────────────────────────────────────── */
typedef enum hu_prospective_render_style {
    HU_PM_RENDER_LEGACY = 0, /* "[PROSPECTIVE MEMORY: Remember to: a (triggered by: c) | …]" */
    HU_PM_RENDER_SOFT,       /* "[PROSPECTIVE MEMORY: If it fits naturally, you could bring up: a | …]" */
    HU_PM_RENDER_DUE_LIST,   /* "- a\n- b\n" — the proactive due_followups section */
} hu_prospective_render_style_t;

/* Renders up to HU_PROSPECTIVE_RENDER_CAP of `n` items into buf[cap] and
 * returns how many were rendered; 0 means nothing usable (buf is "" and
 * *out_len 0). LEGACY is byte-for-byte the pre-v2 directive for the same
 * inputs and cap 1024, and needs `cues`; SOFT and DUE_LIST ignore `cues`. */
size_t hu_prospective_render(hu_prospective_render_style_t style, const char *const *actions,
                             const char *const *cues, size_t n, char *buf, size_t cap,
                             size_t *out_len);

/* ── Judge prompt ────────────────────────────────────────────────────── */
const char *hu_prospective_judge_system(size_t *len);
/* The user turn: the last lines of `history` (at most 4000 bytes, cut at a
 * line start; "(none)" when empty), the intention and its cue ("they just
 * mentioned \"<cue>\"" for keyword, "it came due N day(s) ago" for time),
 * then "answer:". Returns the bytes written, 0 when it does not fit. */
size_t hu_prospective_judge_user(char *buf, size_t cap, const char *history, size_t history_len,
                                 const char *action, const char *cue,
                                 hu_prospective_cue_kind_t kind, int64_t overdue_s);

/* Local midnight at or before `now` — the day of the per-day cap. */
int64_t hu_prospective_local_day_start(int64_t now);

#endif /* HU_MEMORY_PROSPECTIVE_POLICY_H */
```

- [ ] **Step 5: Write the policy implementation**

Create `src/memory/prospective_policy.c`:

```c
/*
 * src/memory/prospective_policy.c — the pure decisions of prospective memory
 * v2. Contract in include/human/memory/prospective_policy.h; spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.2-4.4.
 */
#include "human/memory/prospective_policy.h"

#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define PM_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const char *const k_pm_cue_kind[] = {"keyword", "time", "after_event"};
static const char *const k_pm_status[] = {"pending", "surfaced", "done", "canceled", "expired"};
static const char *const k_pm_source[] = {"extractor", "promise_keeper", "followup"};
static const char *const k_pm_verdict[] = {"fire", "already_resolved", "cancel", "not_now",
                                           "parse_fail"};

static const char *pm_name(const char *const *names, size_t n, int v) {
    return (v >= 0 && (size_t)v < n) ? names[v] : NULL;
}

static bool pm_lookup(const char *const *names, size_t n, const char *s, int *out) {
    if (!s)
        return false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(names[i], s) == 0) {
            *out = (int)i;
            return true;
        }
    }
    return false;
}

const char *hu_prospective_cue_kind_str(hu_prospective_cue_kind_t k) {
    return pm_name(k_pm_cue_kind, PM_COUNT(k_pm_cue_kind), (int)k);
}

bool hu_prospective_cue_kind_parse(const char *s, hu_prospective_cue_kind_t *out) {
    int v = 0;
    if (!out || !pm_lookup(k_pm_cue_kind, PM_COUNT(k_pm_cue_kind), s, &v))
        return false;
    *out = (hu_prospective_cue_kind_t)v;
    return true;
}

const char *hu_prospective_status_str(hu_prospective_status_t s) {
    return pm_name(k_pm_status, PM_COUNT(k_pm_status), (int)s);
}

bool hu_prospective_status_parse(const char *s, hu_prospective_status_t *out) {
    int v = 0;
    if (!out || !pm_lookup(k_pm_status, PM_COUNT(k_pm_status), s, &v))
        return false;
    *out = (hu_prospective_status_t)v;
    return true;
}

const char *hu_prospective_outcome_str(hu_prospective_outcome_t o) {
    switch (o) {
    case HU_PM_OUTCOME_USED:
        return "used";
    case HU_PM_OUTCOME_IGNORED:
        return "ignored";
    case HU_PM_OUTCOME_SUPPRESSED:
        return "suppressed";
    default:
        return NULL;
    }
}

const char *hu_prospective_source_str(hu_prospective_source_t s) {
    return pm_name(k_pm_source, PM_COUNT(k_pm_source), (int)s);
}

const char *hu_prospective_verdict_str(hu_prospective_verdict_t v) {
    return pm_name(k_pm_verdict, PM_COUNT(k_pm_verdict), (int)v);
}

int hu_prospective_status_to_fired(hu_prospective_status_t s) {
    switch (s) {
    case HU_PM_DONE:
        return 1;
    case HU_PM_CANCELED:
        return 2;
    case HU_PM_EXPIRED:
        return 3;
    default:
        return 0;
    }
}

hu_gate_mode_t hu_prospective_gate_mode(void) {
    return hu_gate_mode_from_env("HU_PROSPECTIVE", HU_GATE_OFF);
}

hu_gate_mode_t hu_prospective_time_gate_mode(void) {
    return hu_gate_mode_from_env("HU_PROSPECTIVE_TIME", HU_GATE_OFF);
}

const char *hu_prospective_gate_banner(hu_gate_mode_t mode, bool time_gate) {
    if (time_gate) {
        switch (mode) {
        case HU_GATE_LIVE:
            return "prospective time LIVE (HU_PROSPECTIVE_TIME=live): due follow-ups come from "
                   "the typed store, at most 1 per contact per day";
        case HU_GATE_SHADOW:
            return "prospective time SHADOW (HU_PROSPECTIVE_TIME=shadow): legacy follow-ups "
                   "unchanged; would-send counts logged";
        default:
            return "prospective time v2 disabled (HU_PROSPECTIVE_TIME unset or off); set "
                   "HU_PROSPECTIVE_TIME=shadow|live to run the per-contact due set";
        }
    }
    switch (mode) {
    case HU_GATE_LIVE:
        return "prospective LIVE (HU_PROSPECTIVE=live): the fire-time check decides every cued "
               "intention";
    case HU_GATE_SHADOW:
        return "prospective SHADOW (HU_PROSPECTIVE=shadow): legacy directive unchanged; "
               "Filter+Decide counts logged";
    default:
        return "prospective v2 disabled (HU_PROSPECTIVE unset or off); set "
               "HU_PROSPECTIVE=shadow|live to run the fire-time check";
    }
}

hu_prospective_filter_t hu_prospective_filter(const hu_prospective_filter_facts_t *f) {
    if (!f || f->status != HU_PM_PENDING || f->is_group || f->is_self)
        return HU_PM_FILTER_SKIP;
    if (f->expires_at > 0 && f->expires_at <= f->now)
        return HU_PM_FILTER_EXPIRE;
    switch (f->cue_kind) {
    case HU_PM_CUE_KEYWORD:
        return f->keyword_in_text ? HU_PM_FILTER_ELIGIBLE : HU_PM_FILTER_SKIP;
    case HU_PM_CUE_TIME:
        if (f->due_at <= 0 || f->now < f->due_at)
            return HU_PM_FILTER_SKIP;
        if (f->now > f->due_at + f->grace_s)
            return HU_PM_FILTER_EXPIRE;
        return f->surfaced_today >= 1 ? HU_PM_FILTER_CAPPED : HU_PM_FILTER_ELIGIBLE;
    default:
        return HU_PM_FILTER_SKIP;
    }
}

/* One [a-z_] word, lowercased into w[cap]; returns the bytes consumed. */
static size_t pm_word(const char *p, const char *end, char *w, size_t cap) {
    size_t n = 0;
    while (p + n < end && (isalpha((unsigned char)p[n]) || p[n] == '_')) {
        if (n + 1 < cap)
            w[n] = (char)tolower((unsigned char)p[n]);
        n++;
    }
    w[n < cap ? n : cap - 1] = '\0';
    return n;
}

hu_prospective_verdict_t hu_prospective_parse_verdict(const char *raw, size_t len) {
    if (!raw || len == 0)
        return HU_PM_VERDICT_PARSE_FAIL;
    const char *p = raw;
    const char *end = raw + len;
    for (const char *q = raw; q + 8 <= end; q++) /* a leaked thinking block is not the answer */
        if (memcmp(q, "</think>", 8) == 0)
            p = q + 8;
    while (p < end && !isalpha((unsigned char)*p))
        p++;
    char w[48];
    size_t n = pm_word(p, end, w, sizeof(w));
    if (strcmp(w, "not") == 0 || strcmp(w, "already") == 0) {
        const char *q = p + n;
        while (q < end && (*q == ' ' || *q == '-'))
            q++;
        char w2[24];
        (void)pm_word(q, end, w2, sizeof(w2));
        size_t wl = strlen(w);
        size_t l2 = strlen(w2);
        if (wl + 1 + l2 < sizeof(w)) {
            w[wl] = '_';
            memcpy(w + wl + 1, w2, l2 + 1);
        }
    }
    if (strcmp(w, "fire") == 0)
        return HU_PM_VERDICT_FIRE;
    if (strcmp(w, "already_resolved") == 0 || strcmp(w, "resolved") == 0)
        return HU_PM_VERDICT_RESOLVED;
    if (strcmp(w, "cancel") == 0 || strcmp(w, "canceled") == 0 || strcmp(w, "cancelled") == 0)
        return HU_PM_VERDICT_CANCEL;
    if (strcmp(w, "not_now") == 0)
        return HU_PM_VERDICT_NOT_NOW;
    return HU_PM_VERDICT_PARSE_FAIL;
}

hu_prospective_action_t hu_prospective_decide(bool judge_ok, hu_prospective_verdict_t v) {
    if (!judge_ok)
        return HU_PM_ACT_KEEP_PENDING;
    switch (v) {
    case HU_PM_VERDICT_FIRE:
        return HU_PM_ACT_SURFACE;
    case HU_PM_VERDICT_RESOLVED:
        return HU_PM_ACT_MARK_DONE;
    case HU_PM_VERDICT_CANCEL:
        return HU_PM_ACT_MARK_CANCELED;
    default:
        return HU_PM_ACT_KEEP_PENDING;
    }
}

/* Words that never identify WHAT an intention is about. Only >= 4-char words
 * matter (shorter ones are skipped before this list is consulted). */
static const char *const k_pm_stop[] = {
    "about", "again",  "also",   "back",   "bring", "check", "could", "does",   "done",
    "follow", "forget", "from",  "going",  "gonna", "have",  "into",  "just",   "know",
    "later", "maybe",  "more",   "much",   "need",  "next",  "over",  "remember", "remind",
    "send",  "should", "some",   "still",  "sure",  "tell",  "that",  "their",  "them",
    "then",  "there",  "they",   "thing",  "this",  "time",  "want",  "what",   "when",
    "will",  "with",   "would",  "your",
};

static bool pm_is_stop(const char *w) {
    for (size_t i = 0; i < PM_COUNT(k_pm_stop); i++)
        if (strcmp(k_pm_stop[i], w) == 0)
            return true;
    return false;
}

size_t hu_prospective_key_terms(const char *action, char out[][HU_PROSPECTIVE_KEY_TERM_LEN],
                                size_t max) {
    size_t n = 0;
    if (!action || !out)
        return 0;
    const char *p = action;
    while (*p && n < max) {
        while (*p && !isalnum((unsigned char)*p))
            p++;
        char w[HU_PROSPECTIVE_KEY_TERM_LEN];
        size_t wl = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '\'')) {
            if (wl + 1 < sizeof(w))
                w[wl++] = (char)tolower((unsigned char)*p);
            p++;
        }
        w[wl] = '\0';
        if (wl >= 2 && w[wl - 2] == '\'' && w[wl - 1] == 's') {
            wl -= 2;
            w[wl] = '\0';
        }
        if (wl < 4 || strchr(w, '\'') || pm_is_stop(w))
            continue;
        bool dup = false;
        for (size_t i = 0; i < n && !dup; i++)
            dup = strcmp(out[i], w) == 0;
        if (!dup)
            memcpy(out[n++], w, wl + 1);
    }
    return n;
}

static bool pm_reply_has_term(const char *reply, size_t len, const char *term) {
    if (hu_str_contains_word_ci_n(reply, len, term))
        return true;
    char alt[HU_PROSPECTIVE_KEY_TERM_LEN + 2];
    size_t tl = strlen(term);
    if (tl > 4 && term[tl - 1] == 's') {
        memcpy(alt, term, tl - 1);
        alt[tl - 1] = '\0';
    } else {
        memcpy(alt, term, tl);
        alt[tl] = 's';
        alt[tl + 1] = '\0';
    }
    return hu_str_contains_word_ci_n(reply, len, alt);
}

bool hu_prospective_reply_uses_action(const char *action, const char *reply, size_t reply_len) {
    if (!reply || reply_len == 0)
        return false;
    char terms[HU_PROSPECTIVE_KEY_TERMS_MAX][HU_PROSPECTIVE_KEY_TERM_LEN];
    size_t n = hu_prospective_key_terms(action, terms, HU_PROSPECTIVE_KEY_TERMS_MAX);
    if (n == 0)
        return false;
    size_t hit = 0;
    for (size_t i = 0; i < n; i++)
        if (pm_reply_has_term(reply, reply_len, terms[i]))
            hit++;
    return hit > 0 && hit * 2 >= n;
}

hu_prospective_status_t hu_prospective_after_delivery_status(bool used, int attempts_before,
                                                             int max_attempts) {
    if (used)
        return HU_PM_DONE;
    return attempts_before + 1 >= max_attempts ? HU_PM_EXPIRED : HU_PM_PENDING;
}

static const char k_pm_legacy_prefix[] = "[PROSPECTIVE MEMORY: Remember to: ";
static const char k_pm_soft_prefix[] =
    "[PROSPECTIVE MEMORY: If it fits naturally, you could bring up: ";

static size_t pm_render_due_list(const char *const *actions, size_t n, char *buf, size_t cap,
                                 size_t *out_len) {
    size_t pos = 0;
    size_t rendered = 0;
    for (size_t i = 0; i < n && i < HU_PROSPECTIVE_RENDER_CAP; i++) {
        int w = snprintf(buf + pos, cap - pos, "- %s\n", actions[i]);
        if (w <= 0 || pos + (size_t)w >= cap)
            break;
        pos += (size_t)w;
        rendered++;
    }
    buf[pos] = '\0';
    *out_len = pos;
    return rendered;
}

size_t hu_prospective_render(hu_prospective_render_style_t style, const char *const *actions,
                             const char *const *cues, size_t n, char *buf, size_t cap,
                             size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!buf || cap < 2 || !out_len)
        return 0;
    buf[0] = '\0';
    if (!actions || n == 0 || (style == HU_PM_RENDER_LEGACY && !cues))
        return 0;
    if (style == HU_PM_RENDER_DUE_LIST)
        return pm_render_due_list(actions, n, buf, cap, out_len);
    /* LEGACY and SOFT: the pre-v2 loop, byte for byte (cap stands in for the
     * old sizeof(buf); `pos + 64 < cap` is its `pos < sizeof(buf) - 64`). */
    size_t pos = 0;
    int h = snprintf(buf, cap, "%s",
                     style == HU_PM_RENDER_LEGACY ? k_pm_legacy_prefix : k_pm_soft_prefix);
    if (h > 0 && (size_t)h < cap)
        pos = (size_t)h;
    size_t rendered = 0;
    for (size_t i = 0; i < n && i < HU_PROSPECTIVE_RENDER_CAP && pos + 64 < cap; i++) {
        if (i > 0) {
            memcpy(buf + pos, " | ", 3);
            pos += 3;
        }
        int w = style == HU_PM_RENDER_LEGACY
                    ? snprintf(buf + pos, cap - pos, "%s (triggered by: %s)", actions[i], cues[i])
                    : snprintf(buf + pos, cap - pos, "%s", actions[i]);
        if (w <= 0 || pos + (size_t)w >= cap)
            break;
        pos += (size_t)w;
        rendered++;
    }
    if (rendered == 0 || pos + 2 >= cap) {
        buf[0] = '\0';
        return 0;
    }
    buf[pos++] = ']';
    buf[pos] = '\0';
    *out_len = pos;
    return rendered;
}

static const char k_pm_judge_system[] =
    "You check one reminder before it is shown to Seth while he texts a friend. You see the "
    "recent conversation (oldest first) and one thing Seth meant to bring up. Answer with "
    "exactly one word:\n"
    "fire - it is still open and bringing it up now would be natural\n"
    "already_resolved - the conversation shows it already happened, was answered, or no "
    "longer applies\n"
    "cancel - Seth or the other person called it off\n"
    "not_now - still open, but this is not a good moment\n"
    "If you are unsure, answer not_now.";

const char *hu_prospective_judge_system(size_t *len) {
    if (len)
        *len = sizeof(k_pm_judge_system) - 1;
    return k_pm_judge_system;
}

size_t hu_prospective_judge_user(char *buf, size_t cap, const char *history, size_t history_len,
                                 const char *action, const char *cue,
                                 hu_prospective_cue_kind_t kind, int64_t overdue_s) {
    if (!buf || cap == 0)
        return 0;
    buf[0] = '\0';
    if (!action || !action[0])
        return 0;
    const char *h = history ? history : "";
    size_t hl = history ? history_len : 0;
    if (hl > 4000) { /* keep the most recent lines, starting at a line */
        const char *start = h + hl - 4000;
        const char *nl = memchr(start, '\n', (size_t)(h + hl - start));
        size_t skip = nl ? (size_t)(nl + 1 - h) : hl - 4000;
        h += skip;
        hl -= skip;
    }
    size_t pos = hl ? hu_buf_appendf(buf, cap, 0, "conversation (oldest first):\n%.*s\n",
                                     (int)hl, h)
                    : hu_buf_appendf(buf, cap, 0, "conversation (oldest first):\n(none)\n");
    if (kind == HU_PM_CUE_TIME)
        pos = hu_buf_appendf(buf, cap, pos,
                             "\nintention: %s\ncue: it came due %lld day(s) ago; nobody has "
                             "brought it up yet\n",
                             action, (long long)(overdue_s > 0 ? overdue_s / 86400 : 0));
    else
        pos = hu_buf_appendf(buf, cap, pos, "\nintention: %s\ncue: they just mentioned \"%s\"\n",
                             action, cue ? cue : "");
    pos = hu_buf_appendf(buf, cap, pos, "answer:");
    if (pos >= cap - 1) { /* hu_buf_appendf clamps to cap-1 on truncation */
        buf[0] = '\0';
        return 0;
    }
    return pos;
}

int64_t hu_prospective_local_day_start(int64_t now) {
    time_t t = (time_t)now;
    struct tm tmv;
    if (!localtime_r(&t, &tmv))
        return now - (now % 86400);
    tmv.tm_hour = 0;
    tmv.tm_min = 0;
    tmv.tm_sec = 0;
    tmv.tm_isdst = -1;
    time_t m = mktime(&tmv);
    return m == (time_t)-1 ? now - (now % 86400) : (int64_t)m;
}
```

In `CMakeLists.txt`, in the unconditional core list after `    src/memory/superhuman.c`, add:

```cmake
    src/memory/prospective_policy.c
```

- [ ] **Step 6: Give the policy module its product caller — the legacy renderer**

In `src/memory/prospective.c`, add `#include "human/memory/prospective_policy.h"` after `#include "human/memory/prospective.h"`. Delete `#define PROSPECTIVE_RENDER_CAP 3`. In `hu_prospective_directive_build`, replace everything from `    char buf[1024];` through the closing `}` of `if (rendered > 0 && pos + 2 < sizeof(buf)) { … }` with:

```c
    /* The render lives in prospective_policy.c so the v2 SOFT directive and
     * this legacy one share the loop; LEGACY is byte-identical to the pre-v2
     * code (pinned by directive_build_legacy_bytes_are_pinned). */
    char buf[1024];
    const char *acts[HU_PROSPECTIVE_RENDER_CAP];
    const char *cues[HU_PROSPECTIVE_RENDER_CAP];
    size_t m = count < HU_PROSPECTIVE_RENDER_CAP ? count : HU_PROSPECTIVE_RENDER_CAP;
    for (size_t i = 0; i < m; i++) {
        acts[i] = entries[i].action;
        cues[i] = entries[i].trigger_value;
    }
    size_t pos = 0;
    size_t rendered =
        hu_prospective_render(HU_PM_RENDER_LEGACY, acts, cues, m, buf, sizeof(buf), &pos);
    char *out = NULL;
    if (rendered > 0) {
        out = (char *)alloc->alloc(alloc->ctx, pos + 1);
        if (out) {
            memcpy(out, buf, pos + 1);
            *out_len = pos;
            /* A reminder is surfaced once: retire every keyword of the rendered
             * intentions so the next text does not re-inject them. */
            if (hu_prospective_mark_fired(db, entries, rendered) != HU_OK)
                hu_log_warn("prospective", NULL, "could not retire %zu surfaced triggers",
                            rendered);
            hu_log_info("prospective", NULL,
                        "fired %zu of %zu open triggers for %.*s: %s (cue: %s)", rendered, count,
                        (int)cid_len, contact_id ? contact_id : "", entries[0].action,
                        entries[0].trigger_value);
        }
    }
```

(The `alloc->free(alloc->ctx, entries, …)` and `return out;` lines that follow stay as they are.)

- [ ] **Step 7: Run the tests to verify they pass**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human human_tests -j8 && cd "$W" && ./build/human_tests --suite=prospective`
Expected: PASS. `prospective policy` passes 13/13, and `prospective memory triggers` passes 9/9, including `directive_build_legacy_bytes_are_pinned`, now running through `hu_prospective_render`.

- [ ] **Step 8: Full suite, ratchets, commit**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cd "$W" && ./build/human_tests 2>&1 | grep -E 'Results:' && bash scripts/check-clone-ratchet.sh && bash scripts/check-test-source-gate-symmetry.sh`
Expected: 0 failed, and both checks pass.

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2
git -C "$W" add include/human/memory/prospective_policy.h src/memory/prospective_policy.c \
  src/memory/prospective.c CMakeLists.txt tests/test_main.c tests/test_prospective_policy.c \
  tests/test_prospective.c
git -C "$W" commit -m "feat(prospective): pure Filter/Decide/evidence policy; legacy render moves onto it

Filter (keyword whole-word, time due_at + 3-day grace, 1/contact/day cap,
never group or self-chat), verdict parse, Decide failing toward silence,
done-only-after-evidence key terms, the SOFT and DUE_LIST renderers and the
judge prompt, each with a truth-table test. The legacy directive now
renders through hu_prospective_render; its bytes are pinned first.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Typed repository API

**Files:**
- Modify: `include/human/memory/prospective_repo.h`
- Modify: `src/memory/repos/prospective_repo_sqlite.c`
- Test: `tests/test_prospective_repo_sqlite.c`

**Interfaces:**
- Consumes: Task 2 enums and `hu_prospective_{cue_kind,status,outcome,source}_str`, `hu_prospective_cue_kind_parse`, `hu_prospective_status_parse`, `hu_prospective_status_to_fired`.
- Produces (exact signatures, used by Tasks 6, 8, 9, 10):
  - `typedef struct hu_prospective_item { int64_t id; hu_prospective_cue_kind_t cue_kind; hu_prospective_status_t status; char trigger_value[256]; char action[512]; char contact_id[128]; int64_t due_at; int64_t expires_at; int64_t created_at; int64_t surfaced_at; int attempts; } hu_prospective_item_t;`
  - `hu_error_t hu_prospective_repo_list(hu_allocator_t *alloc, sqlite3 *db, hu_prospective_cue_kind_t kind, hu_prospective_status_t status, const char *contact, size_t contact_len, hu_prospective_item_t **out, size_t *out_count);`
  - `void hu_prospective_repo_free(hu_allocator_t *alloc, hu_prospective_item_t *items, size_t count);`
  - `hu_error_t hu_prospective_repo_transition(sqlite3 *db, const hu_prospective_item_t *it, hu_prospective_status_t to, hu_prospective_outcome_t outcome, int attempts, int64_t now, int *changed);`
  - `hu_error_t hu_prospective_repo_count_surfaced_since(sqlite3 *db, hu_prospective_cue_kind_t kind, const char *contact, size_t contact_len, int64_t since, int64_t *out);`
  - `hu_error_t hu_prospective_repo_upsert_time(sqlite3 *db, const char *contact, size_t contact_len, const char *action, size_t action_len, int64_t due_at, int64_t grace_s, hu_prospective_source_t source, const char *source_key, hu_prospective_status_t status, int64_t now, bool *inserted);`
  - `hu_error_t hu_prospective_repo_sync_source(sqlite3 *db, const hu_prospective_item_t *it, hu_prospective_status_t to, int64_t now);`

- [ ] **Step 1: Write the failing tests**

In `tests/test_prospective_repo_sqlite.c`, add `#include "human/memory/prospective_policy.h"` after the `prospective_repo.h` include. Insert these before `void run_prospective_repo_sqlite_tests(void)`:

```c
static void seed_kw(sqlite3 *db, const char *cue, const char *action, const char *contact,
                    int64_t created) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO prospective_memories(trigger_type,trigger_value,action,contact_id,"
             "expires_at,fired,created_at) VALUES('keyword','%s','%s',%s%s%s,0,0,%lld)",
             cue, action, contact ? "'" : "", contact ? contact : "NULL", contact ? "'" : "",
             (long long)created);
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
}

static void repo_list_scope_order_and_exact_allocation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "flight", "ask about the flight", "+15550000001", 100);
    seed_kw(db, "dyson", "ask about the dyson", "+15550000001", 200);
    seed_kw(db, "global", "a contact-less intention", NULL, 150);
    seed_kw(db, "other", "someone else's", "+15550000002", 300);
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, "+15550000001", 12, "call about the lease",
                                                 20, 5000, 100, HU_PM_SOURCE_PROMISE_KEEPER,
                                                 "commitment:1", HU_PM_PENDING, 10, NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, "+15550000001", 12, "return the drill", 16,
                                                 3000, 100, HU_PM_SOURCE_FOLLOWUP, "followup:7",
                                                 HU_PM_PENDING, 10, NULL),
                 HU_OK);

    hu_prospective_item_t *items = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_prospective_repo_list(&alloc, db, HU_PM_CUE_KEYWORD, HU_PM_PENDING,
                                          "+15550000001", 12, &items, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, (size_t)3); /* own two + the contact-less one; never the other contact */
    HU_ASSERT_STR_EQ(items[0].action, "ask about the dyson"); /* newest first */
    HU_ASSERT_STR_EQ(items[1].action, "a contact-less intention");
    HU_ASSERT_STR_EQ(items[2].action, "ask about the flight");
    HU_ASSERT_EQ(items[0].cue_kind, HU_PM_CUE_KEYWORD);
    HU_ASSERT_EQ(items[0].status, HU_PM_PENDING);
    HU_ASSERT_STR_EQ(items[0].trigger_value, "dyson");
    hu_prospective_repo_free(&alloc, items, n);

    HU_ASSERT_EQ(hu_prospective_repo_list(&alloc, db, HU_PM_CUE_TIME, HU_PM_PENDING,
                                          "+15550000001", 12, &items, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, (size_t)2);
    HU_ASSERT_STR_EQ(items[0].action, "return the drill"); /* oldest due first */
    HU_ASSERT_EQ(items[0].due_at, (int64_t)3000);
    HU_ASSERT_EQ(items[0].expires_at, (int64_t)3100);
    HU_ASSERT_STR_EQ(items[0].trigger_value, "followup:7");
    hu_prospective_repo_free(&alloc, items, n);

    HU_ASSERT_EQ(hu_prospective_repo_list(&alloc, db, HU_PM_CUE_TIME, HU_PM_SURFACED,
                                          "+15550000001", 12, &items, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, (size_t)0);
    HU_ASSERT_NULL(items);
    HU_ASSERT_EQ(hu_prospective_repo_list(&alloc, db, (hu_prospective_cue_kind_t)9,
                                          HU_PM_PENDING, "+15550000001", 12, &items, &n),
                 HU_ERR_INVALID_ARGUMENT);
    mem.vtable->deinit(mem.ctx);
}

static void repo_transition_moves_the_whole_intention_and_fired(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "tacos", "ask about the taco place", "+15550000001", 100);
    seed_kw(db, "taco place", "ask about the taco place", "+15550000001", 100);
    seed_kw(db, "tacos", "ask about the taco place", "+15550000002", 100); /* other contact */
    hu_prospective_item_t *items = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_prospective_repo_list(&alloc, db, HU_PM_CUE_KEYWORD, HU_PM_PENDING,
                                          "+15550000001", 12, &items, &n),
                 HU_OK);
    HU_ASSERT_EQ(n, (size_t)2);
    int changed = 0;
    HU_ASSERT_EQ(hu_prospective_repo_transition(db, &items[0], HU_PM_SURFACED, HU_PM_OUTCOME_NONE,
                                                0, 777, &changed),
                 HU_OK);
    HU_ASSERT_EQ(changed, 2); /* both keyword rows of the intention */
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='surfaced' "
                           "AND surfaced_at=777 AND fired=0"),
                 (int64_t)2);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE "
                           "contact_id='+15550000002' AND status='pending'"),
                 (int64_t)1);
    HU_ASSERT_EQ(hu_prospective_repo_transition(db, &items[0], HU_PM_DONE, HU_PM_OUTCOME_USED, 0,
                                                888, &changed),
                 HU_OK);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='done' AND "
                           "fired=1 AND outcome='used' AND surfaced_at=777"),
                 (int64_t)2);
    /* a settled intention is never moved again */
    HU_ASSERT_EQ(hu_prospective_repo_transition(db, &items[0], HU_PM_PENDING,
                                                HU_PM_OUTCOME_IGNORED, 1, 999, &changed),
                 HU_OK);
    HU_ASSERT_EQ(changed, 0);
    HU_ASSERT_EQ(hu_prospective_repo_transition(db, &items[0], (hu_prospective_status_t)9,
                                                HU_PM_OUTCOME_NONE, 0, 1, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    hu_prospective_repo_free(&alloc, items, n);
    mem.vtable->deinit(mem.ctx);
}

static void repo_count_surfaced_since_counts_intentions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, "+15550000001", 12, "call the vet", 12, 500,
                                                 100, HU_PM_SOURCE_FOLLOWUP, "followup:1",
                                                 HU_PM_PENDING, 10, NULL),
                 HU_OK);
    hu_prospective_item_t *items = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_prospective_repo_list(&alloc, db, HU_PM_CUE_TIME, HU_PM_PENDING,
                                          "+15550000001", 12, &items, &n),
                 HU_OK);
    HU_ASSERT_EQ(hu_prospective_repo_transition(db, &items[0], HU_PM_SURFACED, HU_PM_OUTCOME_NONE,
                                                0, 1000, NULL),
                 HU_OK);
    hu_prospective_repo_free(&alloc, items, n);
    int64_t c = -1;
    HU_ASSERT_EQ(hu_prospective_repo_count_surfaced_since(db, HU_PM_CUE_TIME, "+15550000001", 12,
                                                          900, &c),
                 HU_OK);
    HU_ASSERT_EQ(c, (int64_t)1);
    HU_ASSERT_EQ(hu_prospective_repo_count_surfaced_since(db, HU_PM_CUE_TIME, "+15550000001", 12,
                                                          1001, &c),
                 HU_OK);
    HU_ASSERT_EQ(c, (int64_t)0);
    HU_ASSERT_EQ(hu_prospective_repo_count_surfaced_since(db, HU_PM_CUE_TIME, "+15550000002", 12,
                                                          0, &c),
                 HU_OK);
    HU_ASSERT_EQ(c, (int64_t)0);
    mem.vtable->deinit(mem.ctx);
}

/* A commitment and its paired delayed follow-up are ONE intention. */
static void repo_upsert_time_is_idempotent_by_key_and_by_intention(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    bool ins = false;
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, "+15550000001", 12, "call about the lease",
                                                 20, 5000, 100, HU_PM_SOURCE_PROMISE_KEEPER,
                                                 "commitment:1", HU_PM_PENDING, 10, &ins),
                 HU_OK);
    HU_ASSERT_TRUE(ins);
    /* same key again */
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, "+15550000001", 12, "call about the lease",
                                                 20, 5000, 100, HU_PM_SOURCE_PROMISE_KEEPER,
                                                 "commitment:1", HU_PM_PENDING, 10, &ins),
                 HU_OK);
    HU_ASSERT_FALSE(ins);
    /* the paired follow-up: other key, same contact + action + due */
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, "+15550000001", 12, "call about the lease",
                                                 20, 5000, 100, HU_PM_SOURCE_FOLLOWUP,
                                                 "followup:9", HU_PM_PENDING, 10, &ins),
                 HU_OK);
    HU_ASSERT_FALSE(ins);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE cue_kind='time'"),
                 (int64_t)1);
    char s[96];
    q_text(db,
           "SELECT trigger_type || '/' || source || '/' || status || '/' || fired || '/' || "
           "expires_at FROM prospective_memories WHERE cue_kind='time'",
           s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "time/promise_keeper/pending/0/5100");
    /* an import already past its window lands expired, fired=3 */
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, "+15550000001", 12, "old promise", 11, 40,
                                                 100, HU_PM_SOURCE_PROMISE_KEEPER,
                                                 "commitment:2", HU_PM_EXPIRED, 10, &ins),
                 HU_OK);
    HU_ASSERT_TRUE(ins);
    HU_ASSERT_EQ(q_int(db, "SELECT fired FROM prospective_memories WHERE action='old promise'"),
                 (int64_t)3);
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, "+15550000001", 12, "x", 1, 0, 100,
                                                 HU_PM_SOURCE_FOLLOWUP, "followup:1",
                                                 HU_PM_PENDING, 10, &ins),
                 HU_ERR_INVALID_ARGUMENT); /* no due time */
    mem.vtable->deinit(mem.ctx);
}

static void repo_sync_source_retires_ledger_twins(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(
        sqlite3_exec(db,
                     "INSERT INTO commitments(contact_id,description,who,deadline,status,"
                     "created_at) VALUES('+15550000001','call about the lease','me',5000,"
                     "'pending',1),('+15550000002','call about the lease','me',5000,'pending',1);"
                     "INSERT INTO delayed_followups(contact_id,topic,scheduled_at,sent) VALUES"
                     "('+15550000001','call about the lease',5000,0)",
                     NULL, NULL, NULL),
        SQLITE_OK);
    hu_prospective_item_t it;
    memset(&it, 0, sizeof(it));
    it.cue_kind = HU_PM_CUE_TIME;
    snprintf(it.action, sizeof(it.action), "call about the lease");
    snprintf(it.contact_id, sizeof(it.contact_id), "+15550000001");
    HU_ASSERT_EQ(hu_prospective_repo_sync_source(db, &it, HU_PM_DONE, 6000), HU_OK);
    char s[64];
    q_text(db, "SELECT status || '/' || followed_up_at FROM commitments WHERE "
               "contact_id='+15550000001'",
           s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "followed_up/6000");
    q_text(db, "SELECT status FROM commitments WHERE contact_id='+15550000002'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending");
    HU_ASSERT_EQ(q_int(db, "SELECT sent FROM delayed_followups"), (int64_t)1);
    HU_ASSERT_EQ(hu_prospective_repo_sync_source(db, &it, HU_PM_PENDING, 6000),
                 HU_ERR_INVALID_ARGUMENT); /* only settled states retire the ledger */
    mem.vtable->deinit(mem.ctx);
}
```

Register them in `run_prospective_repo_sqlite_tests` after the Task 1 entries:

```c
    HU_RUN_TEST(repo_list_scope_order_and_exact_allocation);
    HU_RUN_TEST(repo_transition_moves_the_whole_intention_and_fired);
    HU_RUN_TEST(repo_count_surfaced_since_counts_intentions);
    HU_RUN_TEST(repo_upsert_time_is_idempotent_by_key_and_by_intention);
    HU_RUN_TEST(repo_sync_source_retires_ledger_twins);
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human_tests -j8 2>&1 | grep -m3 error`
Expected: FAIL with `unknown type name 'hu_prospective_item_t'` / implicit declaration of `hu_prospective_repo_list`.

- [ ] **Step 3: Extend the header**

In `include/human/memory/prospective_repo.h`, replace the `#include "human/core/error.h"` line with:

```c
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/prospective_policy.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
```

and add after the `hu_prospective_repo_ensure_schema` declaration (still inside `#ifdef HU_ENABLE_SQLITE`):

```c
typedef struct hu_prospective_item {
    int64_t id;
    hu_prospective_cue_kind_t cue_kind;
    hu_prospective_status_t status;
    char trigger_value[256]; /* keyword cue; "commitment:<id>" / "followup:<id>" for time rows */
    char action[512];
    char contact_id[128]; /* "" when the row has no contact */
    int64_t due_at;       /* 0 = none */
    int64_t expires_at;   /* 0 = never */
    int64_t created_at;
    int64_t surfaced_at;  /* 0 = never surfaced */
    int attempts;
} hu_prospective_item_t;

/* Rows of `kind` in `status` for `contact`. Keyword reads keep the legacy
 * scope: the contact's rows plus rows with no contact, trigger_type
 * 'keyword' only, newest first. Time reads are the contact's rows, oldest
 * due first. *out holds exactly *out_count items (free with
 * hu_prospective_repo_free), NULL when none. */
hu_error_t hu_prospective_repo_list(hu_allocator_t *alloc, sqlite3 *db,
                                    hu_prospective_cue_kind_t kind, hu_prospective_status_t status,
                                    const char *contact, size_t contact_len,
                                    hu_prospective_item_t **out, size_t *out_count);
void hu_prospective_repo_free(hu_allocator_t *alloc, hu_prospective_item_t *items, size_t count);

/* Move the INTENTION — every row with the item's action, contact and cue
 * kind that is still pending or surfaced — to `to`: status and the matching
 * legacy fired value in one UPDATE, `attempts`, `outcome` (NONE leaves it),
 * and surfaced_at = now when `to` is SURFACED. *changed (may be NULL) is the
 * number of rows updated. */
hu_error_t hu_prospective_repo_transition(sqlite3 *db, const hu_prospective_item_t *it,
                                          hu_prospective_status_t to,
                                          hu_prospective_outcome_t outcome, int attempts,
                                          int64_t now, int *changed);

/* Distinct intentions of `kind` for `contact` with surfaced_at >= since. */
hu_error_t hu_prospective_repo_count_surfaced_since(sqlite3 *db, hu_prospective_cue_kind_t kind,
                                                    const char *contact, size_t contact_len,
                                                    int64_t since, int64_t *out);

/* Insert a cue_kind='time' row unless one already stands for this intention —
 * the same source key, or the same contact + action + due_at (a commitment
 * and its paired delayed follow-up are one intention) — atomically, in one
 * statement: trigger_type 'time', trigger_value =
 * source_key, expires_at = due_at + grace_s (so the legacy sweeps retire it
 * too), status and fired from `status`, created_at = now. *inserted may be
 * NULL. HU_ERR_INVALID_ARGUMENT for an empty contact/action/key or due_at <= 0. */
hu_error_t hu_prospective_repo_upsert_time(sqlite3 *db, const char *contact, size_t contact_len,
                                           const char *action, size_t action_len, int64_t due_at,
                                           int64_t grace_s, hu_prospective_source_t source,
                                           const char *source_key, hu_prospective_status_t status,
                                           int64_t now, bool *inserted);

/* A settled time intention (DONE / CANCELED / EXPIRED) retires its ledger
 * twins so the legacy readers never resurface it: pending commitments with
 * the same contact + description get status 'followed_up' / 'canceled' /
 * 'expired' and followed_up_at = now; unsent delayed_followups with the same
 * contact + topic get sent=1. */
hu_error_t hu_prospective_repo_sync_source(sqlite3 *db, const hu_prospective_item_t *it,
                                           hu_prospective_status_t to, int64_t now);
```

- [ ] **Step 4: Implement the queries**

Append to `src/memory/repos/prospective_repo_sqlite.c`, before the closing `#endif /* HU_ENABLE_SQLITE */`:

```c
static void pm_col_copy(sqlite3_stmt *st, int col, char *dst, size_t cap) {
    const unsigned char *s = sqlite3_column_text(st, col);
    size_t n = s ? (size_t)sqlite3_column_bytes(st, col) : 0;
    if (n >= cap)
        n = cap - 1;
    if (n)
        memcpy(dst, s, n);
    dst[n] = '\0';
}

static void pm_decode(sqlite3_stmt *st, hu_prospective_item_t *it) {
    memset(it, 0, sizeof(*it));
    it->id = sqlite3_column_int64(st, 0);
    if (!hu_prospective_cue_kind_parse((const char *)sqlite3_column_text(st, 1), &it->cue_kind))
        it->cue_kind = HU_PM_CUE_KEYWORD;
    if (!hu_prospective_status_parse((const char *)sqlite3_column_text(st, 2), &it->status))
        it->status = HU_PM_EXPIRED; /* unknown state: never eligible */
    pm_col_copy(st, 3, it->trigger_value, sizeof(it->trigger_value));
    pm_col_copy(st, 4, it->action, sizeof(it->action));
    pm_col_copy(st, 5, it->contact_id, sizeof(it->contact_id));
    it->due_at = sqlite3_column_int64(st, 6);
    it->expires_at = sqlite3_column_int64(st, 7);
    it->created_at = sqlite3_column_int64(st, 8);
    it->surfaced_at = sqlite3_column_int64(st, 9);
    it->attempts = sqlite3_column_int(st, 10);
}

static void pm_bind_contact(sqlite3_stmt *st, int idx, const char *contact, size_t len) {
    if (contact && len > 0)
        sqlite3_bind_text(st, idx, contact, (int)len, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, idx);
}

static const char k_pm_list[] =
    "SELECT id, cue_kind, status, trigger_value, action, contact_id, due_at, expires_at, "
    "created_at, surfaced_at, attempts FROM prospective_memories "
    "WHERE cue_kind = ?1 AND status = ?2 "
    "AND (contact_id = ?3 OR (?1 = 'keyword' AND contact_id IS NULL)) "
    "AND (?1 <> 'keyword' OR trigger_type = 'keyword') "
    "ORDER BY CASE WHEN ?1 = 'time' THEN due_at END ASC, created_at DESC, id DESC";

hu_error_t hu_prospective_repo_list(hu_allocator_t *alloc, sqlite3 *db,
                                    hu_prospective_cue_kind_t kind, hu_prospective_status_t status,
                                    const char *contact, size_t contact_len,
                                    hu_prospective_item_t **out, size_t *out_count) {
    if (out)
        *out = NULL;
    if (out_count)
        *out_count = 0;
    const char *kind_s = hu_prospective_cue_kind_str(kind);
    const char *status_s = hu_prospective_status_str(status);
    if (!alloc || !db || !out || !out_count || !kind_s || !status_s)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, k_pm_list, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(st, 1, kind_s, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, status_s, -1, SQLITE_STATIC);
    pm_bind_contact(st, 3, contact, contact_len);
    size_t cap = 0;
    size_t n = 0;
    hu_prospective_item_t *arr = NULL;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 8;
            hu_prospective_item_t *nb = (hu_prospective_item_t *)alloc->alloc(
                alloc->ctx, nc * sizeof(hu_prospective_item_t));
            if (!nb) {
                if (arr)
                    alloc->free(alloc->ctx, arr, cap * sizeof(hu_prospective_item_t));
                sqlite3_finalize(st);
                return HU_ERR_OUT_OF_MEMORY;
            }
            if (arr) {
                memcpy(nb, arr, n * sizeof(hu_prospective_item_t));
                alloc->free(alloc->ctx, arr, cap * sizeof(hu_prospective_item_t));
            }
            arr = nb;
            cap = nc;
        }
        pm_decode(st, &arr[n++]);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE || n == 0) {
        if (arr)
            alloc->free(alloc->ctx, arr, cap * sizeof(hu_prospective_item_t));
        return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_BACKEND;
    }
    /* Hand back exactly n items so the caller frees with the count it holds. */
    hu_prospective_item_t *exact =
        (hu_prospective_item_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_prospective_item_t));
    if (exact)
        memcpy(exact, arr, n * sizeof(hu_prospective_item_t));
    alloc->free(alloc->ctx, arr, cap * sizeof(hu_prospective_item_t));
    if (!exact)
        return HU_ERR_OUT_OF_MEMORY;
    *out = exact;
    *out_count = n;
    return HU_OK;
}

void hu_prospective_repo_free(hu_allocator_t *alloc, hu_prospective_item_t *items, size_t count) {
    if (alloc && items)
        alloc->free(alloc->ctx, items, count * sizeof(hu_prospective_item_t));
}

static const char k_pm_transition[] =
    "UPDATE prospective_memories SET status = ?1, fired = ?2, attempts = ?3, "
    "outcome = COALESCE(?4, outcome), "
    "surfaced_at = CASE WHEN ?1 = 'surfaced' THEN ?5 ELSE surfaced_at END "
    "WHERE action = ?6 AND cue_kind = ?7 AND status IN ('pending', 'surfaced') "
    "AND ((?8 IS NULL AND contact_id IS NULL) OR contact_id = ?8)";

hu_error_t hu_prospective_repo_transition(sqlite3 *db, const hu_prospective_item_t *it,
                                          hu_prospective_status_t to,
                                          hu_prospective_outcome_t outcome, int attempts,
                                          int64_t now, int *changed) {
    if (changed)
        *changed = 0;
    const char *to_s = hu_prospective_status_str(to);
    const char *kind_s = it ? hu_prospective_cue_kind_str(it->cue_kind) : NULL;
    if (!db || !it || !to_s || !kind_s || !it->action[0])
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, k_pm_transition, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    const char *out_s = hu_prospective_outcome_str(outcome);
    sqlite3_bind_text(st, 1, to_s, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, hu_prospective_status_to_fired(to));
    sqlite3_bind_int(st, 3, attempts);
    if (out_s)
        sqlite3_bind_text(st, 4, out_s, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 4);
    sqlite3_bind_int64(st, 5, now);
    sqlite3_bind_text(st, 6, it->action, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 7, kind_s, -1, SQLITE_STATIC);
    pm_bind_contact(st, 8, it->contact_id, strlen(it->contact_id));
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;
    if (changed)
        *changed = sqlite3_changes(db);
    return HU_OK;
}

hu_error_t hu_prospective_repo_count_surfaced_since(sqlite3 *db, hu_prospective_cue_kind_t kind,
                                                    const char *contact, size_t contact_len,
                                                    int64_t since, int64_t *out) {
    if (out)
        *out = 0;
    const char *kind_s = hu_prospective_cue_kind_str(kind);
    if (!db || !out || !kind_s || !contact || contact_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT COUNT(DISTINCT action) FROM prospective_memories WHERE "
                           "cue_kind = ?1 AND contact_id = ?2 AND surfaced_at >= ?3",
                           -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(st, 1, kind_s, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, contact, (int)contact_len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, since);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW)
        *out = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

static const char k_pm_upsert_time[] =
    "INSERT INTO prospective_memories(trigger_type, trigger_value, action, contact_id, "
    "expires_at, fired, created_at, cue_kind, due_at, status, source) "
    "SELECT 'time', ?1, ?2, ?3, ?4, ?5, ?6, 'time', ?7, ?8, ?9 "
    "WHERE NOT EXISTS (SELECT 1 FROM prospective_memories WHERE cue_kind = 'time' AND "
    "(trigger_value = ?1 OR (contact_id = ?3 AND action = ?2 AND due_at = ?7)))";

hu_error_t hu_prospective_repo_upsert_time(sqlite3 *db, const char *contact, size_t contact_len,
                                           const char *action, size_t action_len, int64_t due_at,
                                           int64_t grace_s, hu_prospective_source_t source,
                                           const char *source_key, hu_prospective_status_t status,
                                           int64_t now, bool *inserted) {
    if (inserted)
        *inserted = false;
    const char *status_s = hu_prospective_status_str(status);
    const char *source_s = hu_prospective_source_str(source);
    if (!db || !contact || contact_len == 0 || !action || action_len == 0 || !source_key ||
        !source_key[0] || due_at <= 0 || !status_s || !source_s)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, k_pm_upsert_time, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(st, 1, source_key, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, action, (int)action_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, contact, (int)contact_len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, due_at + grace_s);
    sqlite3_bind_int(st, 5, hu_prospective_status_to_fired(status));
    sqlite3_bind_int64(st, 6, now);
    sqlite3_bind_int64(st, 7, due_at);
    sqlite3_bind_text(st, 8, status_s, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 9, source_s, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;
    if (inserted)
        *inserted = sqlite3_changes(db) > 0;
    return HU_OK;
}

hu_error_t hu_prospective_repo_sync_source(sqlite3 *db, const hu_prospective_item_t *it,
                                           hu_prospective_status_t to, int64_t now) {
    const char *ledger = to == HU_PM_DONE       ? "followed_up"
                         : to == HU_PM_CANCELED ? "canceled"
                         : to == HU_PM_EXPIRED  ? "expired"
                                                : NULL;
    if (!db || !it || !it->contact_id[0] || !it->action[0] || !ledger)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "UPDATE commitments SET status = ?1, followed_up_at = ?2 WHERE "
                           "contact_id = ?3 AND description = ?4 AND status = 'pending'",
                           -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(st, 1, ledger, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, now);
    sqlite3_bind_text(st, 3, it->contact_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, it->action, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;
    if (sqlite3_prepare_v2(db,
                           "UPDATE delayed_followups SET sent = 1 WHERE contact_id = ?1 AND "
                           "topic = ?2 AND sent = 0",
                           -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    sqlite3_bind_text(st, 1, it->contact_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, it->action, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_MEMORY_BACKEND;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human human_tests -j8 && cd "$W" && ./build/human_tests --suite="prospective repo"`
Expected: PASS, 10/10.

- [ ] **Step 6: Full suite, clone ratchet, commit**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cd "$W" && ./build/human_tests 2>&1 | grep -E 'Results:' && bash scripts/check-clone-ratchet.sh`
Expected: 0 failed, and the clone count at or under its ceiling. If it grew, fold the repeated `prepare/bind/step/finalize` tails into one static helper rather than copying them.

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2
git -C "$W" add include/human/memory/prospective_repo.h src/memory/repos/prospective_repo_sqlite.c \
  tests/test_prospective_repo_sqlite.c
git -C "$W" commit -m "feat(prospective): typed repository — list, intention-wide transitions, time upsert

Every v2 SQL statement lives in the repo: typed reads with the legacy keyword
scope, transitions that write status and fired together, the per-day surfaced
count, an atomic time-row upsert that treats a commitment and its paired
follow-up as one intention, and the ledger sync that retires both twins.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Extract the thinking-off one-shot call

**Files:**
- Create: `include/human/providers/chat_oneshot.h`
- Create: `src/providers/chat_oneshot.c`
- Modify: `src/agent/init_proposer.c` (the body of `init_proposer_call_llm`, ~:659-718)
- Modify: `CMakeLists.txt` (after `    src/providers/factory.c` ~:964; test list), `tests/test_main.c`
- Test: `tests/test_chat_oneshot.c`

**Interfaces:**
- Consumes: `hu_provider_t`, `hu_chat_request_t`, `hu_chat_message_t`, `hu_chat_response_t`, `hu_chat_response_free` (`include/human/provider.h`).
- Produces: `typedef struct hu_chat_oneshot_opts { double temperature; uint32_t max_tokens; bool json_object; } hu_chat_oneshot_opts_t;` and `hu_error_t hu_provider_chat_oneshot(hu_allocator_t *alloc, hu_provider_t *provider, const char *model, size_t model_len, const char *system, size_t system_len, const char *user, size_t user_len, const hu_chat_oneshot_opts_t *opts, char **out, size_t *out_len);`. On a non-empty answer, `*out` is heap memory freed with `alloc->free(ctx, *out, *out_len + 1)`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_chat_oneshot.c`:

```c
/* tests/test_chat_oneshot.c
 *
 * hu_provider_chat_oneshot (src/providers/chat_oneshot.c): the one-system,
 * one-user, short-answer request with thinking OFF that init_proposer uses
 * and the prospective Decide judge reuses. A recording mock provider pins
 * the request shape — thinking_budget 0 is the whole point (CLAUDE.md
 * "Gemini 3.x thinking-token budget gotcha"). No network. */
#include "test_framework.h"

#include "human/providers/chat_oneshot.h"
#include <string.h>

typedef struct mock {
    int calls;
    int cws_calls;
    int thinking_budget;
    uint32_t max_tokens;
    double temperature;
    char response_format[32];
    char system[64];
    char user[64];
    char model[32];
    const char *reply; /* NULL -> empty answer */
} mock_t;

static hu_error_t mock_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                            const char *model, size_t model_len, double temperature,
                            hu_chat_response_t *out) {
    mock_t *m = (mock_t *)ctx;
    m->calls++;
    m->thinking_budget = req->thinking_budget;
    m->max_tokens = req->max_tokens;
    m->temperature = temperature;
    snprintf(m->response_format, sizeof(m->response_format), "%.*s",
             (int)req->response_format_len, req->response_format ? req->response_format : "");
    HU_ASSERT_EQ(req->messages_count, (size_t)2);
    HU_ASSERT_EQ(req->messages[0].role, HU_ROLE_SYSTEM);
    HU_ASSERT_EQ(req->messages[1].role, HU_ROLE_USER);
    snprintf(m->system, sizeof(m->system), "%.*s", (int)req->messages[0].content_len,
             req->messages[0].content);
    snprintf(m->user, sizeof(m->user), "%.*s", (int)req->messages[1].content_len,
             req->messages[1].content);
    snprintf(m->model, sizeof(m->model), "%.*s", (int)model_len, model);
    memset(out, 0, sizeof(*out));
    if (m->reply) {
        size_t n = strlen(m->reply);
        char *c = (char *)alloc->alloc(alloc->ctx, n + 1);
        memcpy(c, m->reply, n + 1);
        out->content = c;
        out->content_len = n;
    }
    return HU_OK;
}

static hu_error_t mock_cws(void *ctx, hu_allocator_t *alloc, const char *sys, size_t sys_len,
                           const char *msg, size_t msg_len, const char *model, size_t model_len,
                           double temperature, char **out, size_t *out_len) {
    (void)sys;
    (void)sys_len;
    (void)msg;
    (void)msg_len;
    (void)model;
    (void)model_len;
    mock_t *m = (mock_t *)ctx;
    m->cws_calls++;
    m->temperature = temperature;
    *out = (char *)alloc->alloc(alloc->ctx, 5);
    memcpy(*out, "fire", 5);
    *out_len = 4;
    return HU_OK;
}

static void oneshot_sends_a_short_thinking_off_request(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_t m;
    memset(&m, 0, sizeof(m));
    m.reply = "not_now";
    hu_provider_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.chat = mock_chat;
    vt.chat_with_system = mock_cws;
    hu_provider_t p = {.ctx = &m, .vtable = &vt};
    const hu_chat_oneshot_opts_t opts = {.temperature = 0.0, .max_tokens = 16,
                                         .json_object = false};
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_provider_chat_oneshot(&alloc, &p, "glm", 3, "sys", 3, "usr", 3, &opts, &out,
                                          &len),
                 HU_OK);
    HU_ASSERT_EQ(m.calls, 1);
    HU_ASSERT_EQ(m.cws_calls, 0); /* the structured chat() path is preferred */
    HU_ASSERT_EQ(m.thinking_budget, 0);
    HU_ASSERT_EQ(m.max_tokens, (uint32_t)16);
    HU_ASSERT_STR_EQ(m.response_format, "");
    HU_ASSERT_STR_EQ(m.system, "sys");
    HU_ASSERT_STR_EQ(m.user, "usr");
    HU_ASSERT_STR_EQ(m.model, "glm");
    HU_ASSERT_STR_EQ(out, "not_now");
    HU_ASSERT_EQ(len, (size_t)7);
    alloc.free(alloc.ctx, out, len + 1);

    const hu_chat_oneshot_opts_t json = {.temperature = 0.2, .max_tokens = 512,
                                         .json_object = true};
    m.reply = NULL; /* empty answer: HU_OK, nothing allocated */
    HU_ASSERT_EQ(hu_provider_chat_oneshot(&alloc, &p, NULL, 0, "s", 1, "u", 1, &json, &out, &len),
                 HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(len, (size_t)0);
    HU_ASSERT_STR_EQ(m.response_format, "json_object");
    HU_ASSERT_EQ(m.max_tokens, (uint32_t)512);
}

static void oneshot_falls_back_and_refuses_cleanly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_t m;
    memset(&m, 0, sizeof(m));
    hu_provider_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.chat_with_system = mock_cws;
    hu_provider_t p = {.ctx = &m, .vtable = &vt};
    const hu_chat_oneshot_opts_t opts = {.temperature = 0.1, .max_tokens = 16,
                                         .json_object = false};
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_provider_chat_oneshot(&alloc, &p, "m", 1, "s", 1, "u", 1, &opts, &out, &len),
                 HU_OK);
    HU_ASSERT_EQ(m.cws_calls, 1);
    HU_ASSERT_STR_EQ(out, "fire");
    alloc.free(alloc.ctx, out, len + 1);

    vt.chat_with_system = NULL;
    HU_ASSERT_EQ(hu_provider_chat_oneshot(&alloc, &p, "m", 1, "s", 1, "u", 1, &opts, &out, &len),
                 HU_ERR_NOT_SUPPORTED);
    hu_provider_t none = {.ctx = NULL, .vtable = NULL};
    HU_ASSERT_EQ(
        hu_provider_chat_oneshot(&alloc, &none, "m", 1, "s", 1, "u", 1, &opts, &out, &len),
        HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(hu_provider_chat_oneshot(&alloc, &p, "m", 1, "s", 1, "u", 1, NULL, &out, &len),
                 HU_ERR_INVALID_ARGUMENT);
}

void run_chat_oneshot_tests(void) {
    HU_TEST_SUITE("chat oneshot");
    HU_RUN_TEST(oneshot_sends_a_short_thinking_off_request);
    HU_RUN_TEST(oneshot_falls_back_and_refuses_cleanly);
}
```

Register it: in `CMakeLists.txt` add `    tests/test_chat_oneshot.c` after `tests/test_prospective_policy.c`. In `tests/test_main.c` add `void run_chat_oneshot_tests(void);` and `    run_chat_oneshot_tests();` next to the prospective lines.

- [ ] **Step 2: Run the build to verify it fails**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human_tests -j8 2>&1 | grep -m2 error`
Expected: FAIL with `'human/providers/chat_oneshot.h' file not found`.

- [ ] **Step 3: Implement the helper**

Create `include/human/providers/chat_oneshot.h`:

```c
#ifndef HU_PROVIDERS_CHAT_ONESHOT_H
#define HU_PROVIDERS_CHAT_ONESHOT_H
/* One system message, one user message, one short answer, thinking OFF.
 *
 * The request small classifiers need and chat_with_system() cannot express:
 * an explicit max_tokens and thinking_budget = 0. Gemini 3.x and GLM
 * otherwise spend the output budget reasoning (CLAUDE.md "Gemini 3.x
 * thinking-token budget gotcha"; init_proposer's 2026-05-26 err=42
 * truncations). Extracted unchanged from init_proposer.c so the prospective
 * Decide judge reuses the same request instead of a copy. */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hu_chat_oneshot_opts {
    double temperature;
    uint32_t max_tokens;
    bool json_object; /* response_format = "json_object" */
} hu_chat_oneshot_opts_t;

/* Prefers the structured chat() vtable. Falls back to chat_with_system()
 * (no max_tokens / thinking control there) for providers and mocks without
 * it, and returns HU_ERR_NOT_SUPPORTED when neither exists. On HU_OK with a
 * non-empty answer *out is heap (alloc), freed with
 * alloc->free(ctx, *out, *out_len + 1); an empty answer is HU_OK with *out
 * NULL. `model` may be NULL (sent as ""). */
hu_error_t hu_provider_chat_oneshot(hu_allocator_t *alloc, hu_provider_t *provider,
                                    const char *model, size_t model_len, const char *system,
                                    size_t system_len, const char *user, size_t user_len,
                                    const hu_chat_oneshot_opts_t *opts, char **out,
                                    size_t *out_len);

#endif /* HU_PROVIDERS_CHAT_ONESHOT_H */
```

Create `src/providers/chat_oneshot.c`:

```c
/* src/providers/chat_oneshot.c — see include/human/providers/chat_oneshot.h. */
#include "human/providers/chat_oneshot.h"

#include <string.h>

hu_error_t hu_provider_chat_oneshot(hu_allocator_t *alloc, hu_provider_t *provider,
                                    const char *model, size_t model_len, const char *system,
                                    size_t system_len, const char *user, size_t user_len,
                                    const hu_chat_oneshot_opts_t *opts, char **out,
                                    size_t *out_len) {
    if (!out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (!alloc || !provider || !system || !user || !opts)
        return HU_ERR_INVALID_ARGUMENT;
    if (!provider->vtable)
        return HU_ERR_NOT_SUPPORTED;
    if (!model) {
        model = "";
        model_len = 0;
    }
    if (provider->vtable->chat) {
        hu_chat_message_t msgs[2];
        memset(msgs, 0, sizeof(msgs));
        msgs[0].role = HU_ROLE_SYSTEM;
        msgs[0].content = system;
        msgs[0].content_len = system_len;
        msgs[1].role = HU_ROLE_USER;
        msgs[1].content = user;
        msgs[1].content_len = user_len;

        hu_chat_request_t req;
        memset(&req, 0, sizeof(req));
        req.messages = msgs;
        req.messages_count = 2;
        req.model = model;
        req.model_len = model_len;
        req.temperature = opts->temperature;
        req.max_tokens = opts->max_tokens;
        req.thinking_budget = 0; /* deterministic classifier — no thinking */
        if (opts->json_object) {
            req.response_format = "json_object";
            req.response_format_len = sizeof("json_object") - 1;
        }

        hu_chat_response_t resp;
        memset(&resp, 0, sizeof(resp));
        hu_error_t err = provider->vtable->chat(provider->ctx, alloc, &req, model, model_len,
                                                opts->temperature, &resp);
        if (err == HU_OK && resp.content && resp.content_len > 0) {
            char *buf = (char *)alloc->alloc(alloc->ctx, resp.content_len + 1);
            if (buf) {
                memcpy(buf, resp.content, resp.content_len);
                buf[resp.content_len] = '\0';
                *out = buf;
                *out_len = resp.content_len;
            } else {
                err = HU_ERR_OUT_OF_MEMORY;
            }
        }
        hu_chat_response_free(alloc, &resp);
        return err;
    }
    if (!provider->vtable->chat_with_system)
        return HU_ERR_NOT_SUPPORTED;
    return provider->vtable->chat_with_system(provider->ctx, alloc, system, system_len, user,
                                              user_len, model, model_len, opts->temperature, out,
                                              out_len);
}
```

In `CMakeLists.txt`, after `    src/providers/factory.c`:

```cmake
    src/providers/chat_oneshot.c
```

- [ ] **Step 4: Point init_proposer at it (no behavior change)**

In `src/agent/init_proposer.c`, add `#include "human/providers/chat_oneshot.h"` after `#include "human/provider.h"`. Keep the large rationale comment above `init_proposer_call_llm` and the `#if !HU_IS_TEST` guard. Replace the function body (everything between its opening `{` and closing `}`) with:

```c
    /* max_tokens 512, thinking_budget 0, json_object: see the comment above. */
    const hu_chat_oneshot_opts_t opts = {.temperature = 0.2, .max_tokens = 512,
                                         .json_object = true};
    return hu_provider_chat_oneshot(alloc, provider, model, strlen(model), sys_prompt,
                                    strlen(sys_prompt), user_msg, strlen(user_msg), &opts,
                                    out_response, out_response_len);
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human human_tests -j8 && cd "$W" && ./build/human_tests --suite="chat oneshot" && ./build/human_tests --filter=proposer`
Expected: PASS: `chat oneshot` 2/2, and every existing proposer test still passes.

- [ ] **Step 6: Full suite, ratchets, commit**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cd "$W" && ./build/human_tests 2>&1 | grep -E 'Results:' && bash scripts/check-clone-ratchet.sh && bash scripts/check-agent-core-boundary.sh`
Expected: 0 failed. The clone count should drop, because the request block now exists once. The boundary check passes.

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2
git -C "$W" add include/human/providers/chat_oneshot.h src/providers/chat_oneshot.c \
  src/agent/init_proposer.c CMakeLists.txt tests/test_main.c tests/test_chat_oneshot.c
git -C "$W" commit -m "refactor(providers): extract the thinking-off one-shot call from init_proposer

hu_provider_chat_oneshot is init_proposer_call_llm's request unchanged
(system + user, explicit max_tokens, thinking_budget 0, optional json_object,
chat_with_system fallback). The prospective Decide judge reuses it instead of
a second copy.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Carve the legacy time producers out of `hu_service_run` (pure move)

The proactive tick builds the commitment follow-up block (`daemon.c` ~:1064-1105, the global `LIMIT 3` read) and the `due_followups` section (~:1511-1548) inline. `daemon.c` is at the 10,420-line ceiling, so the time gate needs somewhere else to live. This task moves both blocks, **unchanged**, into `src/daemon/daemon_prospective_time.c`. It frees ~50 lines for Tasks 7 and 9. Behavior is byte-identical, and there is no gate yet.

**Files:**
- Create: `include/human/daemon/prospective_time.h`
- Create: `src/daemon/daemon_prospective_time.c`
- Modify: `src/daemon.c` (the include block ~:69-93; the F20 block ~:1063-1105; the due-followups producer ~:1511-1548)
- Modify: `CMakeLists.txt` (after `    src/daemon/daemon_dated_followup.c` ~:1247; test list after `tests/test_chat_oneshot.c`), `tests/test_main.c`
- Test: `tests/test_daemon_prospective_time.c`

**Interfaces:**
- Consumes: `hu_superhuman_commitment_list_due`, `hu_superhuman_commitment_free`, `hu_superhuman_delayed_followup_list_due`, `hu_superhuman_delayed_followup_free` (`include/human/memory/superhuman.h`), which are stubbed in no-SQLite builds; `hu_agent_t` (`human/agent.h`).
- Produces:
  - `void hu_daemon_prospective_commitment_ctx(hu_allocator_t *alloc, struct hu_agent *agent, const char *contact_id, int64_t now, char **ctx_out, size_t *ctx_len_out, int64_t ids_out[3], size_t *ids_count_out);`
  - `size_t hu_daemon_prospective_due_followups(hu_allocator_t *alloc, struct hu_agent *agent, struct hu_channel *ch, const char *target, size_t target_len, const char *contact_id, int64_t now, char *buf, size_t cap, int64_t *listed_id);`

  `ch`, `target` and `target_len` are unused until Task 9. They are in the signature now so `daemon.c` is edited once.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_daemon_prospective_time.c`:

```c
/* tests/test_daemon_prospective_time.c
 *
 * The proactive tick's time-cued producers (src/daemon/daemon_prospective_time.c),
 * moved out of hu_service_run unchanged: the F20 commitment follow-up lines
 * and the proposer's due_followups section. Task 9 adds the
 * HU_PROSPECTIVE_TIME gate on top of these. */
#include "test_framework.h"

#include "human/agent.h"
#include "human/daemon/prospective_time.h"
#include <string.h>

static void time_producers_need_memory_and_a_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static hu_agent_t agent;
    memset(&agent, 0, sizeof(agent)); /* no memory */
    char *ctx = (char *)&agent;
    size_t len = 9;
    int64_t ids[3];
    size_t n = 9;
    hu_daemon_prospective_commitment_ctx(&alloc, &agent, "+15550000001", 100, &ctx, &len, ids,
                                         &n);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(len, (size_t)0);
    HU_ASSERT_EQ(n, (size_t)0);
    char buf[64] = "stale";
    int64_t listed = -1;
    HU_ASSERT_EQ(hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0,
                                                     "+15550000001", 100, buf, sizeof(buf),
                                                     &listed),
                 (size_t)0);
    HU_ASSERT_STR_EQ(buf, "");
    HU_ASSERT_EQ(listed, (int64_t)-1);
}

#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/superhuman.h"

static void commitment_ctx_lists_this_contacts_due_commitments(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    static hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.memory = &mem;
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000001", 12,
                                                "call the dentist", 16, "me", 2, 1000),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000002", 12,
                                                "return the drill", 16, "me", 2, 1000),
                 HU_OK);
    char *ctx = NULL;
    size_t len = 0;
    int64_t ids[3];
    size_t n = 0;
    hu_daemon_prospective_commitment_ctx(&alloc, &agent, "+15550000001", 5000, &ctx, &len, ids,
                                         &n);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_STR_EQ(ctx, "COMMITMENT FOLLOW-UP: call the dentist was due. Ask if it happened: "
                          "'hey did you ever call the dentist?'\n");
    HU_ASSERT_EQ(len, strlen(ctx));
    HU_ASSERT_EQ(n, (size_t)1);
    HU_ASSERT_TRUE(ids[0] > 0);
    alloc.free(alloc.ctx, ctx, len + 1);
    mem.vtable->deinit(mem.ctx);
}

static void due_followups_lists_one_line_for_this_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    static hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.memory = &mem;
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000002", 12,
                                                         "their trip", 10, 500),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000001", 12,
                                                         "the job interview", 17, 1000),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000001", 12,
                                                         "the move", 8, 2000),
                 HU_OK);
    char buf[640];
    int64_t listed = -1;
    size_t n = hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0,
                                                   "+15550000001", 5000, buf, sizeof(buf),
                                                   &listed);
    HU_ASSERT_STR_EQ(buf, "- the job interview (due 4000s ago)\n"); /* oldest due, one line */
    HU_ASSERT_EQ(n, strlen(buf));
    HU_ASSERT_TRUE(listed > 0);
    listed = -1;
    HU_ASSERT_EQ(hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0,
                                                     "+15550000003", 5000, buf, sizeof(buf),
                                                     &listed),
                 (size_t)0);
    HU_ASSERT_EQ(listed, (int64_t)-1);
    mem.vtable->deinit(mem.ctx);
}
#endif /* HU_ENABLE_SQLITE */

void run_daemon_prospective_time_tests(void) {
    HU_TEST_SUITE("daemon prospective time");
    HU_RUN_TEST(time_producers_need_memory_and_a_contact);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(commitment_ctx_lists_this_contacts_due_commitments);
    HU_RUN_TEST(due_followups_lists_one_line_for_this_contact);
#endif
}
```

Register it: in `CMakeLists.txt` add `    tests/test_daemon_prospective_time.c` after `tests/test_chat_oneshot.c`. In `tests/test_main.c` add `void run_daemon_prospective_time_tests(void);` and `    run_daemon_prospective_time_tests();` next to the prospective lines.

- [ ] **Step 2: Run the build to verify it fails**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human_tests -j8 2>&1 | grep -m2 error`
Expected: FAIL with `'human/daemon/prospective_time.h' file not found`.

- [ ] **Step 3: Create the module with the moved code**

Create `include/human/daemon/prospective_time.h`:

```c
#ifndef HU_DAEMON_PROSPECTIVE_TIME_H
#define HU_DAEMON_PROSPECTIVE_TIME_H
/* Time-cued follow-ups for the proactive tick (spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.2,
 * §4.4). The two legacy producers below were moved out of hu_service_run
 * (src/daemon.c) unchanged; HU_PROSPECTIVE_TIME chooses between them and the
 * v2 per-contact due set. */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

struct hu_agent;
struct hu_channel;

/* F20: this contact's due commitments as "COMMITMENT FOLLOW-UP: …" lines.
 * *ctx_out is heap (free with *ctx_len_out + 1) or NULL; ids_out[3] receives
 * the listed commitment ids, marked followed-up by the caller on delivery. */
void hu_daemon_prospective_commitment_ctx(hu_allocator_t *alloc, struct hu_agent *agent,
                                          const char *contact_id, int64_t now, char **ctx_out,
                                          size_t *ctx_len_out, int64_t ids_out[3],
                                          size_t *ids_count_out);

/* The proposer's due_followups section for this contact, written to buf[cap].
 * Returns the bytes written (0 = nothing to list; buf is then ""). *listed_id
 * receives the delayed_followups id the caller marks sent on delivery, and is
 * left unchanged when nothing is listed. `ch`/`target` are the send channel
 * and handle (history for the v2 fire-time check). */
size_t hu_daemon_prospective_due_followups(hu_allocator_t *alloc, struct hu_agent *agent,
                                           struct hu_channel *ch, const char *target,
                                           size_t target_len, const char *contact_id,
                                           int64_t now, char *buf, size_t cap,
                                           int64_t *listed_id);

#endif /* HU_DAEMON_PROSPECTIVE_TIME_H */
```

Create `src/daemon/daemon_prospective_time.c`:

```c
/*
 * src/daemon/daemon_prospective_time.c — time-cued follow-ups for the
 * proactive tick. See include/human/daemon/prospective_time.h.
 *
 * Both producers are the hu_service_run bodies as of b622a96d4, moved
 * verbatim apart from the out-parameters (commitment_ctx / commitment_ids /
 * due_fu_buf / due_followup_id_listed were locals there).
 */
#include "human/daemon/prospective_time.h"

#include "human/agent.h"
#include "human/channel.h"
#include "human/memory/superhuman.h"

#include <stdio.h>
#include <string.h>

void hu_daemon_prospective_commitment_ctx(hu_allocator_t *alloc, struct hu_agent *agent,
                                          const char *contact_id, int64_t now, char **ctx_out,
                                          size_t *ctx_len_out, int64_t ids_out[3],
                                          size_t *ids_count_out) {
    if (ctx_out)
        *ctx_out = NULL;
    if (ctx_len_out)
        *ctx_len_out = 0;
    if (ids_count_out)
        *ids_count_out = 0;
    if (!alloc || !agent || !agent->memory || !contact_id || !ctx_out || !ctx_len_out ||
        !ids_out || !ids_count_out)
        return;
    hu_superhuman_commitment_t *due = NULL;
    size_t due_count = 0;
    if (hu_superhuman_commitment_list_due(agent->memory, alloc, now, 3, &due, &due_count) !=
            HU_OK ||
        !due || due_count == 0)
        return;
    size_t cid_len = strlen(contact_id);
    char ctx_buf[1024];
    size_t ctx_pos = 0;
    for (size_t di = 0; di < due_count && ctx_pos < sizeof(ctx_buf) - 200; di++) {
        if (cid_len != strlen(due[di].contact_id) ||
            memcmp(due[di].contact_id, contact_id, cid_len) != 0)
            continue;
        int n = snprintf(ctx_buf + ctx_pos, sizeof(ctx_buf) - ctx_pos,
                         "COMMITMENT FOLLOW-UP: %s was due. Ask if it happened: "
                         "'hey did you ever %s?'\n",
                         due[di].description, due[di].description);
        if (n > 0 && ctx_pos + (size_t)n < sizeof(ctx_buf)) {
            ctx_pos += (size_t)n;
            if (*ids_count_out < 3)
                ids_out[(*ids_count_out)++] = due[di].id;
        }
    }
    if (ctx_pos > 0) {
        char *c = (char *)alloc->alloc(alloc->ctx, ctx_pos + 1);
        if (c) {
            memcpy(c, ctx_buf, ctx_pos);
            c[ctx_pos] = '\0';
            *ctx_out = c;
            *ctx_len_out = ctx_pos;
        }
    }
    hu_superhuman_commitment_free(alloc, due, due_count);
}

/* Items are listed, not marked sent: mark-sent stays tied to an actual send
 * (the F31 path at the send site), so an unsent item correctly reappears. */
static size_t pm_legacy_due_followups(hu_allocator_t *alloc, struct hu_agent *agent,
                                      const char *contact_id, int64_t now, char *buf, size_t cap,
                                      int64_t *listed_id) {
    hu_delayed_followup_t *due_arr = NULL;
    size_t due_n = 0;
    if (hu_superhuman_delayed_followup_list_due(agent->memory, alloc, now, &due_arr, &due_n) !=
            HU_OK ||
        !due_arr || due_n == 0)
        return 0;
    size_t pos = 0;
    size_t listed = 0;
    for (size_t fi = 0; fi < due_n && listed < 1; fi++) {
        if (strcmp(due_arr[fi].contact_id, contact_id) != 0)
            continue;
        if (listed_id)
            *listed_id = due_arr[fi].id;
        int w = snprintf(buf + pos, cap - pos, "- %s (due %llds ago)\n", due_arr[fi].topic,
                         (long long)(now - due_arr[fi].scheduled_at));
        if (w <= 0 || (size_t)w >= cap - pos)
            break;
        pos += (size_t)w;
        listed++;
    }
    hu_superhuman_delayed_followup_free(alloc, due_arr, due_n);
    return pos;
}

size_t hu_daemon_prospective_due_followups(hu_allocator_t *alloc, struct hu_agent *agent,
                                           struct hu_channel *ch, const char *target,
                                           size_t target_len, const char *contact_id,
                                           int64_t now, char *buf, size_t cap,
                                           int64_t *listed_id) {
    (void)ch;
    (void)target;
    (void)target_len;
    if (buf && cap > 0)
        buf[0] = '\0';
    if (!alloc || !agent || !agent->memory || !contact_id || !buf || cap == 0)
        return 0;
    return pm_legacy_due_followups(alloc, agent, contact_id, now, buf, cap, listed_id);
}
```

In `CMakeLists.txt`, after `    src/daemon/daemon_dated_followup.c`:

```cmake
    src/daemon/daemon_prospective_time.c
```

- [ ] **Step 4: Replace the two blocks in `src/daemon.c`**

Add `#include "human/daemon/prospective_time.h"` to the `human/daemon/*.h` include block (alphabetically after `promise_keeper.h`).

Replace the F20 block. It starts at `#ifdef HU_ENABLE_SQLITE` / `/* F20: Commitment follow-up — add due commitments for this contact */` and ends at the `#endif` after `hu_superhuman_commitment_free(alloc, due, due_count);` and its two closing braces. Replace it with:

```c
#ifdef HU_ENABLE_SQLITE
            /* F20: Commitment follow-up — this contact's due commitments
             * (src/daemon/daemon_prospective_time.c). */
            char *commitment_ctx = NULL;
            size_t commitment_ctx_len = 0;
            int64_t commitment_ids[3];
            size_t commitment_ids_count = 0;
            if (agent && agent->memory && cp->contact_id)
                hu_daemon_prospective_commitment_ctx(alloc, agent, cp->contact_id, (int64_t)now,
                                                     &commitment_ctx, &commitment_ctx_len,
                                                     commitment_ids, &commitment_ids_count);
#endif
```

Replace the due-followups producer. It starts at `/* Producer for the due_followups field: surface THIS` and ends at the closing brace of `if (agent->memory) { … }`, just before `int64_t unified_last_tick = 0;`. Replace it with:

```c
                    /* Producer for the due_followups field: this contact's due
                     * delayed follow-up as a labeled section, so the proposer has
                     * a concrete trigger. Listed, not marked sent — mark-sent
                     * stays tied to an actual send (the F31 path at the send
                     * site). src/daemon/daemon_prospective_time.c. */
                    char due_fu_buf[640];
                    size_t due_fu_len = hu_daemon_prospective_due_followups(
                        alloc, agent, channels[c].channel, target_part, target_len,
                        cp->contact_id, (int64_t)now, due_fu_buf, sizeof(due_fu_buf),
                        &due_followup_id_listed);
                    if (due_fu_len > 0) {
                        inputs.due_followups_context = due_fu_buf;
                        inputs.due_followups_context_len = due_fu_len;
                    }
```

- [ ] **Step 5: Run the tests and the size checks**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human human_tests -j8 && cd "$W" && ./build/human_tests --suite="daemon prospective time" && wc -l src/daemon.c && bash scripts/check-file-size-ceiling.sh && bash scripts/check-function-length-ceiling.sh`
Expected: PASS, 3/3. `src/daemon.c` is now ~50 lines below 10420, and both size gates pass.

- [ ] **Step 6: Full suite, ratchets, commit**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cd "$W" && ./build/human_tests 2>&1 | grep -E 'Results:' && bash scripts/check-clone-ratchet.sh && bash scripts/check-dead-strip-ratchet.sh`
Expected: 0 failed. The clone count is unchanged or lower (the code moved, it was not copied). Dead-strip A/B are unchanged.

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2
git -C "$W" add include/human/daemon/prospective_time.h src/daemon/daemon_prospective_time.c \
  src/daemon.c CMakeLists.txt tests/test_main.c tests/test_daemon_prospective_time.c
git -C "$W" commit -m "refactor(daemon): move the proactive time-cue producers out of hu_service_run

The F20 commitment follow-up lines and the proposer's due_followups section
move to daemon_prospective_time.c unchanged, so the HU_PROSPECTIVE_TIME gate
can live beside them instead of growing daemon.c (at its 10,420-line ceiling).

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: The v2 pass (Filter → Decide → transitions) and `human prospective probe`

**Files:**
- Create: `include/human/memory/prospective_v2.h`, `src/memory/prospective_v2.c`
- Create: `include/human/cli_prospective.h`, `src/app/cli_prospective.c`
- Modify: `include/human/cli_commands.h` (declare `cmd_prospective`), `src/app/main.c` (command table, after the `memory` row ~:593)
- Modify: `CMakeLists.txt` (SQLite block after `        src/memory/prospective.c` ~:1679; core list after `    src/app/cli_ctl.c` ~:1280; test list after `tests/test_daemon_prospective_time.c`), `tests/test_main.c`
- Test: `tests/test_prospective_v2.c`, `tests/test_cli_prospective.c`

**Interfaces:**
- Consumes: Task 2 policy (`hu_prospective_filter`, `hu_prospective_parse_verdict`, `hu_prospective_decide`, `hu_prospective_reply_uses_action`, `hu_prospective_after_delivery_status`, `hu_prospective_render`, `hu_prospective_judge_system`, `hu_prospective_judge_user`, `hu_prospective_local_day_start`, `hu_prospective_verdict_str`); Task 3 repo (`hu_prospective_repo_list/free/transition/count_surfaced_since/sync_source`); `hu_str_contains_word_ci_n`.
- Produces (used by Tasks 7, 9, 10, 11):
  - `typedef hu_error_t (*hu_prospective_judge_fn)(void *ctx, hu_allocator_t *alloc, const char *system, size_t system_len, const char *user, size_t user_len, char **out, size_t *out_len);`
  - `typedef struct hu_prospective_judge { hu_prospective_judge_fn fn; void *ctx; } hu_prospective_judge_t;`
  - `typedef struct hu_prospective_turn { const char *contact; size_t contact_len; const char *inbound; size_t inbound_len; const char *history; size_t history_len; bool is_group; bool is_self; int64_t now; int64_t day_start; } hu_prospective_turn_t;`
  - `typedef struct hu_prospective_item_verdict { int64_t id; hu_prospective_verdict_t verdict; bool judge_ok; } hu_prospective_item_verdict_t;`
  - `typedef struct hu_prospective_counts { size_t candidates, fire, resolved, cancel, not_now, parse_fail, judge_err, expired, capped; hu_prospective_item_verdict_t items[HU_PROSPECTIVE_JUDGE_CAP]; size_t item_count; char fire_actions[HU_PROSPECTIVE_RENDER_CAP][256]; size_t fire_action_count; } hu_prospective_counts_t;`
  - `typedef struct hu_prospective_delivery_counts { size_t surfaced, used, ignored, expired; } hu_prospective_delivery_counts_t;`
  - `hu_error_t hu_prospective_v2_run(hu_allocator_t *alloc, sqlite3 *db, hu_prospective_cue_kind_t kind, const hu_prospective_turn_t *turn, const hu_prospective_judge_t *judge, bool apply, hu_prospective_counts_t *counts, char **directive, size_t *directive_len);`
  - `hu_error_t hu_prospective_v2_after_delivery(hu_allocator_t *alloc, sqlite3 *db, hu_prospective_cue_kind_t kind, const char *contact, size_t contact_len, const char *reply, size_t reply_len, int64_t now, hu_prospective_delivery_counts_t *out);`
  - CLI (`include/human/cli_prospective.h`): `hu_cli_prospective_op_t { HU_CLI_PM_NONE, HU_CLI_PM_INIT, HU_CLI_PM_INBOUND, HU_CLI_PM_TICK, HU_CLI_PM_DELIVER, HU_CLI_PM_BACKFILL }`, `hu_cli_prospective_args_t`, `bool hu_cli_prospective_parse(int argc, char **argv, hu_cli_prospective_args_t *out);`, `hu_error_t hu_cli_prospective_run(hu_allocator_t *alloc, hu_memory_t *mem, const hu_cli_prospective_args_t *a, const char *history, size_t history_len, const hu_prospective_judge_t *judge, FILE *out);`, and `hu_error_t cmd_prospective(hu_allocator_t *alloc, int argc, char **argv);` in `cli_commands.h`.
  - Probe output contract (Tasks 11–12 parse it): line 1 is `candidates=%zu fire=%zu resolved=%zu cancel=%zu not_now=%zu parse_fail=%zu judge_err=%zu expired=%zu capped=%zu bytes=%zu`. With `--full`, it is followed by one `item id=%lld verdict=<fire|already_resolved|cancel|not_now|parse_fail|judge_err>` line per judged item, then the directive text. `--deliver` prints `surfaced=%zu used=%zu ignored=%zu expired=%zu`. `init` prints `ok`.

- [ ] **Step 1: Write the failing v2 tests**

Create `tests/test_prospective_v2.c`:

```c
/* tests/test_prospective_v2.c
 *
 * hu_prospective_v2_run / _after_delivery (src/memory/prospective_v2.c): the
 * PIS-style pass over the typed store with a SCRIPTED judge (spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.2-4.3,
 * §4.5). Pins: a cued intention is decided once and surfaced with the soft
 * directive; already_resolved / cancel settle it and it never refires;
 * not_now, parse failures and model errors stay silent and pending; SHADOW
 * writes and renders nothing; group and self-chat are never judged; at most
 * three Decide calls per turn; time cues fire within their grace window, one
 * per contact per day; done only after a delivered reply carries the action;
 * an undelivered surfacing counts as an attempt. */
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/prospective_v2.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#define C1 "+15550000001"
#define NOW ((int64_t)1790000000)

typedef struct script {
    const char *const *replies;
    size_t n;
    size_t calls;
    hu_error_t err;
    char last_user[4096];
} script_t;

static hu_error_t scripted_judge(void *ctx, hu_allocator_t *alloc, const char *system,
                                 size_t system_len, const char *user, size_t user_len,
                                 char **out, size_t *out_len) {
    script_t *s = (script_t *)ctx;
    (void)system;
    (void)system_len;
    size_t cl = user_len < sizeof(s->last_user) - 1 ? user_len : sizeof(s->last_user) - 1;
    memcpy(s->last_user, user, cl);
    s->last_user[cl] = '\0';
    s->calls++;
    if (s->err != HU_OK)
        return s->err;
    const char *r = s->calls <= s->n ? s->replies[s->calls - 1] : "not_now";
    size_t rl = strlen(r);
    char *b = (char *)alloc->alloc(alloc->ctx, rl + 1);
    memcpy(b, r, rl + 1);
    *out = b;
    *out_len = rl;
    return HU_OK;
}

static hu_prospective_judge_t judge_of(script_t *s, const char *const *replies, size_t n) {
    memset(s, 0, sizeof(*s));
    s->replies = replies;
    s->n = n;
    hu_prospective_judge_t j = {.fn = scripted_judge, .ctx = s};
    return j;
}

static void seed_kw_exp(sqlite3 *db, const char *cue, const char *action, int64_t created,
                        int64_t expires) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO prospective_memories(trigger_type,trigger_value,action,contact_id,"
             "expires_at,fired,created_at) VALUES('keyword','%s','%s','" C1 "',%lld,0,%lld)",
             cue, action, (long long)expires, (long long)created);
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
}

static void seed_kw(sqlite3 *db, const char *cue, const char *action) {
    seed_kw_exp(db, cue, action, NOW - 86400, 0);
}

static void seed_time(sqlite3 *db, const char *action, int64_t due, const char *key) {
    HU_ASSERT_EQ(hu_prospective_repo_upsert_time(db, C1, strlen(C1), action, strlen(action), due,
                                                 HU_PROSPECTIVE_TIME_GRACE_S,
                                                 HU_PM_SOURCE_PROMISE_KEEPER, key, HU_PM_PENDING,
                                                 NOW - 86400, NULL),
                 HU_OK);
}

static int64_t q_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    int64_t v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static hu_prospective_turn_t turn_for(const char *inbound, int64_t now) {
    hu_prospective_turn_t t;
    memset(&t, 0, sizeof(t));
    t.contact = C1;
    t.contact_len = strlen(C1);
    t.inbound = inbound;
    t.inbound_len = inbound ? strlen(inbound) : 0;
    t.history = "them: going to that new taco place friday\nme: nice let me know\n";
    t.history_len = strlen(t.history);
    t.now = now;
    t.day_start = now - 3600;
    return t;
}

static void v2_clean_positive_surfaces_a_soft_directive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "taco place", "ask how the new taco place was");
    seed_kw(db, "tacos", "ask how the new taco place was");
    static const char *const r[] = {"fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_turn_t t = turn_for("ok the taco place was packed", NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(s.calls, (size_t)1); /* one intention, one Decide call */
    HU_ASSERT_EQ(c.candidates, (size_t)1);
    HU_ASSERT_EQ(c.fire, (size_t)1);
    HU_ASSERT_STR_EQ(d, "[PROSPECTIVE MEMORY: If it fits naturally, you could bring up: ask how "
                        "the new taco place was]");
    HU_ASSERT_EQ(dl, strlen(d));
    alloc.free(alloc.ctx, d, dl + 1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='surfaced' "
                           "AND fired=0 AND surfaced_at=1790000000"),
                 (int64_t)2); /* both keyword rows of the intention */
    HU_ASSERT_EQ(c.item_count, (size_t)1);
    HU_ASSERT_EQ(c.items[0].verdict, HU_PM_VERDICT_FIRE);
    HU_ASSERT_TRUE(c.items[0].judge_ok);
    HU_ASSERT_EQ(c.fire_action_count, (size_t)1);
    HU_ASSERT_STR_EQ(c.fire_actions[0], "ask how the new taco place was");
    mem.vtable->deinit(mem.ctx);
}

static void v2_already_resolved_is_done_and_never_refires(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "taco place", "ask how the new taco place was");
    static const char *const r[] = {"already_resolved"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_turn_t t = turn_for("taco place was great btw", NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(c.resolved, (size_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='done' AND "
                           "fired=1 AND outcome='suppressed'"),
                 (int64_t)1);
    /* the same cue later: nothing is judged again (TriggerBench "always remind") */
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(s.calls, (size_t)1);
    HU_ASSERT_EQ(c.candidates, (size_t)0);
    HU_ASSERT_NULL(d);
    mem.vtable->deinit(mem.ctx);
}

static void v2_cancel_retires_the_intention(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "taco place", "ask how the new taco place was");
    static const char *const r[] = {"cancel"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_turn_t t = turn_for("the taco place closed down, forget it", NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(c.cancel, (size_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='canceled' "
                           "AND fired=2"),
                 (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}

static void v2_not_now_parse_fail_and_judge_error_stay_pending_and_silent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw_exp(db, "alpha", "ask about alpha", NOW - 20, 0);
    seed_kw_exp(db, "beta", "ask about beta", NOW - 30, 0);
    static const char *const r[] = {"not_now", "lol yeah def"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 2);
    hu_prospective_turn_t t = turn_for("alpha and beta", NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(c.not_now, (size_t)1);
    HU_ASSERT_EQ(c.parse_fail, (size_t)1);
    s.err = HU_ERR_PROVIDER_RESPONSE; /* the model is down */
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(c.judge_err, (size_t)2);
    HU_ASSERT_EQ(c.items[0].judge_ok, false);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='pending' AND "
                           "fired=0 AND surfaced_at IS NULL"),
                 (int64_t)2);
    mem.vtable->deinit(mem.ctx);
}

static void v2_shadow_judges_but_writes_and_renders_nothing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "taco place", "ask how the new taco place was");
    seed_kw_exp(db, "stale", "an expired one", NOW - 90000, NOW - 5);
    static const char *const r[] = {"fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_turn_t t = turn_for("taco place tonight?", NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(
        hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, false, &c, &d, &dl), HU_OK);
    HU_ASSERT_EQ(s.calls, (size_t)1);
    HU_ASSERT_EQ(c.fire, (size_t)1);
    HU_ASSERT_EQ(c.expired, (size_t)1); /* counted, not written */
    HU_ASSERT_STR_EQ(c.fire_actions[0], "ask how the new taco place was");
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(dl, (size_t)0);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='pending' AND "
                           "fired=0 AND surfaced_at IS NULL"),
                 (int64_t)2);
    mem.vtable->deinit(mem.ctx);
}

static void v2_group_and_self_chat_are_never_judged(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "taco place", "ask how the new taco place was");
    seed_kw_exp(db, "stale", "an expired one", NOW - 90000, NOW - 5);
    static const char *const r[] = {"fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    hu_prospective_turn_t t = turn_for("taco place and stale", NOW);
    t.is_group = true;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(s.calls, (size_t)0);
    HU_ASSERT_EQ(c.candidates + c.expired, (size_t)0);
    t.is_group = false;
    t.is_self = true;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(s.calls, (size_t)0);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='pending'"),
                 (int64_t)2); /* nothing written, not even the expiry */
    mem.vtable->deinit(mem.ctx);
}

static void v2_judges_at_most_three_intentions_per_turn(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw_exp(db, "alpha", "ask about alpha", NOW - 10, 0);
    seed_kw_exp(db, "beta", "ask about beta", NOW - 20, 0);
    seed_kw_exp(db, "gamma", "ask about gamma", NOW - 30, 0);
    seed_kw_exp(db, "delta", "ask about delta", NOW - 40, 0);
    seed_kw_exp(db, "epsilon", "ask about epsilon", NOW - 50, 0);
    static const char *const r[] = {"fire", "fire", "fire", "fire", "fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 5);
    hu_prospective_turn_t t = turn_for("alpha beta gamma delta epsilon", NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(s.calls, (size_t)3);
    HU_ASSERT_EQ(c.candidates, (size_t)3);
    HU_ASSERT_EQ(c.fire, (size_t)3);
    HU_ASSERT_STR_CONTAINS(d, "ask about alpha | ask about beta | ask about gamma]");
    alloc.free(alloc.ctx, d, dl + 1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='surfaced'"),
                 (int64_t)3);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='pending'"),
                 (int64_t)2);
    mem.vtable->deinit(mem.ctx);
}

static void v2_time_due_item_renders_due_list_and_caps_one_per_day(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_time(db, "call about the lease", NOW - 3600, "commitment:1");
    seed_time(db, "return the drill", NOW - 1800, "followup:2");
    seed_time(db, "not due yet", NOW + 3600, "followup:3");
    static const char *const r[] = {"fire", "fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 2);
    hu_prospective_turn_t t = turn_for(NULL, NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_TIME, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(s.calls, (size_t)1);
    HU_ASSERT_STR_EQ(d, "- call about the lease\n"); /* oldest due first */
    HU_ASSERT_EQ(c.capped, (size_t)1);
    alloc.free(alloc.ctx, d, dl + 1);
    /* later the same day, nothing delivered: still capped — one per contact per day */
    HU_ASSERT_EQ(
        hu_prospective_v2_run(&alloc, db, HU_PM_CUE_TIME, &t, &j, true, &c, &d, &dl), HU_OK);
    HU_ASSERT_EQ(s.calls, (size_t)1);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(c.capped, (size_t)2);
    mem.vtable->deinit(mem.ctx);
}

static void v2_time_past_grace_expires_without_judging(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "INSERT INTO commitments(contact_id,description,who,deadline,status,"
                              "created_at) VALUES('" C1 "','call about the lease','me',1,"
                              "'pending',1)",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    seed_time(db, "call about the lease", NOW - 4 * 86400, "commitment:1");
    static const char *const r[] = {"fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_turn_t t = turn_for(NULL, NOW);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_TIME, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    HU_ASSERT_EQ(s.calls, (size_t)0);
    HU_ASSERT_EQ(c.expired, (size_t)1);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(q_int(db, "SELECT fired FROM prospective_memories WHERE cue_kind='time'"),
                 (int64_t)3);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM commitments WHERE status='expired'"),
                 (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}

static void v2_after_delivery_used_is_done_ignored_retries_then_expires(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "taco place", "ask how the new taco place was");
    seed_kw(db, "lasagna", "send the lasagna recipe");
    static const char *const r[] = {"fire", "fire", "fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 3);
    hu_prospective_counts_t c;
    hu_prospective_delivery_counts_t dc;
    char *d = NULL;
    size_t dl = 0;
    hu_prospective_turn_t t = turn_for("taco place, then lasagna night", NOW);
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    alloc.free(alloc.ctx, d, dl + 1);
    static const char reply[] = "wait how was the taco place??";
    HU_ASSERT_EQ(hu_prospective_v2_after_delivery(&alloc, db, HU_PM_CUE_KEYWORD, C1, strlen(C1),
                                                  reply, sizeof(reply) - 1, NOW + 60, &dc),
                 HU_OK);
    HU_ASSERT_EQ(dc.surfaced, (size_t)2);
    HU_ASSERT_EQ(dc.used, (size_t)1);
    HU_ASSERT_EQ(dc.ignored, (size_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE action LIKE 'ask how%' "
                           "AND status='done' AND fired=1 AND outcome='used'"),
                 (int64_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT attempts FROM prospective_memories WHERE action LIKE 'send%' "
                           "AND status='pending' AND outcome='ignored'"),
                 (int64_t)1);
    /* surfaced a second time and still not carried: expired, never a third time */
    hu_prospective_turn_t t2 = turn_for("lasagna tonight?", NOW + 3600);
    HU_ASSERT_EQ(
        hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t2, &j, true, &c, &d, &dl), HU_OK);
    HU_ASSERT_EQ(c.fire, (size_t)1);
    alloc.free(alloc.ctx, d, dl + 1);
    static const char reply2[] = "sounds good";
    HU_ASSERT_EQ(hu_prospective_v2_after_delivery(&alloc, db, HU_PM_CUE_KEYWORD, C1, strlen(C1),
                                                  reply2, sizeof(reply2) - 1, NOW + 3660, &dc),
                 HU_OK);
    HU_ASSERT_EQ(dc.expired, (size_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT fired FROM prospective_memories WHERE action LIKE 'send%'"),
                 (int64_t)3);
    mem.vtable->deinit(mem.ctx);
}

static void v2_undelivered_surfacing_is_reclaimed_as_an_attempt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "taco place", "ask how the new taco place was");
    static const char *const r[] = {"fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    hu_prospective_turn_t t = turn_for("taco place?", NOW);
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    alloc.free(alloc.ctx, d, dl + 1);
    /* the reply went out as a voice memo: no delivered-text hook ran */
    hu_prospective_turn_t t2 = turn_for("how are you", NOW + 600);
    HU_ASSERT_EQ(
        hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t2, &j, true, &c, &d, &dl), HU_OK);
    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(s.calls, (size_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT attempts FROM prospective_memories WHERE status='pending' AND "
                           "outcome='ignored'"),
                 (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}

static void v2_time_done_retires_ledger_twins(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "INSERT INTO commitments(contact_id,description,who,deadline,status,"
                              "created_at) VALUES('" C1 "','call about the lease','me',1,"
                              "'pending',1);INSERT INTO delayed_followups(contact_id,topic,"
                              "scheduled_at,sent) VALUES('" C1 "','call about the lease',1,0)",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    seed_time(db, "call about the lease", NOW - 3600, "commitment:1");
    static const char *const r[] = {"fire"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_counts_t c;
    hu_prospective_delivery_counts_t dc;
    char *d = NULL;
    size_t dl = 0;
    hu_prospective_turn_t t = turn_for(NULL, NOW);
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_TIME, &t, &j, true, &c, &d, &dl),
                 HU_OK);
    alloc.free(alloc.ctx, d, dl + 1);
    static const char sent[] = "hey did you ever call about the lease?";
    HU_ASSERT_EQ(hu_prospective_v2_after_delivery(&alloc, db, HU_PM_CUE_TIME, C1, strlen(C1), sent,
                                                  sizeof(sent) - 1, NOW + 60, &dc),
                 HU_OK);
    HU_ASSERT_EQ(dc.used, (size_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM commitments WHERE status='followed_up'"),
                 (int64_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT sent FROM delayed_followups"), (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}

static void v2_judge_sees_history_intention_and_cue(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    seed_kw(db, "taco place", "ask how the new taco place was");
    static const char *const r[] = {"not_now"};
    script_t s;
    hu_prospective_judge_t j = judge_of(&s, r, 1);
    hu_prospective_counts_t c;
    hu_prospective_turn_t t = turn_for("the taco place!!", NOW);
    HU_ASSERT_EQ(
        hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, &j, true, &c, NULL, NULL), HU_OK);
    HU_ASSERT_STR_CONTAINS(s.last_user, "them: going to that new taco place friday");
    HU_ASSERT_STR_CONTAINS(s.last_user, "intention: ask how the new taco place was");
    HU_ASSERT_STR_CONTAINS(s.last_user, "cue: they just mentioned \"taco place\"");
    mem.vtable->deinit(mem.ctx);
}

static void v2_rejects_invalid_arguments(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    hu_prospective_counts_t c;
    hu_prospective_turn_t t = turn_for("x", NOW);
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, NULL, HU_PM_CUE_KEYWORD, &t, NULL, true, &c, NULL,
                                       NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_AFTER_EVENT, &t, NULL, true, &c,
                                       NULL, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    t.contact_len = 0;
    HU_ASSERT_EQ(hu_prospective_v2_run(&alloc, db, HU_PM_CUE_KEYWORD, &t, NULL, true, &c, NULL,
                                       NULL),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_prospective_v2_after_delivery(&alloc, db, HU_PM_CUE_KEYWORD, NULL, 0, "x", 1,
                                                  NOW, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    mem.vtable->deinit(mem.ctx);
}

void run_prospective_v2_tests(void) {
    HU_TEST_SUITE("prospective v2");
    HU_RUN_TEST(v2_clean_positive_surfaces_a_soft_directive);
    HU_RUN_TEST(v2_already_resolved_is_done_and_never_refires);
    HU_RUN_TEST(v2_cancel_retires_the_intention);
    HU_RUN_TEST(v2_not_now_parse_fail_and_judge_error_stay_pending_and_silent);
    HU_RUN_TEST(v2_shadow_judges_but_writes_and_renders_nothing);
    HU_RUN_TEST(v2_group_and_self_chat_are_never_judged);
    HU_RUN_TEST(v2_judges_at_most_three_intentions_per_turn);
    HU_RUN_TEST(v2_time_due_item_renders_due_list_and_caps_one_per_day);
    HU_RUN_TEST(v2_time_past_grace_expires_without_judging);
    HU_RUN_TEST(v2_after_delivery_used_is_done_ignored_retries_then_expires);
    HU_RUN_TEST(v2_undelivered_surfacing_is_reclaimed_as_an_attempt);
    HU_RUN_TEST(v2_time_done_retires_ledger_twins);
    HU_RUN_TEST(v2_judge_sees_history_intention_and_cue);
    HU_RUN_TEST(v2_rejects_invalid_arguments);
}

#else

void run_prospective_v2_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
```

Register it: add `    tests/test_prospective_v2.c` after `tests/test_daemon_prospective_time.c` in `CMakeLists.txt`, and add `void run_prospective_v2_tests(void);` / `    run_prospective_v2_tests();` to `tests/test_main.c`.

- [ ] **Step 2: Run the build to verify it fails**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human_tests -j8 2>&1 | grep -m2 error`
Expected: FAIL with `'human/memory/prospective_v2.h' file not found`.

- [ ] **Step 3: Write the v2 header and implementation**

Create `include/human/memory/prospective_v2.h`:

```c
#ifndef HU_MEMORY_PROSPECTIVE_V2_H
#define HU_MEMORY_PROSPECTIVE_V2_H
/*
 * Prospective memory v2 — one pass of Filter -> Decide over the typed store
 * (spec docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4).
 *
 * The judge is injected: a scripted judge in tests and in the probe, the
 * agent's provider (thinking off) in the daemon. `apply=false` is SHADOW:
 * it reads and judges but writes nothing and renders nothing.
 */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/prospective_policy.h"
#include "human/memory/prospective_repo.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE

/* One short answer for (system, user). On HU_OK, *out is heap from `alloc`
 * (freed by the caller with *out_len + 1) or NULL for an empty answer. */
typedef hu_error_t (*hu_prospective_judge_fn)(void *ctx, hu_allocator_t *alloc,
                                              const char *system, size_t system_len,
                                              const char *user, size_t user_len, char **out,
                                              size_t *out_len);

typedef struct hu_prospective_judge {
    hu_prospective_judge_fn fn;
    void *ctx;
} hu_prospective_judge_t;

typedef struct hu_prospective_turn {
    const char *contact; /* the contact key rows are stored under */
    size_t contact_len;
    const char *inbound; /* the text keyword cues are matched in; NULL for time */
    size_t inbound_len;
    const char *history; /* "them: …\nme: …\n", oldest first, <= 20 lines */
    size_t history_len;
    bool is_group;
    bool is_self;
    int64_t now;
    int64_t day_start; /* local midnight: the per-day cap's day */
} hu_prospective_turn_t;

typedef struct hu_prospective_item_verdict {
    int64_t id;
    hu_prospective_verdict_t verdict;
    bool judge_ok; /* false: the judge call itself failed */
} hu_prospective_item_verdict_t;

typedef struct hu_prospective_counts {
    size_t candidates; /* eligible and judged (<= HU_PROSPECTIVE_JUDGE_CAP) */
    size_t fire, resolved, cancel, not_now, parse_fail, judge_err;
    size_t expired; /* past their window this pass */
    size_t capped;  /* time cues over the per-day cap */
    hu_prospective_item_verdict_t items[HU_PROSPECTIVE_JUDGE_CAP];
    size_t item_count;
    char fire_actions[HU_PROSPECTIVE_RENDER_CAP][256]; /* would-fire actions (SHADOW uptake) */
    size_t fire_action_count;
} hu_prospective_counts_t;

typedef struct hu_prospective_delivery_counts {
    size_t surfaced, used, ignored, expired;
} hu_prospective_delivery_counts_t;

/* One pass for `turn` over the contact's pending `kind` intentions (KEYWORD
 * or TIME):
 *   1. apply: settle intentions still `surfaced` from an earlier pass that
 *      no delivered reply confirmed (an attempt that did not land);
 *   2. Filter each pending intention (hu_prospective_filter) — EXPIRE retires
 *      it, SKIP/CAPPED leave it;
 *   3. Decide up to HU_PROSPECTIVE_JUDGE_CAP eligible intentions through
 *      `judge` (hu_prospective_decide) — RESOLVED -> done, CANCEL ->
 *      canceled (outcome 'suppressed'), FIRE -> surfaced if rendered;
 *   4. apply: render the fired ones (SOFT for keyword, DUE_LIST for time)
 *      into *directive (heap, free with *directive_len + 1; NULL if none).
 * A settled time intention also retires its ledger twins. Every decision is
 * counted in *counts. `apply=false` performs 2–3 read-only. */
hu_error_t hu_prospective_v2_run(hu_allocator_t *alloc, sqlite3 *db,
                                 hu_prospective_cue_kind_t kind, const hu_prospective_turn_t *turn,
                                 const hu_prospective_judge_t *judge, bool apply,
                                 hu_prospective_counts_t *counts, char **directive,
                                 size_t *directive_len);

/* Done only after evidence (§4.3): each intention of `kind` that is
 * `surfaced` for `contact` becomes DONE (outcome 'used') when `reply`
 * carries its key terms; otherwise attempts+1 and back to PENDING, or
 * EXPIRED at HU_PROSPECTIVE_MAX_ATTEMPTS (outcome 'ignored'). `reply` NULL
 * means nothing was delivered. `out` may be NULL. */
hu_error_t hu_prospective_v2_after_delivery(hu_allocator_t *alloc, sqlite3 *db,
                                            hu_prospective_cue_kind_t kind, const char *contact,
                                            size_t contact_len, const char *reply,
                                            size_t reply_len, int64_t now,
                                            hu_prospective_delivery_counts_t *out);

#endif /* HU_ENABLE_SQLITE */
#endif /* HU_MEMORY_PROSPECTIVE_V2_H */
```

Create `src/memory/prospective_v2.c`:

```c
/*
 * src/memory/prospective_v2.c — Filter -> Decide over the typed intention
 * store. Contract in include/human/memory/prospective_v2.h; spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.
 * All SQL is in src/memory/repos/prospective_repo_sqlite.c.
 */
#include "human/memory/prospective_v2.h"

#ifdef HU_ENABLE_SQLITE

#include "human/core/string.h"
#include <string.h>

#define PM_USER_CAP 6144

static bool pm_same_intention(const hu_prospective_item_t *a, const hu_prospective_item_t *b) {
    return strcmp(a->action, b->action) == 0 && strcmp(a->contact_id, b->contact_id) == 0;
}

static void pm_mark_handled(const hu_prospective_item_t *items, size_t n, bool *handled,
                            size_t i) {
    for (size_t k = 0; k < n; k++)
        if (!handled[k] && pm_same_intention(&items[k], &items[i]))
            handled[k] = true;
}

/* Retire an intention; a time intention also retires its ledger twins. */
static void pm_retire(sqlite3 *db, const hu_prospective_item_t *it, hu_prospective_status_t to,
                      hu_prospective_outcome_t outcome, int attempts, int64_t now) {
    (void)hu_prospective_repo_transition(db, it, to, outcome, attempts, now, NULL);
    if (it->cue_kind == HU_PM_CUE_TIME && to != HU_PM_PENDING)
        (void)hu_prospective_repo_sync_source(db, it, to, now);
}

static hu_prospective_verdict_t pm_judge(hu_allocator_t *alloc, const hu_prospective_judge_t *judge,
                                         const hu_prospective_turn_t *turn,
                                         const hu_prospective_item_t *it, bool *judge_ok) {
    *judge_ok = false;
    if (!judge || !judge->fn)
        return HU_PM_VERDICT_PARSE_FAIL;
    bool is_time = it->cue_kind == HU_PM_CUE_TIME;
    char user[PM_USER_CAP];
    size_t ul = hu_prospective_judge_user(user, sizeof(user), turn->history, turn->history_len,
                                          it->action, is_time ? NULL : it->trigger_value,
                                          it->cue_kind, is_time ? turn->now - it->due_at : 0);
    if (ul == 0)
        return HU_PM_VERDICT_PARSE_FAIL;
    size_t sl = 0;
    const char *sys = hu_prospective_judge_system(&sl);
    char *raw = NULL;
    size_t raw_len = 0;
    hu_error_t err = judge->fn(judge->ctx, alloc, sys, sl, user, ul, &raw, &raw_len);
    hu_prospective_verdict_t v = HU_PM_VERDICT_PARSE_FAIL;
    if (err == HU_OK) {
        *judge_ok = true;
        v = hu_prospective_parse_verdict(raw, raw_len);
    }
    if (raw)
        alloc->free(alloc->ctx, raw, raw_len + 1);
    return v;
}

static bool pm_keyword_cued(const hu_prospective_turn_t *turn, const hu_prospective_item_t *it) {
    return it->trigger_value[0] && turn->inbound && turn->inbound_len > 0 &&
           hu_str_contains_word_ci_n(turn->inbound, turn->inbound_len, it->trigger_value);
}

static void pm_count_verdict(hu_prospective_counts_t *c, const hu_prospective_item_t *it,
                             bool ok, hu_prospective_verdict_t v) {
    if (c->item_count < HU_PROSPECTIVE_JUDGE_CAP) {
        c->items[c->item_count].id = it->id;
        c->items[c->item_count].verdict = v;
        c->items[c->item_count].judge_ok = ok;
        c->item_count++;
    }
}

/* Render the fired intentions and, only for what reached the prompt, mark
 * them surfaced. Returns HU_OK with *directive NULL when nothing rendered. */
static hu_error_t pm_surface(hu_allocator_t *alloc, sqlite3 *db, hu_prospective_cue_kind_t kind,
                             const hu_prospective_item_t *const *fire, size_t fire_n, int64_t now,
                             char **directive, size_t *directive_len) {
    const char *acts[HU_PROSPECTIVE_RENDER_CAP];
    const char *cues[HU_PROSPECTIVE_RENDER_CAP];
    for (size_t k = 0; k < fire_n; k++) {
        acts[k] = fire[k]->action;
        cues[k] = fire[k]->trigger_value;
    }
    char buf[1024];
    size_t blen = 0;
    size_t r = hu_prospective_render(kind == HU_PM_CUE_TIME ? HU_PM_RENDER_DUE_LIST
                                                            : HU_PM_RENDER_SOFT,
                                     acts, cues, fire_n, buf, sizeof(buf), &blen);
    if (r == 0 || !directive)
        return HU_OK;
    char *d = (char *)alloc->alloc(alloc->ctx, blen + 1);
    if (!d)
        return HU_ERR_OUT_OF_MEMORY; /* nothing surfaced: they stay pending */
    memcpy(d, buf, blen + 1);
    for (size_t k = 0; k < r; k++)
        (void)hu_prospective_repo_transition(db, fire[k], HU_PM_SURFACED, HU_PM_OUTCOME_NONE,
                                             fire[k]->attempts, now, NULL);
    *directive = d;
    if (directive_len)
        *directive_len = blen;
    return HU_OK;
}

hu_error_t hu_prospective_v2_run(hu_allocator_t *alloc, sqlite3 *db,
                                 hu_prospective_cue_kind_t kind, const hu_prospective_turn_t *turn,
                                 const hu_prospective_judge_t *judge, bool apply,
                                 hu_prospective_counts_t *counts, char **directive,
                                 size_t *directive_len) {
    if (directive)
        *directive = NULL;
    if (directive_len)
        *directive_len = 0;
    if (!alloc || !db || !turn || !counts || !turn->contact || turn->contact_len == 0 ||
        (kind != HU_PM_CUE_KEYWORD && kind != HU_PM_CUE_TIME))
        return HU_ERR_INVALID_ARGUMENT;
    memset(counts, 0, sizeof(*counts));
    if (apply && !turn->is_group && !turn->is_self) {
        hu_error_t rerr = hu_prospective_v2_after_delivery(
            alloc, db, kind, turn->contact, turn->contact_len, NULL, 0, turn->now, NULL);
        if (rerr != HU_OK)
            return rerr;
    }
    hu_prospective_item_t *items = NULL;
    size_t n = 0;
    hu_error_t err = hu_prospective_repo_list(alloc, db, kind, HU_PM_PENDING, turn->contact,
                                              turn->contact_len, &items, &n);
    if (err != HU_OK || n == 0)
        return err;
    int64_t surfaced_today = 0;
    if (kind == HU_PM_CUE_TIME &&
        hu_prospective_repo_count_surfaced_since(db, kind, turn->contact, turn->contact_len,
                                                 turn->day_start, &surfaced_today) != HU_OK)
        surfaced_today = 1; /* cannot prove the day's slot is free: stay silent */
    bool *handled = (bool *)alloc->alloc(alloc->ctx, n * sizeof(bool));
    if (!handled) {
        hu_prospective_repo_free(alloc, items, n);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(handled, 0, n * sizeof(bool));
    const hu_prospective_item_t *fire[HU_PROSPECTIVE_RENDER_CAP];
    size_t fire_n = 0;
    for (size_t i = 0; i < n; i++) {
        const hu_prospective_item_t *it = &items[i];
        if (handled[i])
            continue;
        hu_prospective_filter_facts_t f;
        memset(&f, 0, sizeof(f));
        f.cue_kind = kind;
        f.status = it->status;
        f.is_group = turn->is_group;
        f.is_self = turn->is_self;
        f.keyword_in_text = kind == HU_PM_CUE_KEYWORD && pm_keyword_cued(turn, it);
        f.due_at = it->due_at;
        f.expires_at = it->expires_at;
        f.now = turn->now;
        f.grace_s = HU_PROSPECTIVE_TIME_GRACE_S;
        f.surfaced_today = (size_t)surfaced_today + (kind == HU_PM_CUE_TIME ? fire_n : 0);
        hu_prospective_filter_t fr = hu_prospective_filter(&f);
        if (fr == HU_PM_FILTER_SKIP)
            continue;
        pm_mark_handled(items, n, handled, i);
        if (fr == HU_PM_FILTER_EXPIRE) {
            counts->expired++;
            if (apply)
                pm_retire(db, it, HU_PM_EXPIRED, HU_PM_OUTCOME_NONE, it->attempts, turn->now);
            continue;
        }
        if (fr == HU_PM_FILTER_CAPPED) {
            counts->capped++;
            continue;
        }
        if (counts->candidates >= HU_PROSPECTIVE_JUDGE_CAP)
            continue; /* bounded model calls per turn; the rest stay pending */
        counts->candidates++;
        bool ok = false;
        hu_prospective_verdict_t v = pm_judge(alloc, judge, turn, it, &ok);
        pm_count_verdict(counts, it, ok, v);
        switch (hu_prospective_decide(ok, v)) {
        case HU_PM_ACT_SURFACE:
            counts->fire++;
            if (fire_n < HU_PROSPECTIVE_RENDER_CAP)
                fire[fire_n++] = it;
            break;
        case HU_PM_ACT_MARK_DONE:
            counts->resolved++;
            if (apply)
                pm_retire(db, it, HU_PM_DONE, HU_PM_OUTCOME_SUPPRESSED, it->attempts, turn->now);
            break;
        case HU_PM_ACT_MARK_CANCELED:
            counts->cancel++;
            if (apply)
                pm_retire(db, it, HU_PM_CANCELED, HU_PM_OUTCOME_SUPPRESSED, it->attempts,
                          turn->now);
            break;
        default:
            if (!ok)
                counts->judge_err++;
            else if (v == HU_PM_VERDICT_PARSE_FAIL)
                counts->parse_fail++;
            else
                counts->not_now++;
            break;
        }
    }
    for (size_t k = 0; k < fire_n; k++) {
        size_t al = strlen(fire[k]->action);
        if (al >= sizeof(counts->fire_actions[0]))
            al = sizeof(counts->fire_actions[0]) - 1;
        memcpy(counts->fire_actions[k], fire[k]->action, al);
        counts->fire_actions[k][al] = '\0';
    }
    counts->fire_action_count = fire_n;
    if (apply && fire_n > 0)
        err = pm_surface(alloc, db, kind, fire, fire_n, turn->now, directive, directive_len);
    alloc->free(alloc->ctx, handled, n * sizeof(bool));
    hu_prospective_repo_free(alloc, items, n);
    return err;
}

hu_error_t hu_prospective_v2_after_delivery(hu_allocator_t *alloc, sqlite3 *db,
                                            hu_prospective_cue_kind_t kind, const char *contact,
                                            size_t contact_len, const char *reply,
                                            size_t reply_len, int64_t now,
                                            hu_prospective_delivery_counts_t *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!alloc || !db || !contact || contact_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    hu_prospective_item_t *items = NULL;
    size_t n = 0;
    hu_error_t err = hu_prospective_repo_list(alloc, db, kind, HU_PM_SURFACED, contact,
                                              contact_len, &items, &n);
    if (err != HU_OK || n == 0)
        return err;
    for (size_t i = 0; i < n; i++) {
        const hu_prospective_item_t *it = &items[i];
        bool dup = false;
        for (size_t k = 0; k < i && !dup; k++)
            dup = pm_same_intention(&items[k], it);
        if (dup)
            continue; /* the intention moved with its first row */
        bool used = hu_prospective_reply_uses_action(it->action, reply, reply_len);
        hu_prospective_status_t to =
            hu_prospective_after_delivery_status(used, it->attempts, HU_PROSPECTIVE_MAX_ATTEMPTS);
        pm_retire(db, it, to, used ? HU_PM_OUTCOME_USED : HU_PM_OUTCOME_IGNORED,
                  used ? it->attempts : it->attempts + 1, now);
        if (out) {
            out->surfaced++;
            if (used)
                out->used++;
            else
                out->ignored++;
            if (to == HU_PM_EXPIRED)
                out->expired++;
        }
    }
    hu_prospective_repo_free(alloc, items, n);
    return HU_OK;
}

#endif /* HU_ENABLE_SQLITE */
```

In `CMakeLists.txt`, in the SQLite source block, after `        src/memory/prospective.c`:

```cmake
        src/memory/prospective_v2.c
```

- [ ] **Step 4: Run the v2 tests**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human_tests -j8 && cd "$W" && ./build/human_tests --suite="prospective v2"`
Expected: PASS, 14/14.

- [ ] **Step 5: Write the failing CLI tests (the v2 module's first product caller)**

Create `tests/test_cli_prospective.c`:

```c
/* tests/test_cli_prospective.c
 *
 * `human prospective init|probe|backfill` (src/app/cli_prospective.c): the
 * probe the harness drives (spec 2026-09-30 §4.5). Parsing is pure; the run
 * path is exercised against an in-memory store with a scripted judge, and its
 * output lines are the contract scripts/pm_bench_local.py parses. */
#include "test_framework.h"

#include "human/cli_prospective.h"
#include <stdio.h>
#include <string.h>

static void cli_prospective_parse_accepts_the_documented_forms(void) {
    hu_cli_prospective_args_t a;
    char *in[] = {"human", "prospective", "probe", "--full", "--db", "/tmp/pm.db", "--contact",
                  "+15550000001", "--now", "1790000000", "--inbound", "taco place", "--judge",
                  "fire", "--history", "/tmp/h.txt"};
    HU_ASSERT_TRUE(hu_cli_prospective_parse(16, in, &a));
    HU_ASSERT_EQ(a.op, HU_CLI_PM_INBOUND);
    HU_ASSERT_TRUE(a.full);
    HU_ASSERT_STR_EQ(a.db, "/tmp/pm.db");
    HU_ASSERT_STR_EQ(a.contact, "+15550000001");
    HU_ASSERT_STR_EQ(a.text, "taco place");
    HU_ASSERT_STR_EQ(a.judge, "fire");
    HU_ASSERT_STR_EQ(a.history_path, "/tmp/h.txt");
    HU_ASSERT_EQ(a.now, 1790000000LL);
    char *tick[] = {"human", "prospective", "probe", "--db", "d", "--contact", "c", "--tick",
                    "--shadow", "--group", "--self"};
    HU_ASSERT_TRUE(hu_cli_prospective_parse(11, tick, &a));
    HU_ASSERT_EQ(a.op, HU_CLI_PM_TICK);
    HU_ASSERT_TRUE(a.shadow && a.group && a.self);
    HU_ASSERT_STR_EQ(a.judge, "not_now"); /* default: silence */
    char *dl[] = {"human", "prospective", "probe", "--db", "d", "--contact", "c", "--deliver",
                  "how was it"};
    HU_ASSERT_TRUE(hu_cli_prospective_parse(9, dl, &a));
    HU_ASSERT_EQ(a.op, HU_CLI_PM_DELIVER);
    char *init[] = {"human", "prospective", "init", "--db", "d"};
    HU_ASSERT_TRUE(hu_cli_prospective_parse(5, init, &a));
    HU_ASSERT_EQ(a.op, HU_CLI_PM_INIT);
}

static void cli_prospective_parse_refuses_unsafe_or_ambiguous_input(void) {
    hu_cli_prospective_args_t a;
    char *no_db[] = {"human", "prospective", "probe", "--contact", "c", "--tick"};
    HU_ASSERT_FALSE(hu_cli_prospective_parse(6, no_db, &a)); /* never a default DB */
    char *two_ops[] = {"human", "prospective", "probe", "--db", "d", "--contact", "c", "--tick",
                       "--inbound", "x"};
    HU_ASSERT_FALSE(hu_cli_prospective_parse(10, two_ops, &a));
    char *no_op[] = {"human", "prospective", "probe", "--db", "d", "--contact", "c"};
    HU_ASSERT_FALSE(hu_cli_prospective_parse(7, no_op, &a));
    char *bad_judge[] = {"human", "prospective", "probe", "--db", "d", "--contact", "c", "--tick",
                         "--judge", "yes"};
    HU_ASSERT_FALSE(hu_cli_prospective_parse(10, bad_judge, &a));
    char *bad_now[] = {"human", "prospective", "probe", "--db", "d", "--contact", "c", "--tick",
                       "--now", "12x"};
    HU_ASSERT_FALSE(hu_cli_prospective_parse(10, bad_now, &a));
    char *no_contact[] = {"human", "prospective", "probe", "--db", "d", "--tick"};
    HU_ASSERT_FALSE(hu_cli_prospective_parse(6, no_contact, &a));
    char *unknown[] = {"human", "prospective", "frobnicate", "--db", "d"};
    HU_ASSERT_FALSE(hu_cli_prospective_parse(5, unknown, &a));
    char *dangling[] = {"human", "prospective", "probe", "--db"};
    HU_ASSERT_FALSE(hu_cli_prospective_parse(4, dangling, &a));
}

#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include <sqlite3.h>

static void read_all(FILE *f, char *buf, size_t cap) {
    rewind(f);
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
}

static hu_error_t fire_judge(void *ctx, hu_allocator_t *alloc, const char *system,
                             size_t system_len, const char *user, size_t user_len, char **out,
                             size_t *out_len) {
    (void)ctx;
    (void)system;
    (void)system_len;
    (void)user;
    (void)user_len;
    char *b = (char *)alloc->alloc(alloc->ctx, 5);
    memcpy(b, "fire", 5);
    *out = b;
    *out_len = 4;
    return HU_OK;
}

static void cli_prospective_run_prints_the_probe_contract(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "INSERT INTO prospective_memories(trigger_type,trigger_value,action,"
                              "contact_id,expires_at,fired,created_at) VALUES('keyword',"
                              "'taco place','ask how the new taco place was','+15550000001',0,0,"
                              "100)",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    hu_prospective_judge_t j = {.fn = fire_judge, .ctx = NULL};
    hu_cli_prospective_args_t a;
    char *in[] = {"human", "prospective", "probe", "--full", "--db", ":memory:", "--contact",
                  "+15550000001", "--now", "1790000000", "--inbound", "the taco place!"};
    HU_ASSERT_TRUE(hu_cli_prospective_parse(12, in, &a));
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    HU_ASSERT_EQ(hu_cli_prospective_run(&alloc, &mem, &a, NULL, 0, &j, f), HU_OK);
    char buf[1024];
    read_all(f, buf, sizeof(buf));
    fclose(f);
    HU_ASSERT_STR_CONTAINS(buf, "candidates=1 fire=1 resolved=0 cancel=0 not_now=0 "
                                "parse_fail=0 judge_err=0 expired=0 capped=0 bytes=");
    HU_ASSERT_STR_CONTAINS(buf, "item id=1 verdict=fire\n");
    HU_ASSERT_STR_CONTAINS(buf, "[PROSPECTIVE MEMORY: If it fits naturally, you could bring up: "
                                "ask how the new taco place was]\n");

    char *dl[] = {"human", "prospective", "probe", "--db", ":memory:", "--contact",
                  "+15550000001", "--now", "1790000100", "--deliver", "how was the taco place"};
    HU_ASSERT_TRUE(hu_cli_prospective_parse(11, dl, &a));
    f = tmpfile();
    HU_ASSERT_EQ(hu_cli_prospective_run(&alloc, &mem, &a, NULL, 0, &j, f), HU_OK);
    read_all(f, buf, sizeof(buf));
    fclose(f);
    HU_ASSERT_STR_EQ(buf, "surfaced=1 used=1 ignored=0 expired=0\n");

    char *init[] = {"human", "prospective", "init", "--db", ":memory:"};
    HU_ASSERT_TRUE(hu_cli_prospective_parse(5, init, &a));
    f = tmpfile();
    HU_ASSERT_EQ(hu_cli_prospective_run(&alloc, &mem, &a, NULL, 0, &j, f), HU_OK);
    read_all(f, buf, sizeof(buf));
    fclose(f);
    HU_ASSERT_STR_EQ(buf, "ok\n");
    mem.vtable->deinit(mem.ctx);
}
#endif /* HU_ENABLE_SQLITE */

void run_cli_prospective_tests(void) {
    HU_TEST_SUITE("cli prospective");
    HU_RUN_TEST(cli_prospective_parse_accepts_the_documented_forms);
    HU_RUN_TEST(cli_prospective_parse_refuses_unsafe_or_ambiguous_input);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(cli_prospective_run_prints_the_probe_contract);
#endif
}
```

Register it: add `    tests/test_cli_prospective.c` after `tests/test_prospective_v2.c` in `CMakeLists.txt`, and add `void run_cli_prospective_tests(void);` / `    run_cli_prospective_tests();` to `tests/test_main.c`.

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human_tests -j8 2>&1 | grep -m2 error`
Expected: FAIL with `'human/cli_prospective.h' file not found`.

- [ ] **Step 6: Write the CLI**

Create `include/human/cli_prospective.h`:

```c
#ifndef HU_CLI_PROSPECTIVE_H
#define HU_CLI_PROSPECTIVE_H
/* `human prospective` — the probe and one-time backfill for prospective
 * memory v2 (docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md
 * §4.5, rollout step 2). src/app/cli_prospective.c. */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef enum hu_cli_prospective_op {
    HU_CLI_PM_NONE = 0,
    HU_CLI_PM_INIT,     /* open (create + migrate) --db, print "ok" */
    HU_CLI_PM_INBOUND,  /* probe --inbound TEXT: keyword pass */
    HU_CLI_PM_TICK,     /* probe --tick: time pass */
    HU_CLI_PM_DELIVER,  /* probe --deliver TEXT: after-delivery evidence */
    HU_CLI_PM_BACKFILL, /* backfill [--write] */
} hu_cli_prospective_op_t;

typedef struct hu_cli_prospective_args {
    hu_cli_prospective_op_t op;
    const char *db;           /* required: the probe never defaults to ~/.human */
    const char *contact;      /* required for probe */
    const char *text;         /* --inbound / --deliver */
    const char *history_path; /* --history FILE, "them: …\nme: …" lines */
    const char *judge;        /* a verdict word or "model"; default "not_now" */
    long long now;            /* --now EPOCH; 0 = time(NULL) */
    bool full, shadow, group, self, write;
} hu_cli_prospective_args_t;

/* argv[0]="human", argv[1]="prospective", argv[2]=init|probe|backfill.
 * Rejects: no --db, a probe without exactly one of --inbound/--tick/
 * --deliver or without --contact, an unknown flag, a flag missing its value,
 * a --judge that is neither a verdict word nor "model", a bad --now. */
bool hu_cli_prospective_parse(int argc, char **argv, hu_cli_prospective_args_t *out);

#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/prospective_v2.h"
/* Runs a parsed probe/init/backfill against an open memory and writes the
 * output contract (see the plan's Task 6 Interfaces) to `out`. */
hu_error_t hu_cli_prospective_run(hu_allocator_t *alloc, hu_memory_t *mem,
                                  const hu_cli_prospective_args_t *a, const char *history,
                                  size_t history_len, const hu_prospective_judge_t *judge,
                                  FILE *out);
#endif

#endif /* HU_CLI_PROSPECTIVE_H */
```

Create `src/app/cli_prospective.c`:

```c
/*
 * src/app/cli_prospective.c — `human prospective init|probe|backfill`.
 *
 *   human prospective init --db PATH
 *   human prospective probe [--full] [--shadow] --db PATH --contact ID [--now EPOCH]
 *         (--inbound TEXT | --tick | --deliver TEXT) [--history FILE]
 *         [--judge fire|already_resolved|cancel|not_now|model] [--group] [--self]
 *   human prospective backfill --db PATH [--write] [--now EPOCH]
 *
 * The probe runs the same functions the daemon runs (hu_prospective_v2_run /
 * _after_delivery) against the database named by --db, which is required:
 * it never opens ~/.human/memory.db by default. scripts/pm_bench_local.py
 * drives it with a fixture DB and a scripted clock.
 */
#include "human/cli_prospective.h"

#include "human/cli_commands.h"
#include "human/memory/prospective_policy.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PM_HISTORY_CAP 16384

bool hu_cli_prospective_parse(int argc, char **argv, hu_cli_prospective_args_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    out->judge = "not_now";
    if (!argv || argc < 3 || !argv[2])
        return false;
    bool probe = strcmp(argv[2], "probe") == 0;
    if (strcmp(argv[2], "init") == 0)
        out->op = HU_CLI_PM_INIT;
    else if (strcmp(argv[2], "backfill") == 0)
        out->op = HU_CLI_PM_BACKFILL;
    else if (!probe)
        return false;
    for (int i = 3; i < argc; i++) {
        const char *k = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(k, "--full") == 0) {
            out->full = true;
        } else if (strcmp(k, "--shadow") == 0) {
            out->shadow = true;
        } else if (strcmp(k, "--group") == 0) {
            out->group = true;
        } else if (strcmp(k, "--self") == 0) {
            out->self = true;
        } else if (strcmp(k, "--write") == 0) {
            out->write = true;
        } else if (strcmp(k, "--tick") == 0) {
            if (out->op != HU_CLI_PM_NONE)
                return false;
            out->op = HU_CLI_PM_TICK;
        } else if (!v) {
            return false;
        } else if (strcmp(k, "--db") == 0) {
            out->db = argv[++i];
        } else if (strcmp(k, "--contact") == 0) {
            out->contact = argv[++i];
        } else if (strcmp(k, "--history") == 0) {
            out->history_path = argv[++i];
        } else if (strcmp(k, "--judge") == 0) {
            out->judge = argv[++i];
        } else if (strcmp(k, "--now") == 0) {
            char *end = NULL;
            out->now = strtoll(v, &end, 10);
            if (!end || *end || out->now <= 0)
                return false;
            i++;
        } else if (strcmp(k, "--inbound") == 0 || strcmp(k, "--deliver") == 0) {
            if (out->op != HU_CLI_PM_NONE)
                return false;
            out->op = k[2] == 'i' ? HU_CLI_PM_INBOUND : HU_CLI_PM_DELIVER;
            out->text = argv[++i];
        } else {
            return false;
        }
    }
    if (!out->db || !out->db[0])
        return false;
    if (!probe)
        return out->op == HU_CLI_PM_INIT || out->op == HU_CLI_PM_BACKFILL;
    if (out->op == HU_CLI_PM_NONE || !out->contact || !out->contact[0])
        return false;
    return strcmp(out->judge, "model") == 0 ||
           hu_prospective_parse_verdict(out->judge, strlen(out->judge)) !=
               HU_PM_VERDICT_PARSE_FAIL;
}

#ifdef HU_ENABLE_SQLITE

static void pm_emit_counts(FILE *out, const hu_prospective_counts_t *c, size_t bytes) {
    fprintf(out,
            "candidates=%zu fire=%zu resolved=%zu cancel=%zu not_now=%zu parse_fail=%zu "
            "judge_err=%zu expired=%zu capped=%zu bytes=%zu\n",
            c->candidates, c->fire, c->resolved, c->cancel, c->not_now, c->parse_fail,
            c->judge_err, c->expired, c->capped, bytes);
}

static hu_error_t pm_run_deliver(hu_allocator_t *alloc, sqlite3 *db,
                                 const hu_cli_prospective_args_t *a, int64_t now, FILE *out) {
    size_t cl = strlen(a->contact);
    size_t tl = a->text ? strlen(a->text) : 0;
    hu_prospective_delivery_counts_t k;
    hu_prospective_delivery_counts_t t;
    hu_error_t e = hu_prospective_v2_after_delivery(alloc, db, HU_PM_CUE_KEYWORD, a->contact, cl,
                                                    a->text, tl, now, &k);
    if (e == HU_OK)
        e = hu_prospective_v2_after_delivery(alloc, db, HU_PM_CUE_TIME, a->contact, cl, a->text,
                                             tl, now, &t);
    if (e != HU_OK)
        return e;
    fprintf(out, "surfaced=%zu used=%zu ignored=%zu expired=%zu\n", k.surfaced + t.surfaced,
            k.used + t.used, k.ignored + t.ignored, k.expired + t.expired);
    return HU_OK;
}

hu_error_t hu_cli_prospective_run(hu_allocator_t *alloc, hu_memory_t *mem,
                                  const hu_cli_prospective_args_t *a, const char *history,
                                  size_t history_len, const hu_prospective_judge_t *judge,
                                  FILE *out) {
    if (!alloc || !mem || !a || !out)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3 *db = hu_sqlite_memory_get_db(mem);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;
    if (a->op == HU_CLI_PM_INIT) { /* opening the store created and migrated it */
        fprintf(out, "ok\n");
        return HU_OK;
    }
    int64_t now = a->now > 0 ? (int64_t)a->now : (int64_t)time(NULL);
    if (a->op == HU_CLI_PM_DELIVER)
        return pm_run_deliver(alloc, db, a, now, out);
    if (a->op != HU_CLI_PM_INBOUND && a->op != HU_CLI_PM_TICK)
        return HU_ERR_NOT_SUPPORTED;
    hu_prospective_turn_t turn;
    memset(&turn, 0, sizeof(turn));
    turn.contact = a->contact;
    turn.contact_len = strlen(a->contact);
    if (a->op == HU_CLI_PM_INBOUND && a->text) {
        turn.inbound = a->text;
        turn.inbound_len = strlen(a->text);
    }
    turn.history = history;
    turn.history_len = history ? history_len : 0;
    turn.is_group = a->group;
    turn.is_self = a->self;
    turn.now = now;
    turn.day_start = hu_prospective_local_day_start(now);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    hu_error_t e = hu_prospective_v2_run(alloc, db,
                                         a->op == HU_CLI_PM_TICK ? HU_PM_CUE_TIME
                                                                 : HU_PM_CUE_KEYWORD,
                                         &turn, judge, !a->shadow, &c, &d, &dl);
    if (e != HU_OK)
        return e;
    pm_emit_counts(out, &c, dl);
    if (a->full) {
        for (size_t i = 0; i < c.item_count; i++)
            fprintf(out, "item id=%lld verdict=%s\n", (long long)c.items[i].id,
                    c.items[i].judge_ok ? hu_prospective_verdict_str(c.items[i].verdict)
                                        : "judge_err");
        if (d && dl > 0)
            fprintf(out, "%.*s\n", (int)dl, d);
    }
    if (d)
        alloc->free(alloc->ctx, d, dl + 1);
    return HU_OK;
}

/* A fixed answer — the scripted judge the probe offers besides the model. */
static hu_error_t pm_const_judge(void *ctx, hu_allocator_t *alloc, const char *system,
                                 size_t system_len, const char *user, size_t user_len, char **out,
                                 size_t *out_len) {
    (void)system;
    (void)system_len;
    (void)user;
    (void)user_len;
    const char *word = (const char *)ctx;
    size_t n = strlen(word);
    char *b = (char *)alloc->alloc(alloc->ctx, n + 1);
    if (!b)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(b, word, n + 1);
    *out = b;
    *out_len = n;
    return HU_OK;
}

static char *pm_read_file(hu_allocator_t *alloc, const char *path, size_t *len) {
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = (char *)alloc->alloc(alloc->ctx, PM_HISTORY_CAP);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, PM_HISTORY_CAP - 1, f);
    fclose(f);
    buf[n] = '\0';
    *len = n;
    return buf;
}

static void pm_usage(void) {
    fprintf(stderr,
            "Usage: human prospective init --db PATH\n"
            "       human prospective probe [--full] [--shadow] --db PATH --contact ID "
            "[--now EPOCH]\n"
            "             (--inbound TEXT | --tick | --deliver TEXT) [--history FILE]\n"
            "             [--judge fire|already_resolved|cancel|not_now|model] [--group] "
            "[--self]\n"
            "       human prospective backfill --db PATH [--write] [--now EPOCH]\n");
}

hu_error_t cmd_prospective(hu_allocator_t *alloc, int argc, char **argv) {
    hu_cli_prospective_args_t a;
    if (!hu_cli_prospective_parse(argc, argv, &a)) {
        pm_usage();
        return HU_ERR_INVALID_ARGUMENT;
    }
    size_t hist_len = 0;
    char *hist = NULL;
    if (a.history_path) {
        hist = pm_read_file(alloc, a.history_path, &hist_len);
        if (!hist) {
            fprintf(stderr, "prospective: cannot read %s\n", a.history_path);
            return HU_ERR_IO;
        }
    }
    hu_memory_t mem = hu_sqlite_memory_create(alloc, a.db);
    if (!mem.vtable) {
        fprintf(stderr, "prospective: cannot open %s\n", a.db);
        if (hist)
            alloc->free(alloc->ctx, hist, PM_HISTORY_CAP);
        return HU_ERR_IO;
    }
    char word[24];
    snprintf(word, sizeof(word), "%s", a.judge);
    hu_prospective_judge_t judge = {.fn = pm_const_judge, .ctx = word};
    hu_error_t err;
    if (strcmp(a.judge, "model") == 0) {
        fprintf(stderr, "prospective: --judge model needs the provider adapter (Task 7)\n");
        err = HU_ERR_NOT_SUPPORTED;
    } else {
        err = hu_cli_prospective_run(alloc, &mem, &a, hist, hist_len, &judge, stdout);
    }
    if (err != HU_OK)
        fprintf(stderr, "prospective: %s\n", hu_error_string(err));
    mem.vtable->deinit(mem.ctx);
    if (hist)
        alloc->free(alloc->ctx, hist, PM_HISTORY_CAP);
    return err;
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t cmd_prospective(hu_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    (void)argc;
    (void)argv;
    fprintf(stderr, "prospective: this build has no SQLite memory\n");
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_ENABLE_SQLITE */
```

In `include/human/cli_commands.h`, after the `cmd_memory` declaration:

```c
/* human prospective init|probe|backfill (src/app/cli_prospective.c). */
hu_error_t cmd_prospective(hu_allocator_t *alloc, int argc, char **argv);
```

In `src/app/main.c`, add a row after the `memory` row of `commands[]`:

```c
    {"prospective", "Prospective memory v2 probe and backfill (needs --db)", cmd_prospective,
     HU_CLI_HELP_BARE},
```

In `CMakeLists.txt`, after `    src/app/cli_ctl.c`:

```cmake
    src/app/cli_prospective.c
```

(`hu_error_string` comes from `human/core/error.h`, which `cli_prospective.h` already includes.)

- [ ] **Step 7: Run the tests and the binary**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human human_tests -j8 && cd "$W" && ./build/human_tests --suite=prospective && ./build/human_tests --suite="cli prospective" && D=$(mktemp -d) && ./build/human prospective init --db "$D/pm.db" && ./build/human prospective probe --db "$D/pm.db" --contact +15550000001 --tick; echo "exit=$?"; rm -rf "$D"`
Expected: PASS for every prospective suite and `cli prospective` (3/3). The binary prints `ok`, then `candidates=0 fire=0 resolved=0 cancel=0 not_now=0 parse_fail=0 judge_err=0 expired=0 capped=0 bytes=0`, then `exit=0`. The scratch DB is under `mktemp -d`, never `~/.human`.

- [ ] **Step 8: Full suite, ratchets, commit**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cd "$W" && ./build/human_tests 2>&1 | grep -E 'Results:' && bash scripts/check-sqlite-includer-ratchet.sh && bash scripts/check-clone-ratchet.sh && bash scripts/check-dead-strip-ratchet.sh && bash scripts/check-test-source-gate-symmetry.sh && bash scripts/check-state-path-literals.sh`
Expected: 0 failed, and every gate passes. `prospective_v2.c` and `cli_prospective.c` are loaded through `main.c`'s command table, so dead-strip A is unchanged.

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2
git -C "$W" add include/human/memory/prospective_v2.h src/memory/prospective_v2.c \
  include/human/cli_prospective.h src/app/cli_prospective.c include/human/cli_commands.h \
  src/app/main.c CMakeLists.txt tests/test_main.c tests/test_prospective_v2.c \
  tests/test_cli_prospective.c
git -C "$W" commit -m "feat(prospective): Filter -> Decide pass over the typed store, and the probe

hu_prospective_v2_run judges at most three cued/due intentions per turn
through an injected judge, settles resolved/canceled ones, surfaces fired
ones with the soft directive, and fails toward silence on parse or model
errors; _after_delivery marks done only when the delivered reply carries the
action. \`human prospective init|probe\` runs the same functions against a
--db fixture (never ~/.human by default) for the harness.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: `HU_PROSPECTIVE` in the reactive path (OFF byte-identical, SHADOW logs, LIVE replaces)

**Files:**
- Create: `include/human/daemon/prospective.h`, `src/daemon/daemon_prospective.c`
- Modify: `src/daemon/daemon_reactive_prompt.c` (include block ~:18-35; site 1 ~:544-553; site 2 ~:1222-1255)
- Modify: `include/human/daemon/reactive_turn.h` (`hu_reactive_turn_ctx_t` inputs)
- Modify: `src/daemon.c` (one line after `rt.llm_decides = llm_decides;` ~:4194)
- Modify: `src/daemon/daemon_message_router.c` (include block; top of `hu_daemon_record_delivered_reply` ~:607), `include/human/daemon/message_router.h` (its comment)
- Modify: `src/app/cli_prospective.c` (`--judge model`)
- Modify: `CMakeLists.txt` (after `    src/daemon/daemon_prospective_time.c`; test list after `tests/test_cli_prospective.c`), `tests/test_main.c`
- Test: `tests/test_daemon_prospective.c`

**Interfaces:**
- Consumes: `hu_prospective_v2_run`, `hu_prospective_v2_after_delivery`, `hu_prospective_directive_build` (legacy), `hu_prospective_gate_mode`, `hu_prospective_gate_banner`, `hu_prospective_local_day_start`, `hu_prospective_reply_uses_action`, `hu_prospective_verdict_str`, `hu_provider_chat_oneshot`, `hu_share_is_owner`, `hu_sqlite_memory_get_db`, `hu_buf_appendf`.
- Produces (Task 9 uses the first four):
  - `typedef struct hu_daemon_prospective_judge_ctx { struct hu_provider *provider; const char *model; size_t model_len; } hu_daemon_prospective_judge_ctx_t;`
  - `hu_error_t hu_daemon_prospective_provider_judge(void *ctx, hu_allocator_t *alloc, const char *system, size_t system_len, const char *user, size_t user_len, char **out, size_t *out_len);`
  - `size_t hu_daemon_prospective_history_render(const hu_channel_history_entry_t *entries, size_t n, char *buf, size_t cap);`
  - `void hu_daemon_prospective_log_counts(const char *tag, const hu_prospective_counts_t *c);`
  - `char *hu_daemon_prospective_directive(hu_allocator_t *alloc, sqlite3 *db, hu_gate_mode_t mode, const hu_prospective_turn_t *turn, const hu_prospective_judge_t *judge, size_t *out_len);`
  - `void hu_daemon_prospective_on_delivered(hu_allocator_t *alloc, sqlite3 *db, hu_gate_mode_t mode, const char *contact, size_t contact_len, const char *reply, size_t reply_len, int64_t now);`
  - `char *hu_daemon_prospective_reactive(hu_allocator_t *alloc, struct hu_agent *agent, sqlite3 *db, const char *contact, size_t contact_len, const char *text, size_t text_len, const hu_channel_history_entry_t *history, size_t history_n, bool is_group, size_t *out_len);`
  - `void hu_daemon_prospective_delivered(struct hu_agent *agent, const char *target, size_t target_len, const char *text, size_t text_len);`
  - `hu_reactive_turn_ctx_t.is_group` (bool).
  - Log lines, which Task 12 parses: `prospective <tag>: candidates=%zu fire=%zu resolved=%zu cancel=%zu not_now=%zu parse_fail=%zu judge_err=%zu expired=%zu capped=%zu` (emitted only when candidates+expired+capped > 0), `prospective <tag> item: id=%lld verdict=%s`, `prospective shadow uptake: would_fire=%zu used=%zu`, and `prospective live delivered: surfaced=%zu used=%zu ignored=%zu expired=%zu`. The tag is `shadow`/`live` here and `time shadow`/`time live` in Task 9. They go through `hu_log_info("prospective", NULL, …)`, which writes a dated line to the service log.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_daemon_prospective.c`:

```c
/* tests/test_daemon_prospective.c
 *
 * HU_PROSPECTIVE in the reactive path (src/daemon/daemon_prospective.c; spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.4).
 * OFF is byte-identical to the legacy directive and never calls the judge;
 * SHADOW returns the legacy directive and changes nothing the legacy path
 * would not; LIVE returns the soft directive and leaves `fired` alone until a
 * delivered reply carries the action; a judge failure is silence. Twin
 * in-memory stores prove the byte-identity. */
#include "test_framework.h"

#include "human/channel.h"
#include "human/daemon/prospective.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* History rendering is pure (no SQLite). */
static void history_render_keeps_last_twenty_lines_oldest_first(void) {
    hu_channel_history_entry_t e[25];
    memset(e, 0, sizeof(e));
    for (int i = 0; i < 25; i++) {
        e[i].from_me = (i % 2) == 1;
        snprintf(e[i].text, sizeof(e[i].text), "line %02d", i);
    }
    char buf[2048];
    size_t n = hu_daemon_prospective_history_render(e, 25, buf, sizeof(buf));
    HU_ASSERT_EQ(n, strlen(buf));
    HU_ASSERT_NULL(strstr(buf, "line 04"));
    HU_ASSERT_TRUE(strncmp(buf, "me: line 05\n", 12) == 0);
    HU_ASSERT_TRUE(n >= 14 && strcmp(buf + n - 14, "them: line 24\n") == 0);
    HU_ASSERT_EQ(hu_daemon_prospective_history_render(NULL, 0, buf, sizeof(buf)), (size_t)0);
    HU_ASSERT_STR_EQ(buf, "");
}

static void history_render_keeps_the_newest_lines_that_fit(void) {
    hu_channel_history_entry_t e[3];
    memset(e, 0, sizeof(e));
    memset(e[0].text, 'x', 40); /* oldest, too big to keep */
    e[1].from_me = true;
    snprintf(e[1].text, sizeof(e[1].text), "yes");
    snprintf(e[2].text, sizeof(e[2].text), "ok go");
    char buf[32];
    size_t n = hu_daemon_prospective_history_render(e, 3, buf, sizeof(buf));
    HU_ASSERT_STR_EQ(buf, "me: yes\nthem: ok go\n");
    HU_ASSERT_EQ(n, (size_t)20);
}

#ifdef HU_ENABLE_SQLITE
#include "human/agent.h"
#include "human/memory.h"
#include "human/memory/prospective.h"
#include <sqlite3.h>

#define C1 "+15550000001"
#define NOW ((int64_t)1790000000)

typedef struct fixed {
    const char *reply;
    int calls;
    hu_error_t err;
} fixed_t;

static hu_error_t fixed_judge(void *ctx, hu_allocator_t *alloc, const char *system,
                              size_t system_len, const char *user, size_t user_len, char **out,
                              size_t *out_len) {
    fixed_t *f = (fixed_t *)ctx;
    (void)system;
    (void)system_len;
    (void)user;
    (void)user_len;
    f->calls++;
    if (f->err != HU_OK)
        return f->err;
    size_t n = strlen(f->reply);
    char *b = (char *)alloc->alloc(alloc->ctx, n + 1);
    memcpy(b, f->reply, n + 1);
    *out = b;
    *out_len = n;
    return HU_OK;
}

static void seed_kw(sqlite3 *db, const char *cue, const char *action) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO prospective_memories(trigger_type,trigger_value,action,contact_id,"
             "expires_at,fired,created_at) VALUES('keyword','%s','%s','" C1 "',0,0,%lld)",
             cue, action, (long long)(NOW - 86400));
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
}

static void seed_pair(sqlite3 *db) {
    seed_kw(db, "taco place", "ask how the new taco place was");
    seed_kw(db, "lasagna", "send the lasagna recipe");
}

static void dump(sqlite3 *db, char *buf, size_t cap) {
    sqlite3_stmt *st = NULL;
    size_t pos = 0;
    buf[0] = '\0';
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "SELECT id, fired, status, attempts, "
                                    "COALESCE(surfaced_at, 0), COALESCE(outcome, '') "
                                    "FROM prospective_memories ORDER BY id",
                                    -1, &st, NULL),
                 SQLITE_OK);
    while (sqlite3_step(st) == SQLITE_ROW && pos + 96 < cap) {
        int w = snprintf(buf + pos, cap - pos, "%lld/%d/%s/%d/%lld/%s;",
                         (long long)sqlite3_column_int64(st, 0), sqlite3_column_int(st, 1),
                         (const char *)sqlite3_column_text(st, 2), sqlite3_column_int(st, 3),
                         (long long)sqlite3_column_int64(st, 4),
                         (const char *)sqlite3_column_text(st, 5));
        if (w > 0)
            pos += (size_t)w;
    }
    sqlite3_finalize(st);
}

static int64_t q_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    int64_t v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static hu_prospective_turn_t turn_for(const char *text) {
    hu_prospective_turn_t t;
    memset(&t, 0, sizeof(t));
    t.contact = C1;
    t.contact_len = strlen(C1);
    t.inbound = text;
    t.inbound_len = strlen(text);
    t.now = NOW;
    t.day_start = NOW - 3600;
    return t;
}

static void directive_off_is_byte_identical_to_legacy_and_never_judges(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t ma = hu_sqlite_memory_create(&alloc, ":memory:");
    hu_memory_t mb = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *a = hu_sqlite_memory_get_db(&ma);
    sqlite3 *b = hu_sqlite_memory_get_db(&mb);
    seed_pair(a);
    seed_pair(b);
    static const char msg[] = "taco place and lasagna";
    size_t la = 0;
    size_t lb = 0;
    char *da = hu_prospective_directive_build(&alloc, a, msg, sizeof(msg) - 1, C1, strlen(C1),
                                              NOW, &la);
    fixed_t f = {.reply = "fire"};
    hu_prospective_judge_t j = {.fn = fixed_judge, .ctx = &f};
    hu_prospective_turn_t t = turn_for(msg);
    char *dbuf = hu_daemon_prospective_directive(&alloc, b, HU_GATE_OFF, &t, &j, &lb);
    HU_ASSERT_NOT_NULL(da);
    HU_ASSERT_NOT_NULL(dbuf);
    HU_ASSERT_EQ(la, lb);
    HU_ASSERT_TRUE(memcmp(da, dbuf, la) == 0);
    HU_ASSERT_EQ(f.calls, 0);
    char sa[1024];
    char sb[1024];
    dump(a, sa, sizeof(sa));
    dump(b, sb, sizeof(sb));
    HU_ASSERT_STR_EQ(sa, sb);
    alloc.free(alloc.ctx, da, la + 1);
    alloc.free(alloc.ctx, dbuf, lb + 1);
    ma.vtable->deinit(ma.ctx);
    mb.vtable->deinit(mb.ctx);
}

static void directive_shadow_returns_legacy_and_writes_nothing_extra(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t ma = hu_sqlite_memory_create(&alloc, ":memory:");
    hu_memory_t mb = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *a = hu_sqlite_memory_get_db(&ma);
    sqlite3 *b = hu_sqlite_memory_get_db(&mb);
    seed_pair(a);
    seed_pair(b);
    static const char msg[] = "taco place and lasagna";
    size_t la = 0;
    size_t lb = 0;
    char *da = hu_prospective_directive_build(&alloc, a, msg, sizeof(msg) - 1, C1, strlen(C1),
                                              NOW, &la);
    fixed_t f = {.reply = "already_resolved"};
    hu_prospective_judge_t j = {.fn = fixed_judge, .ctx = &f};
    hu_prospective_turn_t t = turn_for(msg);
    char *dbuf = hu_daemon_prospective_directive(&alloc, b, HU_GATE_SHADOW, &t, &j, &lb);
    HU_ASSERT_EQ(f.calls, 2); /* Filter + Decide ran on both cued intentions */
    HU_ASSERT_EQ(la, lb);
    HU_ASSERT_TRUE(memcmp(da, dbuf, la) == 0);
    char sa[1024];
    char sb[1024];
    dump(a, sa, sizeof(sa));
    dump(b, sb, sizeof(sb));
    HU_ASSERT_STR_EQ(sa, sb); /* the judge said resolved; SHADOW wrote none of it */
    /* delivery in SHADOW only logs uptake: the store is untouched */
    hu_daemon_prospective_on_delivered(&alloc, b, HU_GATE_SHADOW, C1, strlen(C1),
                                       "how was the taco place", 22, NOW + 60);
    dump(b, sb, sizeof(sb));
    HU_ASSERT_STR_EQ(sa, sb);
    alloc.free(alloc.ctx, da, la + 1);
    alloc.free(alloc.ctx, dbuf, lb + 1);
    ma.vtable->deinit(ma.ctx);
    mb.vtable->deinit(mb.ctx);
}

static void directive_live_returns_soft_directive_and_settles_on_delivery(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t m = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&m);
    seed_pair(db);
    fixed_t f = {.reply = "fire"};
    hu_prospective_judge_t j = {.fn = fixed_judge, .ctx = &f};
    hu_prospective_turn_t t = turn_for("the taco place was packed");
    size_t len = 0;
    char *d = hu_daemon_prospective_directive(&alloc, db, HU_GATE_LIVE, &t, &j, &len);
    HU_ASSERT_STR_EQ(d, "[PROSPECTIVE MEMORY: If it fits naturally, you could bring up: ask how "
                        "the new taco place was]");
    alloc.free(alloc.ctx, d, len + 1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE fired=1"),
                 (int64_t)0); /* LIVE never marks fired at render time */
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='surfaced'"),
                 (int64_t)1);
    hu_daemon_prospective_on_delivered(&alloc, db, HU_GATE_LIVE, C1, strlen(C1),
                                       "wait how was the taco place", 27, NOW + 60);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='done' AND "
                           "fired=1 AND outcome='used'"),
                 (int64_t)1);
    m.vtable->deinit(m.ctx);
}

static void directive_live_judge_failure_is_silent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t m = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&m);
    seed_pair(db);
    fixed_t f = {.reply = "fire", .err = HU_ERR_PROVIDER_RESPONSE};
    hu_prospective_judge_t j = {.fn = fixed_judge, .ctx = &f};
    hu_prospective_turn_t t = turn_for("the taco place was packed");
    size_t len = 7;
    HU_ASSERT_NULL(hu_daemon_prospective_directive(&alloc, db, HU_GATE_LIVE, &t, &j, &len));
    HU_ASSERT_EQ(len, (size_t)0);
    HU_ASSERT_EQ(f.calls, 1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='pending' AND "
                           "fired=0"),
                 (int64_t)2);
    m.vtable->deinit(m.ctx);
}

typedef struct mock_prov {
    int calls;
    int thinking_budget;
    uint32_t max_tokens;
    double temperature;
    char model[32];
} mock_prov_t;

static hu_error_t mock_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                            const char *model, size_t model_len, double temperature,
                            hu_chat_response_t *out) {
    mock_prov_t *m = (mock_prov_t *)ctx;
    m->calls++;
    m->thinking_budget = req->thinking_budget;
    m->max_tokens = req->max_tokens;
    m->temperature = temperature;
    snprintf(m->model, sizeof(m->model), "%.*s", (int)model_len, model);
    memset(out, 0, sizeof(*out));
    char *c = (char *)alloc->alloc(alloc->ctx, 5);
    memcpy(c, "fire", 5);
    out->content = c;
    out->content_len = 4;
    return HU_OK;
}

static void provider_judge_sends_a_short_thinking_off_request(void) {
    hu_allocator_t alloc = hu_system_allocator();
    mock_prov_t m;
    memset(&m, 0, sizeof(m));
    hu_provider_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.chat = mock_chat;
    hu_provider_t p = {.ctx = &m, .vtable = &vt};
    hu_daemon_prospective_judge_ctx_t jc = {.provider = &p, .model = "glm", .model_len = 3};
    char *out = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_daemon_prospective_provider_judge(&jc, &alloc, "s", 1, "u", 1, &out, &len),
                 HU_OK);
    HU_ASSERT_STR_EQ(out, "fire");
    HU_ASSERT_EQ(m.thinking_budget, 0);
    HU_ASSERT_EQ(m.max_tokens, (uint32_t)16);
    HU_ASSERT_TRUE(m.temperature == 0.0);
    HU_ASSERT_STR_EQ(m.model, "glm");
    alloc.free(alloc.ctx, out, len + 1);
    hu_daemon_prospective_judge_ctx_t none = {.provider = NULL};
    HU_ASSERT_EQ(hu_daemon_prospective_provider_judge(&none, &alloc, "s", 1, "u", 1, &out, &len),
                 HU_ERR_INVALID_ARGUMENT);
}

/* The production entry points read HU_PROSPECTIVE and use the agent's own
 * provider — here a mock, so no network. */
static void reactive_and_delivered_entries_follow_the_env_gate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t m = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&m);
    seed_kw(db, "alpha", "ask about alpha");
    seed_kw(db, "beta", "ask about beta");
    mock_prov_t mp;
    memset(&mp, 0, sizeof(mp));
    hu_provider_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.chat = mock_chat;
    static hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.memory = &m;
    agent.provider.ctx = &mp;
    agent.provider.vtable = &vt;

    setenv("HU_PROSPECTIVE", "live", 1);
    size_t len = 0;
    char *d = hu_daemon_prospective_reactive(&alloc, &agent, db, C1, strlen(C1), "alpha!", 6,
                                             NULL, 0, false, &len);
    HU_ASSERT_STR_CONTAINS(d, "If it fits naturally, you could bring up: ask about alpha");
    alloc.free(alloc.ctx, d, len + 1);
    HU_ASSERT_EQ(mp.calls, 1);
    /* a group thread never reaches the judge */
    HU_ASSERT_NULL(hu_daemon_prospective_reactive(&alloc, &agent, db, C1, strlen(C1), "beta!", 5,
                                                  NULL, 0, true, &len));
    HU_ASSERT_EQ(mp.calls, 1);
    hu_daemon_prospective_delivered(&agent, C1, strlen(C1), "so how was alpha", 16);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE status='done'"),
                 (int64_t)1);

    unsetenv("HU_PROSPECTIVE"); /* OFF: the legacy directive, no judge call */
    d = hu_daemon_prospective_reactive(&alloc, &agent, db, C1, strlen(C1), "beta!", 5, NULL, 0,
                                       false, &len);
    HU_ASSERT_STR_CONTAINS(d, "[PROSPECTIVE MEMORY: Remember to: ask about beta");
    alloc.free(alloc.ctx, d, len + 1);
    HU_ASSERT_EQ(mp.calls, 1);
    m.vtable->deinit(m.ctx);
}
#endif /* HU_ENABLE_SQLITE */

void run_daemon_prospective_tests(void) {
    HU_TEST_SUITE("daemon prospective");
    HU_RUN_TEST(history_render_keeps_last_twenty_lines_oldest_first);
    HU_RUN_TEST(history_render_keeps_the_newest_lines_that_fit);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(directive_off_is_byte_identical_to_legacy_and_never_judges);
    HU_RUN_TEST(directive_shadow_returns_legacy_and_writes_nothing_extra);
    HU_RUN_TEST(directive_live_returns_soft_directive_and_settles_on_delivery);
    HU_RUN_TEST(directive_live_judge_failure_is_silent);
    HU_RUN_TEST(provider_judge_sends_a_short_thinking_off_request);
    HU_RUN_TEST(reactive_and_delivered_entries_follow_the_env_gate);
#endif
}
```

`hu_daemon_prospective_history_render` is called outside `#ifdef HU_ENABLE_SQLITE`, so declare and define it outside the SQLite guard in the header and source below. Register the test: add `    tests/test_daemon_prospective.c` after `tests/test_cli_prospective.c` in `CMakeLists.txt`, and add `void run_daemon_prospective_tests(void);` / `    run_daemon_prospective_tests();` to `tests/test_main.c`.

- [ ] **Step 2: Run the build to verify it fails**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human_tests -j8 2>&1 | grep -m2 error`
Expected: FAIL with `'human/daemon/prospective.h' file not found`.

- [ ] **Step 3: Write the header and the dispatcher**

Create `include/human/daemon/prospective.h`:

```c
#ifndef HU_DAEMON_PROSPECTIVE_H
#define HU_DAEMON_PROSPECTIVE_H
/*
 * Prospective memory v2 in the reactive path (spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.3-4.4).
 *
 * HU_PROSPECTIVE=off|shadow|live, default off:
 *   off    — today's directive, byte-identical: fire on match, mark fired=1.
 *   shadow — today's directive unchanged, plus Filter + Decide run read-only
 *            and log counts ("prospective shadow: candidates=…").
 *   live   — the v2 pass replaces it: soft directive; surfaced -> done only
 *            after the delivered reply carries the action.
 */
#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_agent;
struct hu_provider;

/* "me: …" / "them: …" lines of the last HU_PROSPECTIVE_HISTORY_TURNS entries,
 * oldest first. When they do not all fit in buf[cap], the NEWEST lines that
 * fit are kept (a contiguous recent window). Returns the bytes written. */
size_t hu_daemon_prospective_history_render(const hu_channel_history_entry_t *entries, size_t n,
                                            char *buf, size_t cap);

#ifdef HU_ENABLE_SQLITE
#include "human/memory/prospective_v2.h"

typedef struct hu_daemon_prospective_judge_ctx {
    struct hu_provider *provider;
    const char *model; /* NULL = provider default */
    size_t model_len;
} hu_daemon_prospective_judge_ctx_t;

/* hu_prospective_judge_fn over a provider: one short answer — temperature 0,
 * max_tokens 16, thinking off (hu_provider_chat_oneshot). */
hu_error_t hu_daemon_prospective_provider_judge(void *ctx, hu_allocator_t *alloc,
                                                const char *system, size_t system_len,
                                                const char *user, size_t user_len, char **out,
                                                size_t *out_len);

/* One dated service-log line per pass that judged, expired or capped
 * anything, plus one "item" line per judged intention. `tag`: "shadow",
 * "live", "time shadow", "time live". */
void hu_daemon_prospective_log_counts(const char *tag, const hu_prospective_counts_t *c);

/* The gate dispatch with the mode and judge injected (tests). Returns the
 * directive for the prompt (heap, free with *out_len + 1) or NULL. */
char *hu_daemon_prospective_directive(hu_allocator_t *alloc, sqlite3 *db, hu_gate_mode_t mode,
                                      const hu_prospective_turn_t *turn,
                                      const hu_prospective_judge_t *judge, size_t *out_len);

/* After-delivery evidence for the reactive path: LIVE settles surfaced
 * intentions against the delivered reply; SHADOW logs the uptake of the last
 * would-fires for this contact (no write); OFF does nothing. */
void hu_daemon_prospective_on_delivered(hu_allocator_t *alloc, sqlite3 *db, hu_gate_mode_t mode,
                                        const char *contact, size_t contact_len,
                                        const char *reply, size_t reply_len, int64_t now);

/* Production entry points: read HU_PROSPECTIVE, judge with the agent's own
 * provider and model, treat the owner's handles (hu_share_is_owner) as
 * self-chat. */
char *hu_daemon_prospective_reactive(hu_allocator_t *alloc, struct hu_agent *agent, sqlite3 *db,
                                     const char *contact, size_t contact_len, const char *text,
                                     size_t text_len, const hu_channel_history_entry_t *history,
                                     size_t history_n, bool is_group, size_t *out_len);
void hu_daemon_prospective_delivered(struct hu_agent *agent, const char *target,
                                     size_t target_len, const char *text, size_t text_len);
#endif /* HU_ENABLE_SQLITE */

#endif /* HU_DAEMON_PROSPECTIVE_H */
```

Create `src/daemon/daemon_prospective.c`:

```c
/*
 * src/daemon/daemon_prospective.c — prospective memory v2 in the reactive
 * path. Contract in include/human/daemon/prospective.h.
 *
 * HU_PROSPECTIVE activation gated on the spec §3 promotion measurement
 * (scripts/pm_bench_local.py PASS + 30 SHADOW would-fires spot-checked blind
 * with precision >= 0.8, scripts/prospective_spot_check.py): do not flip to
 * default-ON without it.
 */
#include "human/daemon/prospective.h"

#include "human/core/string.h"
#include "human/memory/prospective_policy.h"
#include <string.h>

size_t hu_daemon_prospective_history_render(const hu_channel_history_entry_t *entries, size_t n,
                                            char *buf, size_t cap) {
    if (!buf || cap == 0)
        return 0;
    buf[0] = '\0';
    if (!entries || n == 0)
        return 0;
    size_t first = n > HU_PROSPECTIVE_HISTORY_TURNS ? n - HU_PROSPECTIVE_HISTORY_TURNS : 0;
    size_t need = 0;
    size_t start = n;
    while (start > first) { /* keep the newest lines that fit */
        const hu_channel_history_entry_t *e = &entries[start - 1];
        size_t line = strnlen(e->text, sizeof(e->text)) + (e->from_me ? 4 : 6) + 1;
        if (need + line >= cap)
            break;
        need += line;
        start--;
    }
    size_t pos = 0;
    for (size_t i = start; i < n; i++)
        pos = hu_buf_appendf(buf, cap, pos, "%s: %.*s\n", entries[i].from_me ? "me" : "them",
                             (int)strnlen(entries[i].text, sizeof(entries[i].text)),
                             entries[i].text);
    return pos;
}

#ifdef HU_ENABLE_SQLITE

#include "human/agent.h"
#include "human/core/log.h"
#include "human/daemon/share_queue.h"
#include "human/memory.h"
#include "human/memory/prospective.h"
#include "human/providers/chat_oneshot.h"
#include <stdatomic.h>
#include <time.h>

/* SHADOW uptake: the would-fire actions of the last reactive turn, checked
 * against the reply actually delivered to the same contact. One slot — the
 * daemon runs one reactive turn at a time. */
static struct {
    char contact[128];
    char actions[HU_PROSPECTIVE_RENDER_CAP][256];
    size_t n;
} s_pm_shadow;

static atomic_bool s_pm_banner_once = false;

hu_error_t hu_daemon_prospective_provider_judge(void *ctx, hu_allocator_t *alloc,
                                                const char *system, size_t system_len,
                                                const char *user, size_t user_len, char **out,
                                                size_t *out_len) {
    const hu_daemon_prospective_judge_ctx_t *jc = (const hu_daemon_prospective_judge_ctx_t *)ctx;
    if (!jc || !jc->provider)
        return HU_ERR_INVALID_ARGUMENT;
    /* One word back. Thinking off: GLM on :8741 otherwise reasons out loud
     * and spends the whole budget before the verdict. */
    const hu_chat_oneshot_opts_t opts = {.temperature = 0.0, .max_tokens = 16,
                                         .json_object = false};
    return hu_provider_chat_oneshot(alloc, jc->provider, jc->model, jc->model ? jc->model_len : 0,
                                    system, system_len, user, user_len, &opts, out, out_len);
}

void hu_daemon_prospective_log_counts(const char *tag, const hu_prospective_counts_t *c) {
    if (!tag || !c || c->candidates + c->expired + c->capped == 0)
        return;
    hu_log_info("prospective", NULL,
                "prospective %s: candidates=%zu fire=%zu resolved=%zu cancel=%zu not_now=%zu "
                "parse_fail=%zu judge_err=%zu expired=%zu capped=%zu",
                tag, c->candidates, c->fire, c->resolved, c->cancel, c->not_now, c->parse_fail,
                c->judge_err, c->expired, c->capped);
    for (size_t i = 0; i < c->item_count; i++)
        hu_log_info("prospective", NULL, "prospective %s item: id=%lld verdict=%s", tag,
                    (long long)c->items[i].id,
                    c->items[i].judge_ok ? hu_prospective_verdict_str(c->items[i].verdict)
                                         : "judge_err");
}

static void pm_shadow_remember(const hu_prospective_turn_t *turn, const hu_prospective_counts_t *c) {
    s_pm_shadow.n = 0;
    if (c->fire_action_count == 0 || turn->contact_len >= sizeof(s_pm_shadow.contact))
        return;
    memcpy(s_pm_shadow.contact, turn->contact, turn->contact_len);
    s_pm_shadow.contact[turn->contact_len] = '\0';
    for (size_t i = 0; i < c->fire_action_count; i++)
        memcpy(s_pm_shadow.actions[i], c->fire_actions[i], sizeof(s_pm_shadow.actions[i]));
    s_pm_shadow.n = c->fire_action_count;
}

char *hu_daemon_prospective_directive(hu_allocator_t *alloc, sqlite3 *db, hu_gate_mode_t mode,
                                      const hu_prospective_turn_t *turn,
                                      const hu_prospective_judge_t *judge, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!alloc || !db || !turn || !out_len)
        return NULL;
    hu_prospective_counts_t c;
    if (mode == HU_GATE_LIVE) {
        char *d = NULL;
        size_t dl = 0;
        if (hu_prospective_v2_run(alloc, db, HU_PM_CUE_KEYWORD, turn, judge, true, &c, &d, &dl) !=
            HU_OK)
            return NULL; /* fail toward silence: no directive, nothing retired */
        hu_daemon_prospective_log_counts("live", &c);
        *out_len = dl;
        return d;
    }
    if (mode == HU_GATE_SHADOW &&
        hu_prospective_v2_run(alloc, db, HU_PM_CUE_KEYWORD, turn, judge, false, &c, NULL, NULL) ==
            HU_OK) {
        hu_daemon_prospective_log_counts("shadow", &c);
        pm_shadow_remember(turn, &c);
    }
    /* OFF and SHADOW: today's directive, unchanged (fire on match, fired=1). */
    return hu_prospective_directive_build(alloc, db, turn->inbound, turn->inbound_len,
                                          turn->contact, turn->contact_len, turn->now, out_len);
}

void hu_daemon_prospective_on_delivered(hu_allocator_t *alloc, sqlite3 *db, hu_gate_mode_t mode,
                                        const char *contact, size_t contact_len,
                                        const char *reply, size_t reply_len, int64_t now) {
    if (!alloc || !db || !contact || contact_len == 0)
        return;
    if (mode == HU_GATE_LIVE) {
        hu_prospective_delivery_counts_t dc;
        if (hu_prospective_v2_after_delivery(alloc, db, HU_PM_CUE_KEYWORD, contact, contact_len,
                                             reply, reply_len, now, &dc) == HU_OK &&
            dc.surfaced > 0)
            hu_log_info("prospective", NULL,
                        "prospective live delivered: surfaced=%zu used=%zu ignored=%zu "
                        "expired=%zu",
                        dc.surfaced, dc.used, dc.ignored, dc.expired);
        return;
    }
    if (mode != HU_GATE_SHADOW || s_pm_shadow.n == 0 ||
        strlen(s_pm_shadow.contact) != contact_len ||
        memcmp(s_pm_shadow.contact, contact, contact_len) != 0)
        return;
    size_t used = 0;
    for (size_t i = 0; i < s_pm_shadow.n; i++)
        if (hu_prospective_reply_uses_action(s_pm_shadow.actions[i], reply, reply_len))
            used++;
    hu_log_info("prospective", NULL, "prospective shadow uptake: would_fire=%zu used=%zu",
                s_pm_shadow.n, used);
    s_pm_shadow.n = 0;
}

char *hu_daemon_prospective_reactive(hu_allocator_t *alloc, struct hu_agent *agent, sqlite3 *db,
                                     const char *contact, size_t contact_len, const char *text,
                                     size_t text_len, const hu_channel_history_entry_t *history,
                                     size_t history_n, bool is_group, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!alloc || !db || !contact || contact_len == 0 || !text || text_len == 0 || !out_len)
        return NULL;
    hu_gate_mode_t mode = hu_prospective_gate_mode();
    hu_log_info_once(&s_pm_banner_once, "prospective", NULL, "%s",
                     hu_prospective_gate_banner(mode, false));
    int64_t now = (int64_t)time(NULL);
    if (mode == HU_GATE_OFF) /* byte-identical to the pre-v2 call sites */
        return hu_prospective_directive_build(alloc, db, text, text_len, contact, contact_len,
                                              now, out_len);
    char hist[6144];
    hu_prospective_turn_t turn;
    memset(&turn, 0, sizeof(turn));
    turn.contact = contact;
    turn.contact_len = contact_len;
    turn.inbound = text;
    turn.inbound_len = text_len;
    turn.history = hist;
    turn.history_len = hu_daemon_prospective_history_render(history, history_n, hist, sizeof(hist));
    turn.is_group = is_group;
    turn.is_self = agent && hu_share_is_owner(agent->persona, contact, contact_len);
    turn.now = now;
    turn.day_start = hu_prospective_local_day_start(now);
    hu_daemon_prospective_judge_ctx_t jc = {
        .provider = agent ? &agent->provider : NULL,
        .model = agent ? agent->model_name : NULL,
        .model_len = agent ? agent->model_name_len : 0,
    };
    hu_prospective_judge_t judge = {.fn = hu_daemon_prospective_provider_judge, .ctx = &jc};
    return hu_daemon_prospective_directive(alloc, db, mode, &turn, &judge, out_len);
}

void hu_daemon_prospective_delivered(struct hu_agent *agent, const char *target,
                                     size_t target_len, const char *text, size_t text_len) {
    if (!agent || !agent->memory || !agent->alloc || !target || target_len == 0)
        return;
    hu_gate_mode_t mode = hu_prospective_gate_mode();
    if (mode == HU_GATE_OFF)
        return;
    sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
    if (db)
        hu_daemon_prospective_on_delivered(agent->alloc, db, mode, target, target_len, text,
                                           text_len, (int64_t)time(NULL));
}

#endif /* HU_ENABLE_SQLITE */
```

In `CMakeLists.txt`, after `    src/daemon/daemon_prospective_time.c`:

```cmake
    src/daemon/daemon_prospective.c
```

- [ ] **Step 4: Run the new tests**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human_tests -j8 && cd "$W" && ./build/human_tests --suite="daemon prospective"`
Expected: PASS for `daemon prospective` (8/8) and `daemon prospective time` (3/3). The substring `--suite` match runs both.

- [ ] **Step 5: Wire the call sites**

`include/human/daemon/reactive_turn.h`: in `hu_reactive_turn_ctx_t`, after `bool llm_decides; /* channels.<ch>.daemon.llm_decides */`, add:

```c
    bool is_group;    /* group thread: prospective reminders never fire here */
```

`src/daemon.c`: directly after `                rt.llm_decides = llm_decides;` add

```c
                rt.is_group = msgs[batch_start].is_group;
```

`src/daemon/daemon_reactive_prompt.c`: add `#include "human/daemon/prospective.h"` to the `human/daemon/*.h` include group. At site 1, replace the comment and the call:

```c
                /* 9. Prospective memory — cued intentions. HU_PROSPECTIVE picks
                 * the legacy builder (off/shadow) or the v2 fire-time check
                 * (live); src/daemon/daemon_prospective.c. */
                if (combined_len > 0) {
                    size_t pd_len = 0;
                    char *pd = hu_daemon_prospective_reactive(
                        alloc, agent, db, batch_key, key_len, combined, combined_len, ctx_entries,
                        ctx_count, rt->is_group, &pd_len);
                    if (pd)
                        PHASE6_APPEND(pd, pd_len);
                }
```

At site 2 (the `llm_decides` block), replace

```c
            char *pd = hu_prospective_directive_build(alloc, pdb, combined, combined_len, batch_key,
                                                      key_len, (int64_t)time(NULL), &pd_len);
```

with

```c
            char *pd = hu_daemon_prospective_reactive(alloc, agent, pdb, batch_key, key_len,
                                                      combined, combined_len, ctx_entries,
                                                      ctx_count, rt->is_group, &pd_len);
```

`src/daemon/daemon_message_router.c`: add `#include "human/daemon/prospective.h"` to the includes. As the first statement of `hu_daemon_record_delivered_reply`, before the `sota_initialized` early return, add:

```c
#ifdef HU_ENABLE_SQLITE
    /* Prospective v2 settles surfaced reminders against the text as DELIVERED
     * (spec 2026-09-30 §4.3). Runs before the collector checks: it depends on
     * HU_PROSPECTIVE, not on SOTA. */
    hu_daemon_prospective_delivered(agent, target, target_len, text, text_len);
#endif
```

In `include/human/daemon/message_router.h`, add this sentence to the end of the `hu_daemon_record_delivered_reply` comment: `Also hands the delivered text to hu_daemon_prospective_delivered (a no-op unless HU_PROSPECTIVE is shadow/live).`

- [ ] **Step 6: `--judge model` in the probe**

In `src/app/cli_prospective.c`, add these includes inside the `#ifdef HU_ENABLE_SQLITE` section:

```c
#include "human/config.h"
#include "human/daemon/prospective.h"
#include "human/providers/factory.h"
```

In `cmd_prospective`, replace from `    hu_error_t err;` through the closing `}` of the `if (strcmp(a.judge, "model") == 0) { … } else { … }` block with:

```c
    /* --judge model: the configured provider and default model (GLM on :8741
     * in prod), through the same thinking-off adapter the daemon uses. */
    bool use_model = strcmp(a.judge, "model") == 0;
    hu_config_t cfg;
    hu_provider_t prov;
    hu_daemon_prospective_judge_ctx_t jc;
    memset(&cfg, 0, sizeof(cfg));
    memset(&prov, 0, sizeof(prov));
    memset(&jc, 0, sizeof(jc));
    hu_error_t err = HU_OK;
    if (use_model) {
        err = hu_config_load(alloc, &cfg);
        if (err == HU_OK && (err = hu_provider_create_default(alloc, &cfg, &prov)) != HU_OK)
            hu_config_deinit(&cfg);
        if (err == HU_OK) {
            jc.provider = &prov;
            jc.model = cfg.default_model;
            jc.model_len = cfg.default_model ? strlen(cfg.default_model) : 0;
            judge.fn = hu_daemon_prospective_provider_judge;
            judge.ctx = &jc;
        }
    }
    if (err == HU_OK)
        err = hu_cli_prospective_run(alloc, &mem, &a, hist, hist_len, &judge, stdout);
    if (use_model && prov.vtable) {
        if (prov.vtable->deinit)
            prov.vtable->deinit(prov.ctx, alloc);
        hu_config_deinit(&cfg);
    }
```

The `if (err != HU_OK) fprintf(…)` line and the cleanup that follow stay as they are.

- [ ] **Step 7: Build, run, and check the gates**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human human_tests -j8 && cd "$W" && ./build/human_tests --suite=prospective && ./build/human_tests 2>&1 | grep -E 'Results:' && wc -l src/daemon.c && bash scripts/check-file-size-ceiling.sh && bash scripts/check-function-length-ceiling.sh && bash scripts/check-sqlite-includer-ratchet.sh && bash scripts/check-clone-ratchet.sh && bash scripts/check-dead-strip-ratchet.sh && bash scripts/check-agent-core-boundary.sh`
Expected: 0 failed, and `daemon.c` ≤ 10420 (Task 5 freed the room). Every gate passes. The reactive prompt builder is compiled out under `HU_IS_TEST`, so the `daemon prospective` tests pin its behavior through `hu_daemon_prospective_reactive` itself.

Confirm the wiring with LSP rather than by reading: `LSP incomingCalls` on `hu_daemon_prospective_reactive` (`include/human/daemon/prospective.h`) should list `hu_daemon_reactive_prompt_build` (its two sites in `daemon_reactive_prompt.c`), and on `hu_daemon_prospective_delivered` it should list `hu_daemon_record_delivered_reply`. Paste both results into the task report (verify-before-you-claim, "a real caller, proven").

- [ ] **Step 8: Commit**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2
git -C "$W" add include/human/daemon/prospective.h src/daemon/daemon_prospective.c \
  src/daemon/daemon_reactive_prompt.c include/human/daemon/reactive_turn.h src/daemon.c \
  src/daemon/daemon_message_router.c include/human/daemon/message_router.h \
  src/app/cli_prospective.c CMakeLists.txt tests/test_main.c tests/test_daemon_prospective.c
git -C "$W" commit -m "feat(prospective): HU_PROSPECTIVE gate on the reactive path, default off

off is the legacy directive byte for byte and never calls the model; shadow
keeps it and logs Filter+Decide counts plus would-fire uptake; live renders
the soft directive and marks done only when the delivered reply carries the
action (hooked at hu_daemon_record_delivered_reply). Group threads and the
owner's self-chat are never judged. The probe gains --judge model.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: The existing writers mirror dated intentions as time rows

§4.1 says pending dated commitments and delayed follow-ups are mirrored in "by a one-time backfill plus the existing writers". This task covers the writers: `hu_superhuman_commitment_store` (when `deadline > 0`) and `hu_superhuman_delayed_followup_schedule` (when `scheduled_at > 0`). Their callers are the promise keeper, the F20 inbound commitment keeper in `daemon.c`, and `daemon_dated_followup.c`. The mirror is a data write with no effect on what is sent: nothing reads time rows until `HU_PROSPECTIVE_TIME` is shadow or live, so it is not gated. A failed mirror is logged and otherwise ignored, because the ledger row is the record.

The F20 keeper stores the *contact's* commitments ("I'll send the photos"). They are mirrored with source `promise_keeper` too, because the spec's `source` enum has no third value for them.

**Files:**
- Modify: `src/memory/superhuman.c` (includes ~:3-16; `hu_superhuman_commitment_store` tail ~:219-222; `hu_superhuman_delayed_followup_schedule` tail ~:528-531)
- Test: `tests/test_superhuman.c`

**Interfaces:**
- Consumes: `hu_prospective_repo_upsert_time` (Task 3), `HU_PROSPECTIVE_TIME_GRACE_S`, `HU_PM_SOURCE_PROMISE_KEEPER`, `HU_PM_SOURCE_FOLLOWUP`, `HU_PM_PENDING`.
- Produces: a time row with `trigger_value` `commitment:<rowid>` or `followup:<rowid>`, `due_at` = deadline/scheduled_at, and `expires_at` = due + 3 days, for every new dated item. A commitment and its paired follow-up give **one** row.

- [ ] **Step 1: Write the failing tests**

In `tests/test_superhuman.c`, inside the existing `#ifdef HU_ENABLE_SQLITE` region with the other commitment tests, add:

```c
static int64_t sh_count(hu_memory_t *mem, const char *sql) {
    sqlite3 *db = hu_sqlite_memory_get_db(mem);
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    int64_t v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* Prospective memory v2 §4.1: a dated commitment is also a time intention. */
static void superhuman_dated_commitment_mirrors_a_time_intention(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "contact_a", 9, "call the dentist",
                                                16, "me", 2, 5000),
                 HU_OK);
    HU_ASSERT_EQ(sh_count(&mem, "SELECT COUNT(*) FROM prospective_memories WHERE "
                                "cue_kind='time' AND trigger_type='time' AND "
                                "trigger_value='commitment:1' AND action='call the dentist' AND "
                                "contact_id='contact_a' AND due_at=5000 AND expires_at=264200 AND "
                                "status='pending' AND fired=0 AND source='promise_keeper'"),
                 (int64_t)1);
    /* an undated commitment is no time intention */
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "contact_a", 9, "grab coffee", 11,
                                                "me", 2, 0),
                 HU_OK);
    HU_ASSERT_EQ(sh_count(&mem, "SELECT COUNT(*) FROM prospective_memories WHERE cue_kind='time'"),
                 (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}

/* The promise keeper stores a commitment AND schedules its follow-up for the
 * same promise: one intention, one reminder. */
static void superhuman_dated_commitment_and_its_followup_mirror_once(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "contact_a", 9, "call the dentist",
                                                16, "me", 2, 5000),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "contact_a", 9,
                                                         "call the dentist", 16, 5000),
                 HU_OK);
    HU_ASSERT_EQ(sh_count(&mem, "SELECT COUNT(*) FROM prospective_memories WHERE cue_kind='time'"),
                 (int64_t)1);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "contact_a", 9,
                                                         "the job interview", 17, 7000),
                 HU_OK);
    HU_ASSERT_EQ(sh_count(&mem, "SELECT COUNT(*) FROM prospective_memories WHERE "
                                "cue_kind='time' AND trigger_value='followup:2' AND "
                                "source='followup' AND due_at=7000"),
                 (int64_t)1);
    mem.vtable->deinit(mem.ctx);
}
```

Register both inside the `#ifdef HU_ENABLE_SQLITE` part of `run_superhuman_tests`, after `superhuman_commitment_mark_followed_up`:

```c
    HU_RUN_TEST(superhuman_dated_commitment_mirrors_a_time_intention);
    HU_RUN_TEST(superhuman_dated_commitment_and_its_followup_mirror_once);
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human_tests -j8 && cd "$W" && ./build/human_tests --suite=superhuman --filter=mirror`
Expected: FAIL at `superhuman_dated_commitment_mirrors_a_time_intention` with `expected 0 == 1`, because no time row exists yet.

- [ ] **Step 3: Mirror in the writers**

In `src/memory/superhuman.c`, add to the includes:

```c
#include "human/core/log.h"
#include "human/memory/prospective_repo.h"
```

Add this helper above `hu_superhuman_commitment_store`:

```c
/* Prospective memory v2 (docs/superpowers/specs/2026-09-30-prospective-
 * memory-v2-design.md §4.1): every dated intention is also a time-cued row in
 * prospective_memories, keyed "<kind>:<rowid>" of the ledger row just
 * inserted. Best-effort — that ledger row is the record; a failed mirror is
 * logged and the time path misses this one item. A commitment and its paired
 * delayed follow-up collapse into one row (the upsert dedupes on contact +
 * action + due_at). */
static void pm_mirror_time(sqlite3 *db, const char *kind, const char *contact,
                           size_t contact_len, const char *text, size_t text_len, int64_t due_at,
                           hu_prospective_source_t source) {
    char key[64];
    snprintf(key, sizeof(key), "%s:%lld", kind, (long long)sqlite3_last_insert_rowid(db));
    if (hu_prospective_repo_upsert_time(db, contact, contact_len, text, text_len, due_at,
                                        HU_PROSPECTIVE_TIME_GRACE_S, source, key, HU_PM_PENDING,
                                        (int64_t)time(NULL), NULL) != HU_OK)
        hu_log_warn("superhuman", NULL, "prospective time mirror failed for %s", key);
}
```

In `hu_superhuman_commitment_store`, replace the tail. The anchor is the three lines after `sqlite3_bind_int64(stmt, 5, now_ts);`:

```c
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
```

with

```c
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;
    if (deadline > 0)
        pm_mirror_time(db, "commitment", contact_id, contact_id_len, description, desc_len,
                       deadline, HU_PM_SOURCE_PROMISE_KEEPER);
    return HU_OK;
```

In `hu_superhuman_delayed_followup_schedule`, apply the same replacement to the three lines after `sqlite3_bind_int64(stmt, 3, scheduled_at);`:

```c
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;
    if (scheduled_at > 0)
        pm_mirror_time(db, "followup", contact_id, contact_id_len, topic, topic_len, scheduled_at,
                       HU_PM_SOURCE_FOLLOWUP);
    return HU_OK;
```

(Those three tail lines repeat throughout the file. Anchor each edit on the unique bind line above it.)

- [ ] **Step 4: Run the tests to verify they pass**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human human_tests -j8 && cd "$W" && ./build/human_tests --suite=superhuman && ./build/human_tests --suite=promise && ./build/human_tests --suite=dated_followup`
Expected: PASS. The two new tests pass, and the promise-keeper and dated-follow-up suites are unchanged.

- [ ] **Step 5: Full suite, clone ratchet, commit**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cd "$W" && ./build/human_tests 2>&1 | grep -E 'Results:' && bash scripts/check-clone-ratchet.sh && bash scripts/check-silent-success.sh`
Expected: 0 failed. The clone count is at or under its ceiling. `check-silent-success` passes: the mirror's error is checked and logged, not discarded.

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2
git -C "$W" add src/memory/superhuman.c tests/test_superhuman.c
git -C "$W" commit -m "feat(prospective): dated commitments and follow-ups mirror as time intentions

Spec 2026-09-30 §4.1: the existing writers now also upsert a cue_kind='time'
row (key commitment:<id> / followup:<id>, expires due + 3 days). The promise
keeper's commitment + paired follow-up collapse into one intention. Nothing
reads these rows until HU_PROSPECTIVE_TIME leaves off.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: `HU_PROSPECTIVE_TIME` in the proactive tick (SHADOW only logs)

**Files:**
- Modify: `include/human/daemon/prospective_time.h`, `src/daemon/daemon_prospective_time.c`
- Modify: `src/daemon.c` (one call in the proactive `if (sent) { … #ifdef HU_ENABLE_SQLITE` block, ~:1575 after Task 5)
- Test: `tests/test_daemon_prospective_time.c`

**Interfaces:**
- Consumes: `hu_prospective_time_gate_mode`, `hu_prospective_gate_banner`, `hu_prospective_local_day_start`, `hu_prospective_v2_run`, `hu_prospective_v2_after_delivery`, `hu_daemon_prospective_provider_judge`, `hu_daemon_prospective_history_render`, `hu_daemon_prospective_log_counts`, `hu_share_is_owner`.
- Produces:
  - `void hu_daemon_prospective_time_after_send(struct hu_agent *agent, const char *contact_id, const char *text, size_t text_len, int64_t now);`
  - Gate behavior of the two Task 5 producers:
    - **OFF**: legacy, unchanged.
    - **SHADOW**: legacy output unchanged, plus one read-only v2 time pass per contact per day, logged as `prospective time shadow: …`.
    - **LIVE**: `hu_daemon_prospective_commitment_ctx` returns nothing, because commitments are mirrored into the due set. `hu_daemon_prospective_due_followups` returns the v2 per-contact due set (`- <action>\n`, at most 1 per contact per day) and leaves `*listed_id` untouched, so the legacy mark-sent does not fire.

- [ ] **Step 1: Write the failing tests**

In `tests/test_daemon_prospective_time.c`, add inside the `#ifdef HU_ENABLE_SQLITE` section (after the Task 5 tests):

```c
#include "human/daemon/prospective.h"
#include <sqlite3.h>
#include <stdlib.h>

#define TA "+15550000001"
#define TB "+15550000002"
#define TNOW ((int64_t)1790000000)

typedef struct tmock {
    int calls;
} tmock_t;

static hu_error_t tmock_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                             const char *model, size_t model_len, double temperature,
                             hu_chat_response_t *out) {
    (void)req;
    (void)model;
    (void)model_len;
    (void)temperature;
    ((tmock_t *)ctx)->calls++;
    memset(out, 0, sizeof(*out));
    char *c = (char *)alloc->alloc(alloc->ctx, 5);
    memcpy(c, "fire", 5);
    out->content = c;
    out->content_len = 4;
    return HU_OK;
}

static hu_provider_vtable_t s_tmock_vt;

static void agent_with_mock(hu_agent_t *agent, hu_allocator_t *alloc, hu_memory_t *mem,
                            tmock_t *m) {
    memset(agent, 0, sizeof(*agent));
    memset(&s_tmock_vt, 0, sizeof(s_tmock_vt));
    s_tmock_vt.chat = tmock_chat;
    agent->alloc = alloc;
    agent->memory = mem;
    agent->provider.ctx = m;
    agent->provider.vtable = &s_tmock_vt;
}

static int64_t t_count(hu_memory_t *mem, const char *sql) {
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(hu_sqlite_memory_get_db(mem), sql, -1, &st, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    int64_t v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* The legacy read took the 3 oldest due commitments GLOBALLY, then filtered
 * by contact: another contact's backlog hid A's due item. LIVE reads A's own
 * due set. */
static void time_live_uses_the_per_contact_due_set(void) {
    setenv("HU_PROSPECTIVE_TIME", "live", 1);
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    tmock_t m = {0};
    static hu_agent_t agent;
    agent_with_mock(&agent, &alloc, &mem, &m);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, TB, 12, "return the drill", 16,
                                                "me", 2, TNOW - 9000),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, TB, 12, "send the photos", 15, "me",
                                                2, TNOW - 8000),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, TB, 12, "book the cabin", 14, "me",
                                                2, TNOW - 7000),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, TA, 12, "call about the lease", 20,
                                                "me", 2, TNOW - 3600),
                 HU_OK);
    char buf[640];
    int64_t listed = -1;
    size_t n = hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, TA, TNOW, buf,
                                                   sizeof(buf), &listed);
    HU_ASSERT_STR_EQ(buf, "- call about the lease\n");
    HU_ASSERT_EQ(n, strlen(buf));
    HU_ASSERT_EQ(listed, (int64_t)-1); /* the legacy mark-sent stays out of LIVE */
    HU_ASSERT_EQ(m.calls, 1);
    char *ctx = NULL;
    size_t cl = 0;
    int64_t ids[3];
    size_t idn = 0;
    hu_daemon_prospective_commitment_ctx(&alloc, &agent, TA, TNOW, &ctx, &cl, ids, &idn);
    HU_ASSERT_NULL(ctx); /* LIVE: covered by the due set, never twice */
    HU_ASSERT_EQ(idn, (size_t)0);
    unsetenv("HU_PROSPECTIVE_TIME");
    mem.vtable->deinit(mem.ctx);
}

static void time_live_caps_one_per_contact_per_day(void) {
    setenv("HU_PROSPECTIVE_TIME", "live", 1);
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    tmock_t m = {0};
    static hu_agent_t agent;
    agent_with_mock(&agent, &alloc, &mem, &m);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, TA, 12, "call about the lease", 20,
                                                "me", 2, TNOW - 3600),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, TA, 12, "return the drill", 16,
                                                "me", 2, TNOW - 1800),
                 HU_OK);
    char buf[640];
    int64_t listed = -1;
    HU_ASSERT_TRUE(hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, TA, TNOW,
                                                       buf, sizeof(buf), &listed) > 0);
    HU_ASSERT_EQ(hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, TA,
                                                     TNOW + 600, buf, sizeof(buf), &listed),
                 (size_t)0);
    HU_ASSERT_EQ(m.calls, 1);
    unsetenv("HU_PROSPECTIVE_TIME");
    mem.vtable->deinit(mem.ctx);
}

static void time_after_send_marks_done_and_retires_the_ledger(void) {
    setenv("HU_PROSPECTIVE_TIME", "live", 1);
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    tmock_t m = {0};
    static hu_agent_t agent;
    agent_with_mock(&agent, &alloc, &mem, &m);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, TA, 12, "call about the lease", 20,
                                                "me", 2, TNOW - 3600),
                 HU_OK);
    char buf[640];
    int64_t listed = -1;
    HU_ASSERT_TRUE(hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, TA, TNOW,
                                                       buf, sizeof(buf), &listed) > 0);
    static const char sent[] = "hey did you ever call about the lease?";
    hu_daemon_prospective_time_after_send(&agent, TA, sent, sizeof(sent) - 1, TNOW + 60);
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM prospective_memories WHERE "
                               "cue_kind='time' AND status='done' AND outcome='used'"),
                 (int64_t)1);
    HU_ASSERT_EQ(t_count(&mem, "SELECT COUNT(*) FROM commitments WHERE status='followed_up'"),
                 (int64_t)1);
    unsetenv("HU_PROSPECTIVE_TIME");
    /* OFF: the hook does nothing */
    hu_daemon_prospective_time_after_send(&agent, TA, sent, sizeof(sent) - 1, TNOW + 120);
    mem.vtable->deinit(mem.ctx);
}

static void time_shadow_keeps_legacy_output_and_store(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t ma = hu_sqlite_memory_create(&alloc, ":memory:");
    hu_memory_t mb = hu_sqlite_memory_create(&alloc, ":memory:");
    tmock_t m = {0};
    static hu_agent_t agent_a;
    static hu_agent_t agent_b;
    agent_with_mock(&agent_a, &alloc, &ma, &m);
    agent_with_mock(&agent_b, &alloc, &mb, &m);
    hu_memory_t *both[2] = {&ma, &mb};
    for (int i = 0; i < 2; i++) {
        HU_ASSERT_EQ(hu_superhuman_commitment_store(both[i], &alloc, TA, 12,
                                                    "call about the lease", 20, "me", 2,
                                                    TNOW - 3600),
                     HU_OK);
        HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(both[i], &alloc, TA, 12,
                                                             "call about the lease", 20,
                                                             TNOW - 3600),
                     HU_OK);
    }
    char off[640];
    char sh[640];
    int64_t lo = -1;
    int64_t ls = -1;
    size_t no = hu_daemon_prospective_due_followups(&alloc, &agent_a, NULL, NULL, 0, TA, TNOW, off,
                                                    sizeof(off), &lo);
    setenv("HU_PROSPECTIVE_TIME", "shadow", 1);
    size_t ns = hu_daemon_prospective_due_followups(&alloc, &agent_b, NULL, NULL, 0, TA, TNOW, sh,
                                                    sizeof(sh), &ls);
    unsetenv("HU_PROSPECTIVE_TIME");
    HU_ASSERT_TRUE(no > 0);
    HU_ASSERT_EQ(no, ns);
    HU_ASSERT_STR_EQ(off, sh);
    HU_ASSERT_EQ(lo, ls);
    HU_ASSERT_EQ(m.calls, 1); /* SHADOW judged once, read-only */
    HU_ASSERT_EQ(t_count(&mb, "SELECT COUNT(*) FROM prospective_memories WHERE status='pending' "
                              "AND surfaced_at IS NULL"),
                 (int64_t)1);
    ma.vtable->deinit(ma.ctx);
    mb.vtable->deinit(mb.ctx);
}
```

Register them in `run_daemon_prospective_time_tests` inside the `#ifdef HU_ENABLE_SQLITE` block:

```c
    HU_RUN_TEST(time_live_uses_the_per_contact_due_set);
    HU_RUN_TEST(time_live_caps_one_per_contact_per_day);
    HU_RUN_TEST(time_after_send_marks_done_and_retires_the_ledger);
    HU_RUN_TEST(time_shadow_keeps_legacy_output_and_store);
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human_tests -j8 2>&1 | grep -m2 error`
Expected: FAIL with an implicit declaration of `hu_daemon_prospective_time_after_send`. Once it is declared but not implemented, `time_live_uses_the_per_contact_due_set` fails because the legacy line is returned.

- [ ] **Step 3: Add the gate**

In `include/human/daemon/prospective_time.h`, before the closing `#endif`, add:

```c
/* HU_PROSPECTIVE_TIME=off|shadow|live (default off) selects what the two
 * producers above return:
 *   off    — the legacy producers, unchanged;
 *   shadow — the legacy producers, plus one read-only v2 time pass per contact
 *            per day, logged as "prospective time shadow: …";
 *   live   — commitment_ctx returns nothing (commitments are mirrored into the
 *            typed store) and due_followups returns the v2 per-contact due set
 *            ("- <action>\n", at most one per contact per day), leaving
 *            *listed_id untouched.
 * HU_PROSPECTIVE_TIME stays in shadow until HU_PROSPECTIVE has passed its
 * promotion (spec §4.4) and then needs its own 7-day SHADOW read. */

/* After a proactive message was delivered (live only): settle the surfaced
 * time intention against the sent text — done when it carries the action
 * (its commitment / follow-up ledger rows retire with it), otherwise an
 * attempt. */
void hu_daemon_prospective_time_after_send(struct hu_agent *agent, const char *contact_id,
                                           const char *text, size_t text_len, int64_t now);
```

In `src/daemon/daemon_prospective_time.c`:

1. After the existing includes, add:

```c
#ifdef HU_ENABLE_SQLITE
#include "human/core/gate_mode.h"
#include "human/core/log.h"
#include "human/daemon/prospective.h"
#include "human/daemon/share_queue.h"
#include "human/memory.h"
#include "human/memory/prospective_policy.h"
#include "human/memory/prospective_v2.h"
#include <stdatomic.h>

static atomic_bool s_pm_time_banner_once = false;

/* SHADOW runs on every proactive tick but counts a contact once per local
 * day, so its would-send numbers are per day, not per tick. */
#define PM_TIME_SHADOW_SLOTS 64
static struct {
    uint64_t hash;
    int64_t day;
} s_pm_time_seen[PM_TIME_SHADOW_SLOTS];

static bool pm_time_first_today(const char *contact, int64_t day) {
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = contact; *p; p++) {
        h ^= (uint64_t)(unsigned char)*p;
        h *= 1099511628211ULL;
    }
    size_t slot = (size_t)(h % PM_TIME_SHADOW_SLOTS);
    if (s_pm_time_seen[slot].hash == h && s_pm_time_seen[slot].day == day)
        return false;
    s_pm_time_seen[slot].hash = h;
    s_pm_time_seen[slot].day = day;
    return true;
}

/* One v2 time pass for the contact. live: apply and copy the due line(s) into
 * buf; shadow: read-only, logged, nothing returned. History comes from the
 * send channel, as the proactive tick has no conversation loaded. */
static size_t pm_time_v2(hu_allocator_t *alloc, struct hu_agent *agent, struct hu_channel *ch,
                         const char *target, size_t target_len, const char *contact_id,
                         int64_t now, bool live, char *buf, size_t cap) {
    sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
    int64_t day = hu_prospective_local_day_start(now);
    if (!db || (!live && !pm_time_first_today(contact_id, day)))
        return 0;
    char hist[6144];
    size_t hist_len = 0;
    hist[0] = '\0';
    if (ch && ch->vtable && ch->vtable->load_conversation_history && target && target_len > 0) {
        hu_channel_history_entry_t *entries = NULL;
        size_t n = 0;
        if (ch->vtable->load_conversation_history(ch->ctx, alloc, target, target_len,
                                                  HU_PROSPECTIVE_HISTORY_TURNS, &entries,
                                                  &n) == HU_OK &&
            entries) {
            hist_len = hu_daemon_prospective_history_render(entries, n, hist, sizeof(hist));
            alloc->free(alloc->ctx, entries, n * sizeof(hu_channel_history_entry_t));
        }
    }
    size_t clen = strlen(contact_id);
    hu_prospective_turn_t turn;
    memset(&turn, 0, sizeof(turn));
    turn.contact = contact_id;
    turn.contact_len = clen;
    turn.history = hist;
    turn.history_len = hist_len;
    turn.is_self = hu_share_is_owner(agent->persona, contact_id, clen);
    turn.now = now;
    turn.day_start = day;
    hu_daemon_prospective_judge_ctx_t jc = {.provider = &agent->provider,
                                            .model = agent->model_name,
                                            .model_len = agent->model_name_len};
    hu_prospective_judge_t judge = {.fn = hu_daemon_prospective_provider_judge, .ctx = &jc};
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    if (hu_prospective_v2_run(alloc, db, HU_PM_CUE_TIME, &turn, &judge, live, &c, &d, &dl) !=
        HU_OK)
        return 0; /* fail toward silence */
    hu_daemon_prospective_log_counts(live ? "time live" : "time shadow", &c);
    size_t n = 0;
    if (d && dl > 0 && dl < cap) {
        memcpy(buf, d, dl);
        buf[dl] = '\0';
        n = dl;
    }
    if (d)
        alloc->free(alloc->ctx, d, dl + 1);
    return n;
}
#endif /* HU_ENABLE_SQLITE */
```

2. In `hu_daemon_prospective_commitment_ctx`, directly after its argument-check `return;`, add:

```c
#ifdef HU_ENABLE_SQLITE
    /* LIVE: commitments are mirrored into the typed store and reach the
     * proposer through the per-contact due set — never twice. */
    if (hu_prospective_time_gate_mode() == HU_GATE_LIVE)
        return;
#endif
```

3. Replace the body of `hu_daemon_prospective_due_followups` with:

```c
    if (buf && cap > 0)
        buf[0] = '\0';
    if (!alloc || !agent || !agent->memory || !contact_id || !buf || cap == 0)
        return 0;
#ifdef HU_ENABLE_SQLITE
    /* HU_PROSPECTIVE_TIME activation gated on its own 7-day SHADOW read after
     * HU_PROSPECTIVE passes promotion (spec §4.4, §6 step 5): do not flip to
     * default-ON without it. */
    hu_gate_mode_t mode = hu_prospective_time_gate_mode();
    hu_log_info_once(&s_pm_time_banner_once, "prospective", NULL, "%s",
                     hu_prospective_gate_banner(mode, true));
    if (mode == HU_GATE_LIVE)
        return pm_time_v2(alloc, agent, ch, target, target_len, contact_id, now, true, buf, cap);
    if (mode == HU_GATE_SHADOW) {
        char scratch[640];
        (void)pm_time_v2(alloc, agent, ch, target, target_len, contact_id, now, false, scratch,
                         sizeof(scratch));
    }
#else
    (void)ch;
    (void)target;
    (void)target_len;
#endif
    return pm_legacy_due_followups(alloc, agent, contact_id, now, buf, cap, listed_id);
```

4. Append the after-send hook:

```c
void hu_daemon_prospective_time_after_send(struct hu_agent *agent, const char *contact_id,
                                           const char *text, size_t text_len, int64_t now) {
#ifdef HU_ENABLE_SQLITE
    if (!agent || !agent->memory || !agent->alloc || !contact_id ||
        hu_prospective_time_gate_mode() != HU_GATE_LIVE)
        return;
    sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
    if (!db)
        return;
    hu_prospective_delivery_counts_t dc;
    if (hu_prospective_v2_after_delivery(agent->alloc, db, HU_PM_CUE_TIME, contact_id,
                                         strlen(contact_id), text, text_len, now, &dc) == HU_OK &&
        dc.surfaced > 0)
        hu_log_info("prospective", NULL,
                    "prospective time live delivered: surfaced=%zu used=%zu ignored=%zu "
                    "expired=%zu",
                    dc.surfaced, dc.used, dc.ignored, dc.expired);
#else
    (void)agent;
    (void)contact_id;
    (void)text;
    (void)text_len;
    (void)now;
#endif
}
```

In `src/daemon.c`, inside the proactive `if (sent) {` block's `#ifdef HU_ENABLE_SQLITE` section, after the `due_followup_id_listed` mark-sent statement, add:

```c
                        hu_daemon_prospective_time_after_send(agent, cp->contact_id, response,
                                                              response_len, (int64_t)now);
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human human_tests -j8 && cd "$W" && ./build/human_tests --suite="daemon prospective time"`
Expected: PASS, 7/7.

- [ ] **Step 5: Full suite, gates, commit**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cd "$W" && ./build/human_tests 2>&1 | grep -E 'Results:' && wc -l src/daemon.c && bash scripts/check-file-size-ceiling.sh && bash scripts/check-function-length-ceiling.sh && bash scripts/check-clone-ratchet.sh && bash scripts/check-dead-strip-ratchet.sh && bash scripts/check-sqlite-includer-ratchet.sh`
Expected: 0 failed, `daemon.c` ≤ 10420, and every gate passes. `LSP incomingCalls` on `hu_daemon_prospective_time_after_send` lists `hu_service_run`. Paste that result into the task report.

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2
git -C "$W" add include/human/daemon/prospective_time.h src/daemon/daemon_prospective_time.c \
  src/daemon.c tests/test_daemon_prospective_time.c
git -C "$W" commit -m "feat(prospective): HU_PROSPECTIVE_TIME gate on the proactive tick, default off

shadow keeps the legacy follow-up producers and logs one read-only v2 time
pass per contact per day; live replaces the global oldest-3 commitment read
with the per-contact due set (1 per contact per day, 3-day grace, fire-time
check with channel history) and settles the item against the delivered
proactive text. Stays in shadow until HU_PROSPECTIVE is promoted.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: One-time backfill (C) and its backup-first wrapper (Python)

**Files:**
- Modify: `include/human/memory/prospective_v2.h`, `src/memory/prospective_v2.c` (add the backfill)
- Modify: `src/app/cli_prospective.c` (`backfill` op), `tests/test_cli_prospective.c`
- Create: `scripts/prospective_backfill.py`
- Test: `tests/test_prospective_v2.c` (backfill), `tests/test_prospective_backfill.py`

**Interfaces:**
- Consumes: `hu_superhuman_commitment_list_due`, `hu_superhuman_delayed_followup_list_due`, their `_free`, `hu_prospective_repo_upsert_time`, `hu_sql_txn_begin/commit/rollback` (`include/human/memory/sql_transaction.h`).
- Produces:
  - `typedef struct hu_prospective_backfill_counts { size_t commitments_seen, followups_seen, imported_pending, imported_expired, reanchored, skipped_existing; } hu_prospective_backfill_counts_t;`
  - `hu_error_t hu_prospective_v2_backfill(hu_allocator_t *alloc, hu_memory_t *mem, int64_t now, bool write, hu_prospective_backfill_counts_t *out);`
  - `human prospective backfill --db PATH [--write] [--now EPOCH]` prints one JSON line: `{"commitments_seen": N, "followups_seen": N, "imported_pending": N, "imported_expired": N, "reanchored": N, "skipped_existing": N, "written": true|false}`.
  - `scripts/prospective_backfill.py` exit codes: 0 done (manifest written), 2 refused (nothing written).
- Rules:
  - Pending commitments with `deadline > 0` and unsent delayed follow-ups become time rows.
  - An item overdue by more than 14 days is imported `expired`.
  - An item overdue by up to 14 days is imported `pending` with `due_at = now` (re-anchored; see "Spec gaps" #3).
  - A future item keeps its due time.
  - The whole run is one transaction. A dry run rolls it back, so its counts are exact, including dedupe.

- [ ] **Step 1: Write the failing C tests**

In `tests/test_prospective_v2.c`, add before `void run_prospective_v2_tests(void)`:

```c
static void v2_backfill_imports_expires_reanchors_and_dedupes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    /* Raw ledger rows (the Task 8 writers would already mirror them). */
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO commitments(contact_id,description,who,deadline,status,created_at) "
             "VALUES('" C1 "','call about the lease','me',%lld,'pending',1),"
             "('" C1 "','old promise','me',%lld,'pending',1),"
             "('" C1 "','future thing','me',%lld,'pending',1),"
             "('" C1 "','undated','me',0,'pending',1),"
             "('" C1 "','already done','me',%lld,'followed_up',1);"
             "INSERT INTO delayed_followups(contact_id,topic,scheduled_at,sent) VALUES"
             "('" C1 "','call about the lease',%lld,0),('" C1 "','sent one',%lld,1)",
             (long long)(NOW - 2 * 86400), (long long)(NOW - 20 * 86400),
             (long long)(NOW + 86400), (long long)(NOW - 86400), (long long)(NOW - 2 * 86400),
             (long long)(NOW - 86400));
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);

    hu_prospective_backfill_counts_t b;
    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, &mem, NOW, false, &b), HU_OK);
    HU_ASSERT_EQ(b.commitments_seen, (size_t)3); /* dated + pending only */
    HU_ASSERT_EQ(b.followups_seen, (size_t)1);   /* unsent only */
    HU_ASSERT_EQ(b.imported_pending, (size_t)2);
    HU_ASSERT_EQ(b.imported_expired, (size_t)1);
    HU_ASSERT_EQ(b.reanchored, (size_t)1);
    HU_ASSERT_EQ(b.skipped_existing, (size_t)1); /* the lease follow-up is the same intention */
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE cue_kind='time'"),
                 (int64_t)0); /* dry run rolled back */

    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, &mem, NOW, true, &b), HU_OK);
    HU_ASSERT_EQ(b.imported_pending + b.imported_expired, (size_t)3);
    HU_ASSERT_EQ(q_int(db, "SELECT due_at FROM prospective_memories WHERE "
                           "action='call about the lease'"),
                 NOW); /* re-anchored: one grace window from the backfill */
    HU_ASSERT_EQ(q_int(db, "SELECT fired FROM prospective_memories WHERE action='old promise'"),
                 (int64_t)3);
    HU_ASSERT_EQ(q_int(db, "SELECT due_at FROM prospective_memories WHERE action='future thing'"),
                 NOW + 86400);

    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, &mem, NOW, true, &b), HU_OK);
    HU_ASSERT_EQ(b.imported_pending + b.imported_expired, (size_t)0); /* idempotent */
    HU_ASSERT_EQ(b.skipped_existing, (size_t)4);
    HU_ASSERT_EQ(hu_prospective_v2_backfill(&alloc, NULL, NOW, true, &b),
                 HU_ERR_INVALID_ARGUMENT);
    mem.vtable->deinit(mem.ctx);
}
```

Register it last in `run_prospective_v2_tests`: `HU_RUN_TEST(v2_backfill_imports_expires_reanchors_and_dedupes);`.

In `tests/test_cli_prospective.c`, inside `cli_prospective_run_prints_the_probe_contract`, before `mem.vtable->deinit(mem.ctx);`, add:

```c
    char *bf[] = {"human", "prospective", "backfill", "--db", ":memory:", "--now", "1790000000"};
    HU_ASSERT_TRUE(hu_cli_prospective_parse(7, bf, &a));
    HU_ASSERT_EQ(a.op, HU_CLI_PM_BACKFILL);
    f = tmpfile();
    HU_ASSERT_EQ(hu_cli_prospective_run(&alloc, &mem, &a, NULL, 0, &j, f), HU_OK);
    read_all(f, buf, sizeof(buf));
    fclose(f);
    HU_ASSERT_STR_EQ(buf, "{\"commitments_seen\": 0, \"followups_seen\": 0, "
                          "\"imported_pending\": 0, \"imported_expired\": 0, \"reanchored\": 0, "
                          "\"skipped_existing\": 0, \"written\": false}\n");
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human_tests -j8 2>&1 | grep -m2 error`
Expected: FAIL with `unknown type name 'hu_prospective_backfill_counts_t'`.

- [ ] **Step 3: Implement the backfill**

In `include/human/memory/prospective_v2.h`, add `#include "human/memory.h"` after `#include "human/core/error.h"`. Inside the `#ifdef HU_ENABLE_SQLITE` block, before its `#endif`, add:

```c
typedef struct hu_prospective_backfill_counts {
    size_t commitments_seen; /* pending, deadline > 0 */
    size_t followups_seen;   /* unsent */
    size_t imported_pending, imported_expired;
    size_t reanchored;       /* overdue <= 14 days: due_at moved to `now` */
    size_t skipped_existing; /* a time row already stands for the intention */
} hu_prospective_backfill_counts_t;

/* One-time mirror of the ledger into time rows (spec §4.1, rollout step 2):
 * pending dated commitments and unsent delayed follow-ups. Overdue by more
 * than HU_PROSPECTIVE_BACKFILL_EXPIRE_S -> imported expired; overdue by less
 * -> imported pending with due_at = now (one grace window); future -> as
 * scheduled. One transaction: `write=false` rolls it back, so a dry run's
 * counts (dedupe included) are exact. Idempotent. */
hu_error_t hu_prospective_v2_backfill(hu_allocator_t *alloc, hu_memory_t *mem, int64_t now,
                                      bool write, hu_prospective_backfill_counts_t *out);
```

In `src/memory/prospective_v2.c`, add to the includes inside the `#ifdef`:

```c
#include "human/memory/sql_transaction.h"
#include "human/memory/superhuman.h"
#include <stdio.h>
```

and before the closing `#endif`:

```c
static hu_error_t pm_backfill_one(sqlite3 *db, const char *kind, int64_t id, const char *contact,
                                  const char *action, int64_t due, hu_prospective_source_t source,
                                  int64_t now, hu_prospective_backfill_counts_t *out) {
    size_t cl = strlen(contact);
    size_t al = strlen(action);
    if (cl == 0 || al == 0 || due <= 0)
        return HU_OK;
    hu_prospective_status_t st = HU_PM_PENDING;
    bool reanchored = false;
    if (now - due > HU_PROSPECTIVE_BACKFILL_EXPIRE_S) {
        st = HU_PM_EXPIRED;
    } else if (due < now) {
        due = now;
        reanchored = true;
    }
    char key[64];
    snprintf(key, sizeof(key), "%s:%lld", kind, (long long)id);
    bool inserted = false;
    hu_error_t e = hu_prospective_repo_upsert_time(db, contact, cl, action, al, due,
                                                   HU_PROSPECTIVE_TIME_GRACE_S, source, key, st,
                                                   now, &inserted);
    if (e != HU_OK)
        return e;
    if (!inserted)
        out->skipped_existing++;
    else if (st == HU_PM_EXPIRED)
        out->imported_expired++;
    else {
        out->imported_pending++;
        if (reanchored)
            out->reanchored++;
    }
    return HU_OK;
}

hu_error_t hu_prospective_v2_backfill(hu_allocator_t *alloc, hu_memory_t *mem, int64_t now,
                                      bool write, hu_prospective_backfill_counts_t *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!alloc || !mem || !out || now <= 0)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3 *db = hu_sqlite_memory_get_db(mem);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;
    hu_superhuman_commitment_t *cs = NULL;
    size_t cn = 0;
    hu_delayed_followup_t *fs = NULL;
    size_t fn = 0;
    hu_error_t err = hu_superhuman_commitment_list_due(mem, alloc, INT64_MAX, 1000000, &cs, &cn);
    if (err == HU_OK)
        err = hu_superhuman_delayed_followup_list_due(mem, alloc, INT64_MAX, &fs, &fn);
    hu_sql_txn_t txn;
    memset(&txn, 0, sizeof(txn));
    if (err == HU_OK)
        err = hu_sql_txn_begin(&txn, db);
    for (size_t i = 0; err == HU_OK && i < cn; i++) {
        if (cs[i].deadline <= 0)
            continue;
        out->commitments_seen++;
        err = pm_backfill_one(db, "commitment", cs[i].id, cs[i].contact_id, cs[i].description,
                              cs[i].deadline, HU_PM_SOURCE_PROMISE_KEEPER, now, out);
    }
    for (size_t i = 0; err == HU_OK && i < fn; i++) {
        out->followups_seen++;
        err = pm_backfill_one(db, "followup", fs[i].id, fs[i].contact_id, fs[i].topic,
                              fs[i].scheduled_at, HU_PM_SOURCE_FOLLOWUP, now, out);
    }
    if (txn.active) {
        if (err == HU_OK && write)
            err = hu_sql_txn_commit(&txn);
        else
            hu_sql_txn_rollback(&txn);
    }
    if (cs)
        hu_superhuman_commitment_free(alloc, cs, cn);
    if (fs)
        hu_superhuman_delayed_followup_free(alloc, fs, fn);
    return err;
}
```

In `src/app/cli_prospective.c`, in `hu_cli_prospective_run`, directly after `int64_t now = …;`, add:

```c
    if (a->op == HU_CLI_PM_BACKFILL) {
        hu_prospective_backfill_counts_t b;
        hu_error_t be = hu_prospective_v2_backfill(alloc, mem, now, a->write, &b);
        if (be != HU_OK)
            return be;
        fprintf(out,
                "{\"commitments_seen\": %zu, \"followups_seen\": %zu, \"imported_pending\": %zu, "
                "\"imported_expired\": %zu, \"reanchored\": %zu, \"skipped_existing\": %zu, "
                "\"written\": %s}\n",
                b.commitments_seen, b.followups_seen, b.imported_pending, b.imported_expired,
                b.reanchored, b.skipped_existing, a->write ? "true" : "false");
        return HU_OK;
    }
```

- [ ] **Step 4: Run the C tests**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cmake --build "$W/build" --target human human_tests -j8 && cd "$W" && ./build/human_tests --suite=prospective`
Expected: PASS. `prospective v2` passes 15/15 and `cli prospective` passes 3/3.

- [ ] **Step 5: Write the failing Python tests**

Create `tests/test_prospective_backfill.py`:

```python
"""Hermetic tests for scripts/prospective_backfill.py (spec 2026-09-30 §4.1, rollout step 2).

A fake `human` binary in tmp_path plays `human prospective backfill`; every
path (db, backups, manifests) is under tmp_path. Nothing reads ~/.human.
"""
import json
import os
import sqlite3
import stat
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

import prospective_backfill as pb  # noqa: E402

NOW = 1_790_000_000

FAKE = """#!/usr/bin/env python3
import json, os, sys
args = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as f:
    f.write(" ".join(args) + "\\n")
if os.environ.get("FAKE_FAIL"):
    sys.exit(3)
if "--write" in args and not os.listdir(os.environ["FAKE_BACKUP_DIR"]):
    sys.exit(4)  # a write before the backup exists is exactly what must never happen
print(json.dumps({"commitments_seen": 3, "followups_seen": 1, "imported_pending": 2,
                  "imported_expired": 1, "reanchored": 1, "skipped_existing": 1,
                  "written": "--write" in args}))
"""


def make_db(tmp_path, migrated=True):
    p = tmp_path / "memory.db"
    cols = ("id INTEGER PRIMARY KEY, trigger_type TEXT, trigger_value TEXT, action TEXT, "
            "contact_id TEXT, expires_at INTEGER, fired INTEGER DEFAULT 0, created_at INTEGER")
    if migrated:
        cols += (", cue_kind TEXT DEFAULT 'keyword', due_at INTEGER, status TEXT DEFAULT "
                 "'pending', surfaced_at INTEGER, attempts INTEGER DEFAULT 0, outcome TEXT, "
                 "source TEXT DEFAULT 'extractor'")
    con = sqlite3.connect(p)
    con.execute(f"CREATE TABLE prospective_memories({cols})")
    con.commit()
    con.close()
    return str(p)


def setup(tmp_path, monkeypatch, migrated=True):
    b = tmp_path / "human"
    b.write_text(FAKE)
    b.chmod(0o755)
    backups = tmp_path / "backups"
    monkeypatch.setenv("FAKE_LOG", str(tmp_path / "calls.log"))
    monkeypatch.setenv("FAKE_BACKUP_DIR", str(backups))
    logs = tmp_path / "logs"
    argv = ["--db", make_db(tmp_path, migrated), "--human-bin", str(b), "--now", str(NOW),
            "--backup-dir", str(backups), "--manifest-dir", str(logs)]
    return argv, backups, logs


def calls(tmp_path):
    p = tmp_path / "calls.log"
    return p.read_text().splitlines() if p.exists() else []


def test_dry_run_counts_without_a_backup(tmp_path, monkeypatch):
    argv, backups, logs = setup(tmp_path, monkeypatch)
    assert pb.main(argv) == 0
    assert calls(tmp_path) and "--write" not in calls(tmp_path)[0]
    assert not backups.exists() or not os.listdir(backups)
    [manifest] = list(logs.iterdir())
    assert stat.S_IMODE(manifest.stat().st_mode) == 0o600
    body = json.loads(manifest.read_text())
    assert body["mode"] == "dry_run" and body["backup_written"] is False
    assert body["counts"]["imported_pending"] == 2 and body["counts"]["written"] is False


def test_write_backs_up_before_the_backfill(tmp_path, monkeypatch):
    argv, backups, logs = setup(tmp_path, monkeypatch)
    assert pb.main(argv + ["--write"]) == 0
    [bak] = list(backups.iterdir())
    assert stat.S_IMODE(bak.stat().st_mode) == 0o600
    con = sqlite3.connect(bak)
    assert con.execute("SELECT COUNT(*) FROM sqlite_master WHERE "
                       "name='prospective_memories'").fetchone()[0] == 1
    con.close()
    assert "--write" in calls(tmp_path)[0]
    body = json.loads(next(logs.iterdir()).read_text())
    assert body["mode"] == "write" and body["backup_written"] is True


def test_refuses_an_unmigrated_database_and_writes_nothing(tmp_path, monkeypatch):
    argv, backups, logs = setup(tmp_path, monkeypatch, migrated=False)
    assert pb.main(argv + ["--write"]) == 2
    assert calls(tmp_path) == []
    assert not logs.exists() and not backups.exists()


def test_refuses_when_the_backfill_fails(tmp_path, monkeypatch):
    argv, _, logs = setup(tmp_path, monkeypatch)
    monkeypatch.setenv("FAKE_FAIL", "1")
    assert pb.main(argv) == 2
    assert not logs.exists()


def test_refuses_without_a_binary(tmp_path, monkeypatch):
    argv, _, logs = setup(tmp_path, monkeypatch)
    argv[argv.index("--human-bin") + 1] = str(tmp_path / "missing")
    assert pb.main(argv) == 2
    assert not logs.exists()


def test_manifest_carries_counts_only(tmp_path, monkeypatch):
    argv, _, logs = setup(tmp_path, monkeypatch)
    assert pb.main(argv) == 0
    body = json.loads(next(logs.iterdir()).read_text())
    assert set(body) == {"schema_version", "measured_at", "mode", "backup_written", "counts"}
    assert set(body["counts"]) == set(pb.KEYS)
```

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cd "$W" && python3 -m pytest -q tests/test_prospective_backfill.py`
Expected: FAIL with `ModuleNotFoundError: No module named 'prospective_backfill'`.

- [ ] **Step 6: Write the wrapper**

Create `scripts/prospective_backfill.py`:

```python
#!/usr/bin/env python3
"""One-time backfill of dated commitments and delayed follow-ups into
prospective_memories as cue_kind='time' rows (spec
docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.1,
rollout step 2).

The mirroring rules (14-day expiry, re-anchoring, dedupe of a commitment and
its paired follow-up) live in C, in hu_prospective_v2_backfill, which runs
through `human prospective backfill`. This wrapper owns the operational safety:

  * refuses (exit 2, writes nothing) when memory.db is missing, the table is
    not migrated (deploy a build with the v2 migration and let the daemon open
    the database once), or the human binary is missing;
  * --write first backs memory.db up with the SQLite online backup API to
    ~/.human/backups/memory.db.bak-prospective-<ts> (0600), and refuses if the
    backup fails;
  * writes a counts-only manifest (0600) to
    ~/.human/logs/prospective-backfill-<ts>.json.

The default is a dry run: the C side rolls its transaction back, so the counts
are exact and the database is unchanged.
"""
import argparse
import datetime as dt
import json
import os
import sqlite3
import subprocess
import sys
import time

HOME = os.path.expanduser("~")
KEYS = ("commitments_seen", "followups_seen", "imported_pending", "imported_expired",
        "reanchored", "skipped_existing", "written")
REQUIRED_COLUMNS = {"cue_kind", "due_at", "status", "surfaced_at", "attempts", "outcome",
                    "source"}


def refuse(msg):
    print(f"refusing: {msg}; nothing written", file=sys.stderr)
    return 2


def migrated(db_path):
    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    try:
        cols = {r[1] for r in con.execute("PRAGMA table_info(prospective_memories)")}
    finally:
        con.close()
    return REQUIRED_COLUMNS <= cols


def backup(db_path, backup_dir, stamp):
    """Consistent 0600 copy through the online backup API from a read-only
    connection. Raises OSError / sqlite3.Error on any failure."""
    os.makedirs(backup_dir, mode=0o700, exist_ok=True)
    dst = os.path.join(backup_dir, f"memory.db.bak-prospective-{stamp}")
    fd = os.open(dst, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    os.close(fd)
    src = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    try:
        d = sqlite3.connect(dst)
        try:
            src.backup(d)
            ok = d.execute("SELECT COUNT(*) FROM sqlite_master WHERE "
                           "name='prospective_memories'").fetchone()[0] == 1
        finally:
            d.close()
    finally:
        src.close()
    os.chmod(dst, 0o600)
    if not ok:
        os.unlink(dst)
        raise sqlite3.DatabaseError("backup has no prospective_memories table")
    return dst


def run_backfill(human_bin, db_path, write, now, timeout=300):
    cmd = [human_bin, "prospective", "backfill", "--db", db_path, "--now", str(now)]
    if write:
        cmd.append("--write")
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError(f"backfill exited {r.returncode}")
    lines = [ln for ln in r.stdout.splitlines() if ln.strip()]
    if not lines:
        raise RuntimeError("backfill printed nothing")
    counts = json.loads(lines[-1])
    if set(counts) != set(KEYS) or counts["written"] is not write:
        raise RuntimeError("backfill output does not match the contract")
    return counts


def write_manifest(out_dir, stamp, payload):
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"prospective-backfill-{stamp}.json")
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(payload, f, indent=2)
    return path


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", default=os.path.join(HOME, ".human/memory.db"))
    ap.add_argument("--human-bin", default=os.path.join(HOME, ".local/bin/human-daemon"))
    ap.add_argument("--write", action="store_true", help="apply (default: dry run)")
    ap.add_argument("--now", type=int, default=None, help="epoch seconds (tests)")
    ap.add_argument("--backup-dir", default=os.path.join(HOME, ".human/backups"))
    ap.add_argument("--manifest-dir", default=os.path.join(HOME, ".human/logs"))
    a = ap.parse_args(argv)
    now = a.now or int(time.time())
    stamp = dt.datetime.fromtimestamp(now, dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    if not (os.path.isfile(a.human_bin) and os.access(a.human_bin, os.X_OK)):
        return refuse(f"no executable human binary at {a.human_bin}")
    if not os.path.isfile(a.db):
        return refuse(f"no database at {a.db}")
    try:
        if not migrated(a.db):
            return refuse("prospective_memories is not migrated; deploy a build with the v2 "
                          "migration and let the daemon open the database once")
    except sqlite3.Error as e:
        return refuse(f"cannot read the database ({e.__class__.__name__})")
    backup_path = None
    if a.write:
        try:
            backup_path = backup(a.db, a.backup_dir, stamp)
        except (OSError, sqlite3.Error) as e:
            return refuse(f"backup failed ({e.__class__.__name__})")
    try:
        counts = run_backfill(a.human_bin, a.db, a.write, now)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as e:
        return refuse(f"backfill failed ({e})")
    payload = {"schema_version": 1, "measured_at": stamp,
               "mode": "write" if a.write else "dry_run",
               "backup_written": backup_path is not None, "counts": counts}
    path = write_manifest(a.manifest_dir, stamp, payload)
    print(json.dumps(counts))
    print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 7: Run the Python tests**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cd "$W" && python3 -m pytest -q tests/test_prospective_backfill.py`
Expected: PASS, 6 passed.

- [ ] **Step 8: Full suite, commit**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cd "$W" && ./build/human_tests 2>&1 | grep -E 'Results:' && bash scripts/check-clone-ratchet.sh && bash scripts/check-dead-strip-ratchet.sh`
Expected: 0 failed, and both gates pass.

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2
git -C "$W" add include/human/memory/prospective_v2.h src/memory/prospective_v2.c \
  src/app/cli_prospective.c tests/test_prospective_v2.c tests/test_cli_prospective.c \
  scripts/prospective_backfill.py tests/test_prospective_backfill.py
git -C "$W" commit -m "feat(prospective): one-time backfill of dated ledger items, backup first

hu_prospective_v2_backfill mirrors pending dated commitments and unsent
follow-ups as time rows in one transaction (dry run rolls back, so its counts
are exact): >14 days overdue imports expired, <=14 days re-anchors to now,
future keeps its time, pairs dedupe. scripts/prospective_backfill.py refuses
an unmigrated DB, backs up 0600 before any write, and records counts only.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 11: `scripts/pm_bench_local.py` — the scripted multi-day harness

**Files:**
- Create: `scripts/pm_bench_local.py`
- Test: `tests/test_pm_bench_local.py`

**Interfaces:**
- Consumes: the Task 6 probe contract: `human prospective init --db D` prints `ok`. `probe --full` prints the header line, then `item id=N verdict=V` lines, then the directive. `probe --deliver` prints `surfaced=… used=… ignored=… expired=…`. Also `--judge model` from Task 7.
- Produces: `~/.human/logs/pm-bench-local-<ts>.json` (0600), with `schema_version`, `measured_at`, `judge`, `scenarios`, `steps`, `counts`, `rates` and `thresholds`, plus `verdict` (`PASS`|`FAIL`). The exit code is:
  - 0 = PASS
  - 1 = FAIL (report written)
  - 2 = refused (nothing written)
  - 3 = inconclusive, when judge failures exceed 10% of judged items (nothing written)
- Functions the tests import: `build_scenarios()`, `validate(scenarios) -> dict` (counts; raises `ValueError`), `score(results) -> (counts, rates)` and `main(argv)`.
- Scenario set: 8 situations × {clean positive, overloaded positive (4 pending distractors), silent negative (resolved earlier in the thread), cancellation, reschedule (time), cross-day keyword (cue 5 days later), cross-day time (due day 3)} = 56 scenarios, 40 positive expectations, 48 silent steps and 16 cross-day expectations. `validate` refuses a set below 20 / 20 / 10, and a step that cues an intention it does not expect.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_pm_bench_local.py`:

```python
"""Hermetic tests for scripts/pm_bench_local.py (spec 2026-09-30 §3, §4.5).

The scenario set and the scoring math are tested directly. The end-to-end runs
use a fake `human` binary in tmp_path that plays the probe contract. Its
"always remind" policy is exactly the TriggerBench failure the harness exists
to catch, and a run with it must FAIL. Nothing touches ~/.human.
"""
import json
import stat
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

import pm_bench_local as pb  # noqa: E402

FAKE = r'''#!/usr/bin/env python3
import os, sqlite3, sys
a = sys.argv[2:]          # argv[1] == "prospective"
mode = os.environ.get("FAKE_MODE", "always")
def arg(k):
    return a[a.index(k) + 1] if k in a else None
db = arg("--db")
if a[0] == "init":
    con = sqlite3.connect(db)
    con.execute("CREATE TABLE IF NOT EXISTS prospective_memories(id INTEGER PRIMARY KEY "
                "AUTOINCREMENT, trigger_type TEXT, trigger_value TEXT, action TEXT, contact_id "
                "TEXT, expires_at INTEGER, fired INTEGER DEFAULT 0, created_at INTEGER, cue_kind "
                "TEXT DEFAULT 'keyword', due_at INTEGER, status TEXT DEFAULT 'pending', "
                "surfaced_at INTEGER, attempts INTEGER DEFAULT 0, outcome TEXT, source TEXT "
                "DEFAULT 'extractor')")
    con.commit()
    print("ok")
    sys.exit(0)
if mode == "crash":
    sys.exit(1)
if "--deliver" in a:
    print("surfaced=0 used=0 ignored=0 expired=0")
    sys.exit(0)
con = sqlite3.connect(db)
now = int(arg("--now"))
if "--tick" in a:
    ids = [r[0] for r in con.execute("SELECT id FROM prospective_memories WHERE cue_kind='time' "
                                     "AND status='pending' AND due_at<=?", (now,))]
else:
    text = arg("--inbound").lower()
    ids = [i for i, cue in con.execute("SELECT id, trigger_value FROM prospective_memories "
                                       "WHERE cue_kind='keyword' AND status='pending'")
           if cue.lower() in text]
ids = ids[:3]
verdict = "parse_fail" if mode == "garbled" else "fire"
fire = len(ids) if verdict == "fire" else 0
pf = len(ids) - fire
print(f"candidates={len(ids)} fire={fire} resolved=0 cancel=0 not_now=0 parse_fail={pf} "
      f"judge_err=0 expired=0 capped=0 bytes=0")
for i in ids:
    print(f"item id={i} verdict={verdict}")
'''


def fake_bin(tmp_path, monkeypatch, mode):
    b = tmp_path / "human"
    b.write_text(FAKE)
    b.chmod(0o755)
    monkeypatch.setenv("FAKE_MODE", mode)
    return str(b)


def test_committed_scenarios_meet_minimums_and_never_cross_cue():
    scenarios = pb.build_scenarios()
    counts = pb.validate(scenarios)
    assert len(scenarios) == 56
    assert counts["positive_expectations"] == 40
    assert counts["silent_steps"] == 48
    assert counts["cross_day_expectations"] == 16


def test_validate_refuses_a_step_that_cues_an_unexpected_intention():
    s = pb.build_scenarios()[1]  # overloaded-taco
    bad = dict(s)
    bad["steps"] = [dict(s["steps"][0], text=s["steps"][0]["text"] + " and the interview")]
    try:
        pb.validate([bad])
    except ValueError:
        return
    raise AssertionError("validate accepted a step cueing a distractor")


def test_score_math_on_a_hand_built_run():
    results = [
        {"expect": ["a"], "pred": ["a"], "tags": []},                    # TP
        {"expect": ["b"], "pred": [], "tags": ["cross_day"]},            # FN, cross-day miss
        {"expect": [], "pred": ["c"], "tags": ["silent"]},               # FP, silent false alarm
        {"expect": [], "pred": [], "tags": ["silent"]},                  # clean silent step
        {"expect": ["d"], "pred": ["d", "e"], "tags": ["update"]},       # TP + FP, update error
    ]
    counts, rates = pb.score(results)
    assert (counts["tp"], counts["fp"], counts["fn"]) == (2, 2, 1)
    assert rates["set_f1"] == 4 / 7  # 2*tp / (2*tp + fp + fn)
    assert rates["silent_negative_false_alarm"] == 0.5
    assert rates["cross_day_miss"] == 1.0
    assert rates["update_miss"] == 1.0
    assert rates["false_alarms_per_step"] == 2 / 5
    empty_counts, empty_rates = pb.score([])
    assert empty_rates["set_f1"] is None and empty_counts["steps"] == 0


def test_always_remind_fails_on_silent_negatives(tmp_path, monkeypatch):
    out = tmp_path / "logs"
    rc = pb.main(["--human-bin", fake_bin(tmp_path, monkeypatch, "always"), "--judge", "fire",
                  "--out-dir", str(out)])
    assert rc == 1
    [report] = list(out.iterdir())
    assert stat.S_IMODE(report.stat().st_mode) == 0o600
    body = json.loads(report.read_text())
    assert body["verdict"] == "FAIL"
    assert body["rates"]["silent_negative_false_alarm"] == 1.0
    text = report.read_text()
    for s in pb.SITUATIONS:  # counts only: no action, cue or conversation text
        assert s["action"] not in text and s["cue_text"] not in text


def test_probe_failure_refuses_and_writes_nothing(tmp_path, monkeypatch):
    out = tmp_path / "logs"
    rc = pb.main(["--human-bin", fake_bin(tmp_path, monkeypatch, "crash"), "--out-dir", str(out)])
    assert rc == 2
    assert not out.exists()


def test_judge_failures_make_the_run_inconclusive(tmp_path, monkeypatch):
    out = tmp_path / "logs"
    rc = pb.main(["--human-bin", fake_bin(tmp_path, monkeypatch, "garbled"), "--out-dir",
                  str(out)])
    assert rc == 3
    assert not out.exists()


def test_missing_binary_refuses(tmp_path):
    out = tmp_path / "logs"
    assert pb.main(["--human-bin", str(tmp_path / "nope"), "--out-dir", str(out)]) == 2
    assert not out.exists()
```

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cd "$W" && python3 -m pytest -q tests/test_pm_bench_local.py`
Expected: FAIL with `ModuleNotFoundError: No module named 'pm_bench_local'`.

- [ ] **Step 2: Write the harness**

Create `scripts/pm_bench_local.py`:

```python
#!/usr/bin/env python3
"""Local PM-Bench-style harness for prospective memory v2 (spec
docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §3, §4.5).

A scripted multi-day scenario set runs against the REAL C functions through
`human prospective probe --full` (hu_prospective_v2_run / _after_delivery):
one fresh fixture DB per scenario (`human prospective init`), each step at a
scripted clock (--now), history passed as a file. The Decide call is the local
model by default (--judge model: the configured provider, GLM on :8741 in
prod); --judge fire|not_now|… makes every call return that word.

Scenario classes (8 situations each): clean_positive, overloaded_positive
(4 pending distractors), silent_negative (resolved earlier in the thread),
cancellation, reschedule (time cue moved; the stale one must not fire, the
new one must), cross_day (keyword cue 5 days after it was noted; time cue due
on day 3).

Reports counts and rates only, no action, cue or conversation text, to
~/.human/logs/pm-bench-local-<ts>.json (0600):
  set_f1                       micro Set-F1 over every judged step's fired set
  false_alarms_per_step        steps with >= 1 fire not expected / judged steps
  silent_negative_false_alarm  steps tagged silent that fired anything / those steps
  cross_day_miss               cross-day expectations not fired / cross-day expectations
  update_miss                  reschedule steps whose fired set was wrong / those steps
  judge_failure_rate           (parse_fail + judge_err) / candidates

Verdict PASS iff set_f1 >= 0.80, silent_negative_false_alarm <= 0.05 and
cross_day_miss <= 0.10 (exit 0); otherwise FAIL (exit 1). Both write the report.
Refuses (exit 2, writes nothing) when the binary is missing, any probe exits
non-zero or breaks the --full contract, nothing was ever judged, or the
scenario set is below its minimums. Exit 3 (writes nothing) when judge
failures exceed 10% of judged items: that run measured the plumbing, not the
policy.
"""
import argparse
import datetime as dt
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile

HOME = os.path.expanduser("~")
BASE = 1_790_000_000  # fixed scenario epoch (2026-09-21 UTC); every step is an offset
CONTACT = "+15550100001"
GRACE = 3 * 86400
THRESHOLDS = {"set_f1_min": 0.80, "silent_false_alarm_max": 0.05, "cross_day_miss_max": 0.10,
              "judge_failure_max": 0.10}
MINIMUMS = {"positive_expectations": 20, "silent_steps": 20, "cross_day_expectations": 10}

HEADER = re.compile(r"^candidates=(\d+) fire=(\d+) resolved=(\d+) cancel=(\d+) not_now=(\d+) "
                    r"parse_fail=(\d+) judge_err=(\d+) expired=(\d+) capped=(\d+) bytes=(\d+)$")
HEADER_KEYS = ("candidates", "fire", "resolved", "cancel", "not_now", "parse_fail", "judge_err",
               "expired", "capped", "bytes")
ITEM = re.compile(r"^item id=(\d+) "
                  r"verdict=(fire|already_resolved|cancel|not_now|parse_fail|judge_err)$")
DELIVERED = re.compile(r"^surfaced=(\d+) used=(\d+) ignored=(\d+) expired=(\d+)$")

SITUATIONS = [
    {"key": "taco", "cue": "taco place", "action": "ask how the new taco place was",
     "setup": ["them: trying that new taco place friday", "me: nice, report back"],
     "cue_text": "ok the taco place was packed",
     "cue_text2": "the taco place again tonight lol",
     "cue_text3": "still thinking about that taco place",
     "resolved": ["them: taco place was amazing, the al pastor was unreal", "me: told you"],
     "cancel": ["them: the taco place closed down, forget it", "me: noo"],
     "push": "them: we pushed the taco place trip to next week",
     "reply": "wait how was the taco place, worth it?"},
    {"key": "interview", "cue": "interview", "action": "ask how her job interview went",
     "setup": ["them: bank interview on thursday, so nervous", "me: you got this"],
     "cue_text": "interview prep is killing me",
     "cue_text2": "ugh the interview",
     "cue_text3": "interview stuff all day",
     "resolved": ["them: interview went great, they offered me the job!", "me: lets gooo"],
     "cancel": ["them: they canceled the interview, hiring freeze", "me: that sucks"],
     "push": "them: the interview got moved to next tuesday",
     "reply": "how did the interview go??"},
    {"key": "dentist", "cue": "dentist",
     "action": "check if he booked the dentist appointment",
     "setup": ["them: my tooth has been killing me", "me: book the dentist already"],
     "cue_text": "ugh my dentist never calls back",
     "cue_text2": "the dentist thing is annoying",
     "cue_text3": "dentist again today maybe",
     "resolved": ["them: booked the dentist appointment for monday", "me: finally"],
     "cancel": ["them: nvm on the dentist, the tooth stopped hurting", "me: ok good"],
     "push": "them: the dentist moved me to next month",
     "reply": "did you ever get the dentist appointment booked?"},
    {"key": "guitar", "cue": "guitar", "action": "send her the guitar teacher's number",
     "setup": ["them: i want to get back into guitar",
               "me: my old teacher is great, ill send you his number"],
     "cue_text": "practiced guitar for an hour",
     "cue_text2": "my guitar is so out of tune",
     "cue_text3": "guitar question for you",
     "resolved": ["me: sent you the guitar teacher's number", "them: got it thanks"],
     "cancel": ["them: giving up on guitar lessons, dont need the number", "me: fair"],
     "push": "them: guitar lessons can wait till next month",
     "reply": "here's the guitar teacher's number: 555-0100"},
    {"key": "marathon", "cue": "marathon", "action": "ask how the marathon training is going",
     "setup": ["them: signed up for the chicago marathon", "me: that's huge"],
     "cue_text": "marathon playlist ideas?",
     "cue_text2": "my legs are dead from marathon stuff",
     "cue_text3": "marathon is in six weeks",
     "resolved": ["them: marathon training update, ran 18 miles today, all good",
                  "me: beast"],
     "cancel": ["them: pulled out of the marathon, knee is shot", "me: oh no"],
     "push": "them: deferring the marathon to spring",
     "reply": "how's the marathon training going?"},
    {"key": "moving", "cue": "moving", "action": "offer to help with the move on saturday",
     "setup": ["them: moving into the new place saturday", "me: need hands?"],
     "cue_text": "moving boxes everywhere",
     "cue_text2": "moving is the worst",
     "cue_text3": "so much moving stress",
     "resolved": ["me: i'll be there saturday to help with the move", "them: you're the best"],
     "cancel": ["them: moving got canceled, landlord extended our lease", "me: oh nice"],
     "push": "them: moving day got pushed to next weekend",
     "reply": "want help with the move saturday? i can bring the truck"},
    {"key": "lasagna", "cue": "lasagna", "action": "send the lasagna recipe",
     "setup": ["them: that lasagna you made was insane", "me: ill send you the recipe"],
     "cue_text": "craving lasagna again",
     "cue_text2": "lasagna night at our place?",
     "cue_text3": "attempting lasagna this weekend",
     "resolved": ["me: just sent you the lasagna recipe", "them: making it tonight"],
     "cancel": ["them: found a lasagna recipe online, dont worry about it", "me: cool"],
     "push": "them: lasagna attempt postponed, kitchen is torn up",
     "reply": "ok here's the lasagna recipe, go easy on the ricotta"},
    {"key": "biscuit", "cue": "biscuit", "action": "ask how biscuit's vet visit went",
     "setup": ["them: biscuit has a vet visit tomorrow, fingers crossed", "me: poor guy"],
     "cue_text": "biscuit is being so dramatic",
     "cue_text2": "biscuit stole a sock again",
     "cue_text3": "biscuit says hi",
     "resolved": ["them: biscuit's vet visit went fine, just allergies", "me: phew"],
     "cancel": ["them: vet canceled biscuit's appointment, rescheduling someday",
                "me: annoying"],
     "push": "them: biscuit's vet visit moved to next friday",
     "reply": "how'd biscuit's vet visit go?"},
]


class ProbeError(Exception):
    """A probe run that does not honor its contract. Messages never carry text."""


def day(d, h=0, m=0):
    return BASE + d * 86400 + h * 3600 + m * 60


def kw(s, created=None):
    return {"kind": "keyword", "key": s["key"], "cue": s["cue"], "action": s["action"],
            "created": day(0) if created is None else created}


def tm(s, due, key=None, action=None, created=None):
    return {"kind": "time", "key": key or s["key"], "action": action or s["action"], "due": due,
            "created": day(0) if created is None else created}


def step(t, op, text=None, history=(), expect=(), tags=(), add=None):
    return {"t": t, "op": op, "text": text, "history": list(history), "expect": list(expect),
            "tags": list(tags), "add": add}


def clean_positive(s):
    return {"id": f"clean-{s['key']}", "cls": "clean_positive", "intentions": [kw(s)], "steps": [
        step(day(1, 10), "inbound", s["cue_text"], s["setup"], [s["key"]]),
        step(day(1, 10, 5), "deliver", s["reply"]),
        step(day(2, 9), "inbound", s["cue_text2"], s["setup"] + ["me: " + s["reply"]], [],
             ["after_done"]),
    ]}


def overloaded_positive(s, others):
    return {"id": f"overloaded-{s['key']}", "cls": "overloaded_positive",
            "intentions": [kw(s)] + [kw(o) for o in others], "steps": [
                step(day(1, 10), "inbound", s["cue_text"], s["setup"], [s["key"]]),
            ]}


def silent_negative(s):
    hist = s["setup"] + s["resolved"]
    return {"id": f"silent-{s['key']}", "cls": "silent_negative", "intentions": [kw(s)], "steps": [
        step(day(1, 10), "inbound", s["cue_text"], hist, [], ["silent"]),
        step(day(2, 10), "inbound", s["cue_text2"], hist, [], ["silent"]),
        step(day(3, 10), "inbound", s["cue_text3"], hist, [], ["silent"]),
    ]}


def cancellation(s):
    hist = s["setup"] + s["cancel"]
    return {"id": f"cancel-{s['key']}", "cls": "cancellation", "intentions": [kw(s)], "steps": [
        step(day(1, 10), "inbound", s["cue_text"], hist, [], ["silent"]),
        step(day(2, 10), "inbound", s["cue_text2"], hist, [], ["silent"]),
    ]}


def reschedule(s):
    hist = s["setup"] + [s["push"]]
    moved = tm(s, day(4, 9), key=s["key"] + "-v2", action=s["action"] + " after the reschedule",
               created=day(1, 11))
    return {"id": f"reschedule-{s['key']}", "cls": "reschedule",
            "intentions": [tm(s, day(1, 9))], "steps": [
                step(day(1, 10), "tick", None, hist, [], ["silent", "update"]),
                step(day(1, 11), "add", add=moved),
                step(day(4, 10), "tick", None, hist, [s["key"] + "-v2"], ["update"]),
            ]}


def cross_day(s):
    return [
        {"id": f"crossday-kw-{s['key']}", "cls": "cross_day", "intentions": [kw(s)], "steps": [
            step(day(5, 12), "inbound", s["cue_text"], s["setup"], [s["key"]], ["cross_day"]),
        ]},
        {"id": f"crossday-time-{s['key']}", "cls": "cross_day", "intentions": [tm(s, day(3, 9))],
         "steps": [
             step(day(1, 10), "tick", None, s["setup"], [], []),
             step(day(3, 10), "tick", None, s["setup"], [s["key"]], ["cross_day"]),
             step(day(3, 10, 5), "deliver", s["reply"]),
             step(day(3, 15), "tick", None, s["setup"] + ["me: " + s["reply"]], [],
                  ["after_done"]),
         ]},
    ]


def build_scenarios():
    out = []
    n = len(SITUATIONS)
    for i, s in enumerate(SITUATIONS):
        others = [SITUATIONS[(i + k) % n] for k in range(1, 5)]
        out += [clean_positive(s), overloaded_positive(s, others), silent_negative(s),
                cancellation(s), reschedule(s)]
        out += cross_day(s)
    return out


def _cued(cue, text):
    return re.search(r"(?<![a-z0-9])" + re.escape(cue.lower()) + r"(?![a-z0-9])",
                     text.lower()) is not None


def validate(scenarios):
    """-> minimum counts. Raises ValueError for a step that cues an intention it
    does not expect (or does not cue one it expects), or a set below MINIMUMS."""
    counts = {k: 0 for k in MINIMUMS}
    for s in scenarios:
        cues = {i["key"]: i["cue"] for i in s["intentions"] if i["kind"] == "keyword"}
        for st in s["steps"]:
            if st["op"] == "inbound":
                for key, cue in cues.items():
                    hit = _cued(cue, st["text"])
                    quiet = "silent" in st["tags"] or "after_done" in st["tags"]
                    if hit and key not in st["expect"] and not quiet:
                        raise ValueError(f"scenario {s['id']}: a step cues an unexpected intention")
                    if key in st["expect"] and not hit:
                        raise ValueError(f"scenario {s['id']}: an expected intention is not cued")
            if st["op"] in ("inbound", "tick"):
                counts["positive_expectations"] += len(st["expect"])
                counts["silent_steps"] += 1 if "silent" in st["tags"] else 0
                if "cross_day" in st["tags"]:
                    counts["cross_day_expectations"] += len(st["expect"])
    for k, floor in MINIMUMS.items():
        if counts[k] < floor:
            raise ValueError(f"scenario set below its minimums ({k} {counts[k]} < {floor})")
    return counts


def score(results):
    """results: [{"expect": [...], "pred": [...], "tags": [...]}] for judged steps."""
    tp = fp = fn = fa_steps = silent = silent_fa = cd_exp = cd_miss = upd = upd_err = 0
    for r in results:
        e, p, tags = set(r["expect"]), set(r["pred"]), set(r["tags"])
        tp += len(e & p)
        fp += len(p - e)
        fn += len(e - p)
        fa_steps += 1 if p - e else 0
        if "silent" in tags:
            silent += 1
            silent_fa += 1 if p else 0
        if "cross_day" in tags:
            cd_exp += len(e)
            cd_miss += len(e - p)
        if "update" in tags:
            upd += 1
            upd_err += 1 if p != e else 0
    denom = 2 * tp + fp + fn
    counts = {"steps": len(results), "tp": tp, "fp": fp, "fn": fn,
              "steps_with_false_alarm": fa_steps, "silent_steps": silent,
              "silent_false_alarms": silent_fa, "cross_day_expectations": cd_exp,
              "cross_day_misses": cd_miss, "update_steps": upd, "update_errors": upd_err}
    rates = {"set_f1": (2 * tp / denom) if denom else None,
             "false_alarms_per_step": (fa_steps / len(results)) if results else None,
             "silent_negative_false_alarm": (silent_fa / silent) if silent else None,
             "cross_day_miss": (cd_miss / cd_exp) if cd_exp else None,
             "update_miss": (upd_err / upd) if upd else None}
    return counts, rates


def probe(a, args):
    r = subprocess.run([a.human_bin, "prospective"] + args, capture_output=True, text=True,
                       timeout=a.timeout)
    if r.returncode != 0:
        raise ProbeError(f"probe '{args[0]}' exited {r.returncode}")
    return r.stdout


def parse_full(out):
    lines = out.splitlines()
    m = HEADER.match(lines[0]) if lines else None
    if not m:
        raise ProbeError("probe header does not match the --full contract")
    h = dict(zip(HEADER_KEYS, (int(g) for g in m.groups())))
    items = [(int(im.group(1)), im.group(2)) for im in map(ITEM.match, lines[1:]) if im]
    if len(items) != h["candidates"] or sum(1 for _, v in items if v == "fire") != h["fire"]:
        raise ProbeError("probe items disagree with its header")
    return h, items


def seed(con, it):
    if it["kind"] == "keyword":
        con.execute("INSERT INTO prospective_memories(trigger_type,trigger_value,action,"
                    "contact_id,expires_at,fired,created_at) VALUES('keyword',?,?,?,?,0,?)",
                    (it["cue"], it["action"], CONTACT, it["created"] + 30 * 86400,
                     it["created"]))
    else:
        con.execute("INSERT INTO prospective_memories(trigger_type,trigger_value,action,"
                    "contact_id,expires_at,fired,created_at,cue_kind,due_at,status,source) "
                    "VALUES('time',?,?,?,?,0,?,'time',?,'pending','promise_keeper')",
                    ("bench:" + it["key"], it["action"], CONTACT, it["due"] + GRACE,
                     it["created"], it["due"]))
    con.commit()


def run_scenario(a, scn, scratch, idx):
    db = os.path.join(scratch, f"s{idx}.db")
    if probe(a, ["init", "--db", db]).strip() != "ok":
        raise ProbeError("init did not print ok")
    con = sqlite3.connect(db)
    by_action = {}
    results = []
    judge = {"candidates": 0, "parse_fail": 0, "judge_err": 0}
    try:
        for it in scn["intentions"]:
            seed(con, it)
            by_action[it["action"]] = it["key"]
        for k, st in enumerate(scn["steps"]):
            if st["op"] == "add":
                seed(con, st["add"])
                by_action[st["add"]["action"]] = st["add"]["key"]
                continue
            base = ["probe", "--db", db, "--contact", CONTACT, "--now", str(st["t"])]
            if st["op"] == "deliver":
                if not DELIVERED.match(probe(a, base + ["--deliver", st["text"]]).strip()):
                    raise ProbeError("deliver output does not match the contract")
                continue
            hist = os.path.join(scratch, f"s{idx}-h{k}.txt")
            with open(hist, "w") as f:
                f.write("".join(line + "\n" for line in st["history"]))
            args = base + ["--full", "--judge", a.judge, "--history", hist]
            args += ["--inbound", st["text"]] if st["op"] == "inbound" else ["--tick"]
            h, items = parse_full(probe(a, args))
            for key in judge:
                judge[key] += h[key]
            pred = set()
            for item_id, verdict in items:
                row = con.execute("SELECT action FROM prospective_memories WHERE id=?",
                                  (item_id,)).fetchone()
                if not row or row[0] not in by_action:
                    raise ProbeError("probe named an intention the scenario never seeded")
                if verdict == "fire":
                    pred.add(by_action[row[0]])
            results.append({"expect": st["expect"], "pred": sorted(pred), "tags": st["tags"]})
    finally:
        con.close()
    return results, judge


def refuse(msg):
    print(f"refusing: {msg}; nothing written", file=sys.stderr)
    return 2


def write_report(out_dir, stamp, payload):
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"pm-bench-local-{stamp}.json")
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(payload, f, indent=2)
    return path


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--human-bin", default=os.path.join(HOME, ".local/bin/human-daemon"))
    ap.add_argument("--judge", default="model",
                    choices=["model", "fire", "already_resolved", "cancel", "not_now"])
    ap.add_argument("--out-dir", default=os.path.join(HOME, ".human/logs"))
    ap.add_argument("--timeout", type=int, default=120)
    a = ap.parse_args(argv)
    if not (os.path.isfile(a.human_bin) and os.access(a.human_bin, os.X_OK)):
        return refuse(f"no executable human binary at {a.human_bin}")
    scenarios = build_scenarios()
    try:
        validate(scenarios)
    except ValueError as e:
        return refuse(str(e))
    scratch = tempfile.mkdtemp(prefix="pm-bench-")
    results = []
    judge = {"candidates": 0, "parse_fail": 0, "judge_err": 0}
    try:
        for i, scn in enumerate(scenarios):
            r, j = run_scenario(a, scn, scratch, i)
            results += r
            for k in judge:
                judge[k] += j[k]
    except (ProbeError, OSError, sqlite3.Error, subprocess.TimeoutExpired) as e:
        return refuse(f"scenario run failed ({e.__class__.__name__}: {e})")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    if judge["candidates"] == 0:
        return refuse("no intention was ever judged")
    jfr = (judge["parse_fail"] + judge["judge_err"]) / judge["candidates"]
    if jfr > THRESHOLDS["judge_failure_max"]:
        print(f"INCONCLUSIVE: judge failures {jfr:.2f} > {THRESHOLDS['judge_failure_max']}; "
              "nothing written", file=sys.stderr)
        return 3
    counts, rates = score(results)
    rates["judge_failure_rate"] = jfr
    counts.update(judge)
    ok = (rates["set_f1"] is not None and rates["set_f1"] >= THRESHOLDS["set_f1_min"]
          and rates["silent_negative_false_alarm"] is not None
          and rates["silent_negative_false_alarm"] <= THRESHOLDS["silent_false_alarm_max"]
          and rates["cross_day_miss"] is not None
          and rates["cross_day_miss"] <= THRESHOLDS["cross_day_miss_max"])
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    payload = {"schema_version": 1, "measured_at": stamp, "judge": a.judge,
               "scenarios": len(scenarios), "steps": counts["steps"], "counts": counts,
               "rates": rates, "thresholds": THRESHOLDS, "verdict": "PASS" if ok else "FAIL"}
    path = write_report(a.out_dir, stamp, payload)
    def fmt(x):
        return "null" if x is None else f"{x:.3f}"
    print(f"pm_bench_local: set_f1={fmt(rates['set_f1'])} "
          f"silent_fa={fmt(rates['silent_negative_false_alarm'])} "
          f"cross_day_miss={fmt(rates['cross_day_miss'])} verdict={payload['verdict']}")
    print(f"wrote {path}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Run the tests**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cd "$W" && python3 -m pytest -q tests/test_pm_bench_local.py`
Expected: PASS, 7 passed.

- [ ] **Step 4: Run the harness against the real probe with a scripted judge**

This proves the contract end to end without a model:

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; D=$(mktemp -d); python3 "$W/scripts/pm_bench_local.py" --human-bin "$W/build/human" --judge fire --out-dir "$D"; echo "exit=$?"; python3 -c "import json,glob; b=json.load(open(glob.glob('$D/*.json')[0])); print(b['verdict'], b['rates'])"; rm -rf "$D"`
Expected: `exit=1`, `FAIL`, and `silent_negative_false_alarm` 1.0. A judge that always says `fire` is the always-remind baseline and must fail. `--judge not_now` gives `set_f1` 0.0 and `FAIL` too.

Do **not** run `--judge model` here. It uses the configured provider (port 8741), which this plan never touches. That run is rollout step 2 in the guide (Task 14).

- [ ] **Step 5: Commit**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2
git -C "$W" add scripts/pm_bench_local.py tests/test_pm_bench_local.py
git -C "$W" commit -m "feat(prospective): pm_bench_local.py — scripted multi-day harness on the probe

56 scenarios (clean/overloaded positives, silent negatives, cancellations,
reschedules, cross-day keyword and time cues) run against the real C pass
through \`human prospective probe --full\`; Set-F1, silent false alarms,
cross-day and update misses, counts only, 0600. Refuses on any contract
break; inconclusive when the judge mostly failed.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 12: `scripts/prospective_shadow_report.py` — the seven §3 SHADOW numbers

**Files:**
- Create: `scripts/prospective_shadow_report.py`
- Test: `tests/test_prospective_shadow_report.py`

**Interfaces:**
- Consumes:
  - The Task 7/9 log lines in `~/.human/logs/service-loop-error.log`: dated `YYYY-MM-DDTHH:MM:SS INFO  [prospective] prospective (time )?shadow…`, written in local time by `hu_log_format_line`.
  - `~/.human/memory.db`, read-only (`prospective_memories`, `messages`).
  - `scripts/eval_prospective_memory.py` helpers (`inbound_by_contact`, `_word_re`).
- Produces: `~/.human/logs/prospective-shadow-<until>.json` (0600), counts and rates only.
  - `would_fire_per_day`
  - `resolved_before_cue_rate` (resolved / candidates)
  - `cue_to_fire_rate` (distinct intentions with a fire item / distinct cued intentions)
  - `uptake` (used / would_fire from the uptake lines)
  - `miss_rate` (cued intentions that never produced an item line / cued intentions, i.e. flow exited before the prompt builder)
  - `duplicate_rate` (intentions that would-fire on more than one day / intentions that would-fire)
  - the same set for time cues (`time_*`, misses = due in the window without a time item line)

  A rate whose denominator is 0 is `null`. Exit 0 on success; exit 2 (nothing written) when the log has no shadow line in the window, or the DB is missing or unmigrated.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_prospective_shadow_report.py`:

```python
"""Hermetic tests for scripts/prospective_shadow_report.py (spec 2026-09-30 §3).

A synthetic service log and a synthetic memory.db in tmp_path. Nothing reads
~/.human.
"""
import json
import sqlite3
import stat
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

import time  # noqa: E402

import pytest  # noqa: E402

import prospective_shadow_report as psr  # noqa: E402

C1 = "+15550000001"


@pytest.fixture(autouse=True)
def utc(monkeypatch):
    """Service-log stamps are LOCAL time; pin the zone so the window is exact."""
    monkeypatch.setenv("TZ", "UTC")
    time.tzset()
    yield
    monkeypatch.undo()
    time.tzset()


def make_db(tmp_path):
    p = tmp_path / "memory.db"
    con = sqlite3.connect(p)
    con.execute("CREATE TABLE prospective_memories(id INTEGER PRIMARY KEY, trigger_type TEXT, "
                "trigger_value TEXT, action TEXT, contact_id TEXT, expires_at INTEGER, fired "
                "INTEGER DEFAULT 0, created_at INTEGER, cue_kind TEXT DEFAULT 'keyword', due_at "
                "INTEGER, status TEXT DEFAULT 'pending', surfaced_at INTEGER, attempts INTEGER "
                "DEFAULT 0, outcome TEXT, source TEXT DEFAULT 'extractor')")
    con.execute("CREATE TABLE messages(id INTEGER PRIMARY KEY, session_id TEXT, role TEXT, "
                "content TEXT, created_at TEXT)")
    since = int(datetime(2026, 10, 1, tzinfo=timezone.utc).timestamp())
    rows = [  # id, cue, action, created
        (1, "taco place", "ask about tacos", since - 86400),
        (2, "tacos", "ask about tacos", since - 86400),
        (3, "lasagna", "send the recipe", since - 86400),
        (4, "guitar", "send the number", since - 86400),  # cued, never reached the builder
    ]
    for i, cue, action, created in rows:
        con.execute("INSERT INTO prospective_memories(id,trigger_type,trigger_value,action,"
                    "contact_id,expires_at,created_at) VALUES(?,?,?,?,?,0,?)",
                    (i, "keyword", cue, action, C1, created))
    con.execute("INSERT INTO prospective_memories(id,trigger_type,trigger_value,action,"
                "contact_id,expires_at,created_at,cue_kind,due_at) VALUES(9,'time',"
                "'commitment:1','call the vet',?,0,?,'time',?)",
                (C1, since - 86400, since + 3600))
    for text, ts in [("the taco place!", "2026-10-01 12:00:00"),
                     ("lasagna tonight", "2026-10-02 12:00:00"),
                     ("guitar time", "2026-10-02 13:00:00")]:
        con.execute("INSERT INTO messages(session_id,role,content,created_at) "
                    "VALUES(?, 'user', ?, ?)", (C1, text, ts))
    con.commit()
    con.close()
    return str(p)


LOG = """\
2026-10-01T12:00:05 INFO  [prospective] prospective shadow: candidates=1 fire=1 resolved=0 cancel=0 not_now=0 parse_fail=0 judge_err=0 expired=0 capped=0
2026-10-01T12:00:05 INFO  [prospective] prospective shadow item: id=1 verdict=fire
2026-10-01T12:00:09 INFO  [prospective] prospective shadow uptake: would_fire=1 used=1
2026-10-02T12:00:05 INFO  [prospective] prospective shadow: candidates=2 fire=1 resolved=1 cancel=0 not_now=0 parse_fail=0 judge_err=0 expired=0 capped=0
2026-10-02T12:00:05 INFO  [prospective] prospective shadow item: id=2 verdict=fire
2026-10-02T12:00:05 INFO  [prospective] prospective shadow item: id=3 verdict=already_resolved
2026-10-02T12:00:09 INFO  [prospective] prospective shadow uptake: would_fire=1 used=0
2026-10-02T12:00:10 INFO  [human] unrelated line
"""


def run(tmp_path, log_text=LOG):
    log = tmp_path / "service.log"
    log.write_text(log_text)
    out = tmp_path / "logs"
    rc = psr.main(["--log", str(log), "--memory-db", make_db(tmp_path), "--since",
                   "2026-10-01", "--until", "2026-10-03", "--out-dir", str(out)])
    return rc, out


def test_report_counts_and_rates(tmp_path):
    rc, out = run(tmp_path)
    assert rc == 0
    [f] = list(out.iterdir())
    assert stat.S_IMODE(f.stat().st_mode) == 0o600
    body = json.loads(f.read_text())
    c, r = body["counts"], body["rates"]
    assert c["candidates"] == 3 and c["fire"] == 2 and c["resolved"] == 1
    assert r["would_fire_per_day"] == 1.0            # 2 fires over 2 days
    assert r["resolved_before_cue_rate"] == 1 / 3
    assert c["cued_intentions"] == 3                 # tacos, recipe, number
    assert r["cue_to_fire_rate"] == 1 / 3            # only "ask about tacos" would fire
    assert r["uptake"] == 0.5
    assert c["missed_intentions"] == 1               # the guitar cue never reached the builder
    assert r["miss_rate"] == 1 / 3
    assert r["duplicate_rate"] == 1.0                # tacos would-fired on two days
    assert r["time_miss_rate"] == 1.0                # the vet call was due, never seen
    text = f.read_text()
    assert "ask about tacos" not in text and C1 not in text


def test_no_shadow_lines_refuses_and_writes_nothing(tmp_path):
    rc, out = run(tmp_path, "2026-10-01T12:00:00 INFO  [human] nothing here\n")
    assert rc == 2
    assert not out.exists()


def test_rates_with_empty_denominators_are_null():
    counts = {"days": 2, "fire": 0, "candidates": 0, "resolved": 0, "cued_intentions": 0,
              "fire_intentions": 0, "missed_intentions": 0, "dup_intentions": 0,
              "uptake_would_fire": 0, "uptake_used": 0, "time_fire": 0, "time_candidates": 0,
              "time_resolved": 0, "time_due_intentions": 0, "time_missed_intentions": 0}
    r = psr.rates_of(counts)
    assert r["would_fire_per_day"] == 0.0
    assert r["resolved_before_cue_rate"] is None and r["uptake"] is None
    assert r["cue_to_fire_rate"] is None and r["time_miss_rate"] is None
```

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cd "$W" && python3 -m pytest -q tests/test_prospective_shadow_report.py`
Expected: FAIL with `ModuleNotFoundError: No module named 'prospective_shadow_report'`.

- [ ] **Step 2: Write the report**

Create `scripts/prospective_shadow_report.py`:

```python
#!/usr/bin/env python3
"""The seven SHADOW numbers for prospective memory v2 (spec
docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §3).

Reads the daemon's dated service log (hu_log lines written by
src/daemon/daemon_prospective.c and daemon_prospective_time.c) and memory.db
READ-ONLY. For [--since, --until) it reports:

  would_fire_per_day        fire verdicts / days
  resolved_before_cue_rate  already_resolved verdicts / judged candidates
  cue_to_fire_rate          intentions with a fire verdict / intentions whose cue
                            arrived in an inbound text in the window
  uptake                    would-fires the delivered reply carried / would-fires
  miss_rate                 cued intentions with no item line at all / cued
                            intentions (the flow exited before the prompt builder)
  duplicate_rate            intentions that would-fired on more than one day /
                            intentions that would-fired
  time_*                    the same for time cues; time misses are intentions due
                            in the window with no "time shadow item" line

Counts and rates only (no text, no contact ids) to
~/.human/logs/prospective-shadow-<until>.json (0600). A rate with an empty
denominator is null. Refuses (exit 2, writes nothing) when the window holds no
shadow line, or memory.db is missing or not migrated.
"""
import argparse
import json
import os
import re
import sqlite3
import sys
import time
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import eval_prospective_memory as epm  # noqa: E402

HOME = os.path.expanduser("~")
LINE = re.compile(r"^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}) \S+\s+\[prospective\] "
                  r"prospective (time )?shadow(?: (item|uptake))?: (.*)$")
KV = re.compile(r"(\w+)=(\S+)")


def refuse(msg):
    print(f"refusing: {msg}; nothing written", file=sys.stderr)
    return 2


def parse_day(s):
    return int(datetime.strptime(s, "%Y-%m-%d").replace(tzinfo=timezone.utc).timestamp())


def local_epoch(stamp):
    """hu_log_format_line writes LOCAL time."""
    return int(time.mktime(time.strptime(stamp, "%Y-%m-%dT%H:%M:%S")))


def read_log(path, since, until):
    rows = []
    with open(path, errors="replace") as f:
        for line in f:
            m = LINE.match(line.rstrip("\n"))
            if not m:
                continue
            ts = local_epoch(m.group(1))
            if since <= ts < until:
                rows.append({"ts": ts, "time": bool(m.group(2)), "kind": m.group(3) or "summary",
                             "kv": dict(KV.findall(m.group(4)))})
    return rows


def intentions(con):
    """{id: (contact, action, cue_kind)}"""
    return {i: (c, a, k) for i, c, a, k in con.execute(
        "SELECT id, contact_id, action, cue_kind FROM prospective_memories")}


def cued_intentions(con, since, until):
    inbound = epm.inbound_by_contact(con, since)
    cued = set()
    for tid, cue, contact, action, created in con.execute(
            "SELECT id, trigger_value, contact_id, action, created_at FROM prospective_memories "
            "WHERE trigger_type='keyword' AND fired <> 2"):
        if not cue:
            continue
        rx = epm._word_re(cue)
        for ep, text in inbound.get(contact, ()):
            if max(created or 0, since) < ep < until and rx.search(text):
                cued.add((contact, action))
                break
    return cued


def rates_of(c):
    def ratio(n, d):
        return (n / d) if d else None
    return {"would_fire_per_day": c["fire"] / c["days"] if c["days"] else None,
            "resolved_before_cue_rate": ratio(c["resolved"], c["candidates"]),
            "cue_to_fire_rate": ratio(c["fire_intentions"], c["cued_intentions"]),
            "uptake": ratio(c["uptake_used"], c["uptake_would_fire"]),
            "miss_rate": ratio(c["missed_intentions"], c["cued_intentions"]),
            "duplicate_rate": ratio(c["dup_intentions"], c["fire_intentions"]),
            "time_would_send_per_day": c["time_fire"] / c["days"] if c["days"] else None,
            "time_resolved_rate": ratio(c["time_resolved"], c["time_candidates"]),
            "time_miss_rate": ratio(c["time_missed_intentions"], c["time_due_intentions"])}


def build(rows, con, since, until):
    ids = intentions(con)
    c = {"days": (until - since) / 86400, "fire": 0, "candidates": 0, "resolved": 0,
         "uptake_would_fire": 0, "uptake_used": 0, "time_fire": 0, "time_candidates": 0,
         "time_resolved": 0}
    seen = {False: set(), True: set()}
    fire_days = {}
    for r in rows:
        kv = r["kv"]
        if r["kind"] == "summary":
            p = "time_" if r["time"] else ""
            c[p + "fire"] += int(kv.get("fire", 0))
            c[p + "candidates"] += int(kv.get("candidates", 0))
            c[p + "resolved"] += int(kv.get("resolved", 0))
        elif r["kind"] == "uptake":
            c["uptake_would_fire"] += int(kv.get("would_fire", 0))
            c["uptake_used"] += int(kv.get("used", 0))
        elif r["kind"] == "item" and kv.get("id", "").isdigit():
            meta = ids.get(int(kv["id"]))
            if not meta:
                continue
            key = (meta[0], meta[1])
            seen[r["time"]].add(key)
            if kv.get("verdict") == "fire" and not r["time"]:
                fire_days.setdefault(key, set()).add(r["ts"] // 86400)
    cued = cued_intentions(con, since, until)
    due = {(ct, a) for ct, a in con.execute(
        "SELECT contact_id, action FROM prospective_memories WHERE cue_kind='time' AND "
        "due_at >= ? AND due_at < ?", (since, until))}
    c["cued_intentions"] = len(cued)
    c["fire_intentions"] = len(fire_days)
    c["missed_intentions"] = len(cued - seen[False])
    c["dup_intentions"] = sum(1 for d in fire_days.values() if len(d) > 1)
    c["time_due_intentions"] = len(due)
    c["time_missed_intentions"] = len(due - seen[True])
    return c


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log", default=os.path.join(HOME, ".human/logs/service-loop-error.log"))
    ap.add_argument("--memory-db", default=os.path.join(HOME, ".human/memory.db"))
    ap.add_argument("--since", required=True, help="YYYY-MM-DD (UTC midnight), inclusive")
    ap.add_argument("--until", required=True, help="YYYY-MM-DD (UTC midnight), exclusive")
    ap.add_argument("--out-dir", default=os.path.join(HOME, ".human/logs"))
    a = ap.parse_args(argv)
    since, until = parse_day(a.since), parse_day(a.until)
    if until <= since:
        return refuse("--until must be after --since")
    if not os.path.isfile(a.log):
        return refuse(f"no log at {a.log}")
    if not os.path.isfile(a.memory_db):
        return refuse(f"no database at {a.memory_db}")
    rows = read_log(a.log, since, until)
    if not rows:
        return refuse("no prospective shadow line in the window (is HU_PROSPECTIVE=shadow?)")
    con = sqlite3.connect(f"file:{a.memory_db}?mode=ro", uri=True)
    try:
        cols = {r[1] for r in con.execute("PRAGMA table_info(prospective_memories)")}
        if not {"cue_kind", "due_at", "status"} <= cols:
            return refuse("prospective_memories is not migrated")
        counts = build(rows, con, since, until)
    except sqlite3.Error as e:
        return refuse(f"cannot read the database ({e.__class__.__name__})")
    finally:
        con.close()
    payload = {"schema_version": 1, "since": a.since, "until": a.until, "counts": counts,
               "rates": rates_of(counts)}
    os.makedirs(a.out_dir, exist_ok=True)
    path = os.path.join(a.out_dir, f"prospective-shadow-{a.until}.json")
    if os.path.exists(path):
        os.unlink(path)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(payload, f, indent=2)
    print(json.dumps(payload["rates"]))
    print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Run the tests**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cd "$W" && python3 -m pytest -q tests/test_prospective_shadow_report.py`
Expected: PASS, 3 passed. The autouse fixture pins `TZ=UTC`, because the log stamps are local time and the window is UTC.

- [ ] **Step 4: Commit**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2
git -C "$W" add scripts/prospective_shadow_report.py tests/test_prospective_shadow_report.py
git -C "$W" commit -m "feat(prospective): shadow report — the seven §3 SHADOW numbers

would-fire/day, resolved-before-cue, cue->fire, uptake, misses before the
prompt builder, duplicate rate, and the time-cue twins, from the dated
service log plus a read-only memory.db. Counts and rates only; null on an
empty denominator; refuses a window with no shadow line.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 13: `scripts/prospective_spot_check.py` — the blind precision check for promotion

§3's promotion bar is "30 SHADOW would-fires spot-checked blind … precision ≥ 0.8". The existing rating-sheet flow (`scripts/build_rule_preference_sheet.py` → `scripts/blind_ab/score_preference.py`) asks an A-vs-B preference, and precision needs a yes/no per item. This script builds the same kind of local sheet (`rating_sheet.csv` + private `answer_key.json` + `README.md`) for a yes/no question. Would-fires are mixed with an equal number of held items (resolved / not_now / cancel), so the rater cannot tell which is which. It then scores precision on the would-fires. The sheet holds real conversation lines, so it stays on this machine, in a 0700 directory with 0600 files.

**Files:**
- Create: `scripts/prospective_spot_check.py`
- Test: `tests/test_prospective_spot_check.py`

**Interfaces:**
- Consumes: the Task 7 `prospective shadow item` log lines, and read-only `prospective_memories` + `messages` (the last 6 turns before each item's timestamp).
- Produces:
  - `build` writes `<out>/rating_sheet.csv` (`id,context,reminder,answer`), `<out>/answer_key.json` (`{"id": "fire"|"hold"}`) and `<out>/README.md`.
  - `score` prints `{"fire_rated", "fire_yes", "precision", "hold_rated", "hold_yes", "pass"}`.
  - `build` exits 2 when fewer than 30 would-fires exist in the window. `score` exits 2 when fewer than 30 would-fires are answered.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_prospective_spot_check.py`:

```python
"""Hermetic tests for scripts/prospective_spot_check.py (spec 2026-09-30 §3 promotion).

A synthetic log and memory.db in tmp_path; nothing reads ~/.human.
"""
import csv
import json
import stat
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

import prospective_spot_check as psc  # noqa: E402


def make_db(tmp_path, n):
    p = tmp_path / "memory.db"
    con = sqlite3.connect(p)
    con.execute("CREATE TABLE prospective_memories(id INTEGER PRIMARY KEY, trigger_type TEXT, "
                "trigger_value TEXT, action TEXT, contact_id TEXT, created_at INTEGER)")
    con.execute("CREATE TABLE messages(id INTEGER PRIMARY KEY, session_id TEXT, role TEXT, "
                "content TEXT, created_at TEXT)")
    for i in range(1, 2 * n + 1):
        con.execute("INSERT INTO prospective_memories VALUES(?, 'keyword', 'cue', ?, ?, 0)",
                    (i, f"action {i}", f"+1555000{i:04d}"))
        con.execute("INSERT INTO messages(session_id,role,content,created_at) VALUES(?, 'user', "
                    "?, '2026-10-01 00:00:00')", (f"+1555000{i:04d}", f"hello {i}"))
    con.commit()
    con.close()
    return str(p)


def make_log(tmp_path, n):
    lines = []
    for i in range(1, 2 * n + 1):
        verdict = "fire" if i <= n else "not_now"
        lines.append(f"2026-10-02T12:{i % 60:02d}:00 INFO  [prospective] prospective shadow "
                     f"item: id={i} verdict={verdict}")
    p = tmp_path / "service.log"
    p.write_text("\n".join(lines) + "\n")
    return str(p)


def build(tmp_path, n):
    out = tmp_path / "sheet"
    rc = psc.main(["build", "--log", make_log(tmp_path, n), "--memory-db", make_db(tmp_path, n),
                   "--since", "2026-10-01", "--until", "2026-10-03", "--out-dir", str(out),
                   "--seed", "7"])
    return rc, out


def test_build_mixes_fire_and_hold_blind(tmp_path):
    rc, out = build(tmp_path, 30)
    assert rc == 0
    assert stat.S_IMODE(out.stat().st_mode) == 0o700
    rows = list(csv.DictReader(open(out / "rating_sheet.csv")))
    key = json.loads((out / "answer_key.json").read_text())
    assert len(rows) == 60 and sorted(set(key.values())) == ["fire", "hold"]
    assert set(rows[0]) == {"id", "context", "reminder", "answer"}
    assert "verdict" not in (out / "rating_sheet.csv").read_text()
    for f in ("rating_sheet.csv", "answer_key.json", "README.md"):
        assert stat.S_IMODE((out / f).stat().st_mode) == 0o600


def test_build_refuses_below_thirty_would_fires(tmp_path):
    rc, out = build(tmp_path, 29)
    assert rc == 2 and not out.exists()


def test_score_precision_and_refusal(tmp_path):
    rc, out = build(tmp_path, 30)
    key = json.loads((out / "answer_key.json").read_text())
    rows = list(csv.DictReader(open(out / "rating_sheet.csv")))
    fire_ids = [r["id"] for r in rows if key[r["id"]] == "fire"]
    for r in rows:  # 25 of 30 would-fires judged right; every hold rated no
        r["answer"] = "yes" if r["id"] in fire_ids[:25] else "no"
    with open(out / "rating_sheet.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["id", "context", "reminder", "answer"])
        w.writeheader()
        w.writerows(rows)
    res = psc.score_sheet(str(out))
    assert res["fire_rated"] == 30 and res["fire_yes"] == 25
    assert abs(res["precision"] - 25 / 30) < 1e-9 and res["pass"] is True
    assert res["hold_yes"] == 0
    for r in rows[:40]:
        r["answer"] = ""
    with open(out / "rating_sheet.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["id", "context", "reminder", "answer"])
        w.writeheader()
        w.writerows(rows)
    assert psc.main(["score", "--out-dir", str(out)]) == 2  # fewer than 30 answered would-fires
```

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cd "$W" && python3 -m pytest -q tests/test_prospective_spot_check.py`
Expected: FAIL with `ModuleNotFoundError`.

- [ ] **Step 2: Write the script**

Create `scripts/prospective_spot_check.py`:

```python
#!/usr/bin/env python3
"""Blind spot check of SHADOW would-fires — the §3 promotion bar for
HU_PROSPECTIVE=live (spec
docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §3, §6 step 4):
"30 SHADOW would-fires spot-checked blind … precision >= 0.8".

  build  From the dated "prospective shadow item" log lines in the window,
         take the most recent would-fires (verdict fire, one per intention)
         and an equal number of held items (already_resolved / not_now /
         cancel), shuffle them (--seed), and write into --out-dir (0700):
           rating_sheet.csv  id, context (last 6 turns before the item),
                             reminder, answer (blank: fill yes/no)
           answer_key.json   {id: "fire" | "hold"}  — do not open while rating
           README.md         the question and the scoring command
         Refuses (exit 2, writes nothing) below 30 would-fires.
  score  precision = yes / answered among would-fires; pass iff >= 0.8 on
         >= 30 answered would-fires (exit 2 otherwise). The hold rows'
         yes-rate is reported to expose a rater who says yes to everything.

The sheet holds real conversation lines: it never leaves this machine.
"""
import argparse
import csv
import json
import os
import random
import re
import sqlite3
import sys
import time
from datetime import datetime, timezone
from io import StringIO

HOME = os.path.expanduser("~")
MIN_FIRE = 30
PRECISION_BAR = 0.8
ITEM = re.compile(r"^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}) \S+\s+\[prospective\] "
                  r"prospective shadow item: id=(\d+) verdict=(\w+)$")
QUESTION = ("Seth is about to reply to this conversation. Would it be natural and welcome for "
            "him to bring up the reminder now? Answer yes or no.")


def refuse(msg):
    print(f"refusing: {msg}; nothing written", file=sys.stderr)
    return 2


def parse_day(s):
    return int(datetime.strptime(s, "%Y-%m-%d").replace(tzinfo=timezone.utc).timestamp())


def read_items(path, since, until):
    """-> {id: (ts, verdict)}, the latest line per id wins."""
    items = {}
    with open(path, errors="replace") as f:
        for line in f:
            m = ITEM.match(line.rstrip("\n"))
            if not m:
                continue
            ts = int(time.mktime(time.strptime(m.group(1), "%Y-%m-%dT%H:%M:%S")))
            if since <= ts < until:
                items[int(m.group(2))] = (ts, m.group(3))
    return items


def context(con, contact, ts, n=6):
    cutoff = datetime.fromtimestamp(ts, timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
    rows = con.execute("SELECT role, content FROM messages WHERE session_id=? AND "
                       "created_at <= ? ORDER BY created_at DESC, id DESC LIMIT ?",
                       (contact, cutoff, n)).fetchall()
    return "\n".join(f"{'them' if r == 'user' else 'me'}: {c}" for r, c in reversed(rows))


def private_write(path, text):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w", newline="") as f:
        f.write(text)


def build(a):
    since, until = parse_day(a.since), parse_day(a.until)
    items = read_items(a.log, since, until)
    con = sqlite3.connect(f"file:{a.memory_db}?mode=ro", uri=True)
    try:
        meta = {i: (c, act) for i, c, act in con.execute(
            "SELECT id, contact_id, action FROM prospective_memories")}
        fire, hold, seen = [], [], set()
        for iid, (ts, verdict) in sorted(items.items(), key=lambda kv: -kv[1][0]):
            if iid not in meta or meta[iid] in seen:
                continue
            seen.add(meta[iid])
            (fire if verdict == "fire" else hold).append((iid, ts))
        if len(fire) < MIN_FIRE:
            return refuse(f"only {len(fire)} would-fires in the window (need {MIN_FIRE})")
        fire, hold = fire[:MIN_FIRE], hold[:MIN_FIRE]
        rows, key = [], {}
        for label, group in (("fire", fire), ("hold", hold)):
            for iid, ts in group:
                contact, action = meta[iid]
                rid = f"r{len(rows) + 1:03d}"
                rows.append({"id": rid, "context": context(con, contact, ts),
                             "reminder": action, "answer": ""})
                key[rid] = label
    finally:
        con.close()
    random.Random(a.seed).shuffle(rows)
    for n, r in enumerate(rows, 1):  # re-number after the shuffle so ids leak no order
        old = r["id"]
        r["id"] = f"q{n:03d}"
        key[r["id"]] = key.pop(old)
    os.makedirs(a.out_dir, mode=0o700)
    os.chmod(a.out_dir, 0o700)
    buf = StringIO()
    w = csv.DictWriter(buf, fieldnames=["id", "context", "reminder", "answer"])
    w.writeheader()
    w.writerows(rows)
    private_write(os.path.join(a.out_dir, "rating_sheet.csv"), buf.getvalue())
    private_write(os.path.join(a.out_dir, "answer_key.json"), json.dumps(key, indent=2))
    private_write(os.path.join(a.out_dir, "README.md"),
                  f"# Prospective reminder spot check\n\n{QUESTION}\n\nFill the `answer` column "
                  f"of rating_sheet.csv with yes or no. Do not open answer_key.json.\n\nScore: "
                  f"`python3 scripts/prospective_spot_check.py score --out-dir {a.out_dir}`\n")
    print(f"wrote {len(rows)} rows to {a.out_dir}")
    return 0


def score_sheet(out_dir):
    key = json.load(open(os.path.join(out_dir, "answer_key.json")))
    res = {"fire_rated": 0, "fire_yes": 0, "hold_rated": 0, "hold_yes": 0}
    with open(os.path.join(out_dir, "rating_sheet.csv"), newline="") as f:
        for r in csv.DictReader(f):
            ans = (r.get("answer") or "").strip().lower()
            if ans not in ("yes", "no") or r["id"] not in key:
                continue
            p = key[r["id"]]
            res[f"{p}_rated"] += 1
            res[f"{p}_yes"] += 1 if ans == "yes" else 0
    res["precision"] = res["fire_yes"] / res["fire_rated"] if res["fire_rated"] else None
    res["hold_yes_rate"] = res["hold_yes"] / res["hold_rated"] if res["hold_rated"] else None
    res["pass"] = (res["fire_rated"] >= MIN_FIRE and res["precision"] is not None
                   and res["precision"] >= PRECISION_BAR)
    return res


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--log", default=os.path.join(HOME, ".human/logs/service-loop-error.log"))
    b.add_argument("--memory-db", default=os.path.join(HOME, ".human/memory.db"))
    b.add_argument("--since", required=True)
    b.add_argument("--until", required=True)
    b.add_argument("--out-dir", required=True)
    b.add_argument("--seed", type=int, default=None)
    s = sub.add_parser("score")
    s.add_argument("--out-dir", required=True)
    a = ap.parse_args(argv)
    if a.cmd == "build":
        if os.path.exists(a.out_dir):
            return refuse(f"{a.out_dir} already exists")
        if not (os.path.isfile(a.log) and os.path.isfile(a.memory_db)):
            return refuse("log or memory.db missing")
        return build(a)
    res = score_sheet(a.out_dir)
    print(json.dumps(res))
    if res["fire_rated"] < MIN_FIRE:
        return refuse(f"only {res['fire_rated']} would-fires answered (need {MIN_FIRE})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Run the tests**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cd "$W" && python3 -m pytest -q tests/test_prospective_spot_check.py`
Expected: PASS, 3 passed.

- [ ] **Step 4: Commit**

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2
git -C "$W" add scripts/prospective_spot_check.py tests/test_prospective_spot_check.py
git -C "$W" commit -m "feat(prospective): blind yes/no spot check of SHADOW would-fires

The §3 promotion bar needs precision on 30 would-fires; the existing sheet
flow is A/B preference only. build mixes 30 would-fires with 30 held items
(0700 dir, 0600 files, never leaves the machine); score reports precision and
the hold yes-rate, and refuses below 30 answered would-fires.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 14: Operator guide and CI line

**Files:**
- Create: `docs/guides/prospective-memory.md`
- Modify: `.github/workflows/ci.yml` (the `capability-gate-check` job, after the "Insight-stream + curator + second-opinion pins" step ~:1228)

- [ ] **Step 1: Add the CI step**

In `.github/workflows/ci.yml`, directly after the step named `Insight-stream + curator + second-opinion pins (hermetic, no chat.db, no model)`, add:

```yaml
      # Prospective memory v2 tooling (spec 2026-09-30): fake `human` binary,
      # temp sqlite, temp logs — never ~/.human, chat.db or a model.
      - name: Prospective memory v2 harness + backfill + shadow report + spot check (hermetic)
        run: python3 -m pytest -q tests/test_pm_bench_local.py tests/test_prospective_backfill.py tests/test_prospective_shadow_report.py tests/test_prospective_spot_check.py
```

`pytest` is already installed earlier in that job, by the "Install pytest for the persona-evolution suite" step. The C tests run in the existing `build-and-test` jobs through `human_tests`.

- [ ] **Step 2: Write the guide**

Create `docs/guides/prospective-memory.md`:

````markdown
---
title: Prospective memory v2 — gates, backfill, harness and promotion
created: 2026-09-30
status: operator-facing
spec: docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md
---

# Prospective memory v2

How h-uman keeps promises and follows up, and how to move it from off to live.
Design: [spec](../superpowers/specs/2026-09-30-prospective-memory-v2-design.md).
Build: [plan](../superpowers/plans/2026-09-30-prospective-memory-v2.md).

Every intention lives in `prospective_memories`. The nightly curator writes
keyword intentions. The promise keeper, the inbound commitment keeper and the
dated check-ins add time intentions. Before anything surfaces, code filters it
(cue or due time, a 3-day grace, one time cue per contact per day, never in a
group chat or the owner's self-chat). Then the local model decides whether to
`fire`, mark it `already_resolved`, `cancel` it, or hold it for `not_now`. An
intention is `done` only when the delivered reply actually carries it.

## Gate 1: `HU_PROSPECTIVE` (reactive keyword path, daemon plist)

| Value | Effect |
|---|---|
| `off` (default) | Today's directive, byte-identical: `[PROSPECTIVE MEMORY: Remember to: …]`, marked `fired=1` at render. Logs once: `prospective v2 disabled (HU_PROSPECTIVE unset or off); set HU_PROSPECTIVE=shadow|live …`. |
| `shadow` | Today's directive unchanged. Filter + Decide also run read-only, up to 3 model calls on a turn whose text cued an intention, and log `prospective shadow: candidates=… fire=… resolved=… cancel=… not_now=… parse_fail=… judge_err=… expired=… capped=…`, one `prospective shadow item: id=… verdict=…` per judged intention, and `prospective shadow uptake: would_fire=… used=…` after delivery. |
| `live` | The v2 pass replaces it: `[PROSPECTIVE MEMORY: If it fits naturally, you could bring up: …]`. The intention becomes `surfaced`, and `done` only when the delivered reply contains its key terms. Otherwise it gets one more attempt and then `expired`. A model error or unparseable answer shows nothing. |

## Gate 2: `HU_PROSPECTIVE_TIME` (time cues that start a message)

| Value | Effect |
|---|---|
| `off` (default) | The legacy producers: the commitment follow-up lines and the proposer's `due_followups` section. |
| `shadow` | Legacy output unchanged. One read-only v2 time pass per contact per day logs `prospective time shadow: …`. |
| `live` | The commitment follow-up lines stop, because commitments are mirrored into the typed store. `due_followups` comes from the per-contact due set, at most one per contact per day. The item is settled against the proactive text that was actually sent. |

`HU_PROSPECTIVE_TIME` stays in `shadow` until `HU_PROSPECTIVE` has been promoted.

## Rollout (spec §6)

1. **Merge with both gates off. Deploy** with `scripts/install-human-daemon.sh` and verify with `scripts/verify-deploy.sh <commit>`. The daemon's first open migrates `memory.db` (the new columns plus the `fired → status` mapping).
2. **Backfill and harness.**
   - `python3 scripts/prospective_backfill.py` is a dry run: it reports exact counts and writes nothing.
   - `python3 scripts/prospective_backfill.py --write` takes a 0600 backup under `~/.human/backups/` first, and writes a counts-only manifest to `~/.human/logs/prospective-backfill-<ts>.json`.
   - Items overdue by more than 14 days import as expired. Items overdue by up to 14 days import pending, re-anchored to the backfill time.
   - `python3 scripts/pm_bench_local.py` runs 56 scripted scenarios against the real probe, with the local model as judge. PASS needs Set-F1 ≥ 0.80, silent-negative false alarms ≤ 5% and cross-day misses ≤ 10%. Exit 3 means the judge failed on more than 10% of items (for example it answered in Seth's voice), and nothing was measured.
3. **`HU_PROSPECTIVE=shadow` for 7 days**, then `python3 scripts/prospective_shadow_report.py --since <day1> --until <day8>`. That gives would-fire/day, resolved-before-cue, cue→fire, uptake, misses before the prompt builder, and duplicate rate, for keyword and time cues.
4. **Spot-check 30 would-fires blind.**
   - Build the sheet: `python3 scripts/prospective_spot_check.py build --since … --until … --out-dir ~/.human/prospective-spot-check-<date>`.
   - Answer the sheet yes/no without opening the key.
   - Score it: `python3 scripts/prospective_spot_check.py score --out-dir …`.
   - Set `HU_PROSPECTIVE=live` only if the harness passed **and** precision ≥ 0.8.
5. **Then `HU_PROSPECTIVE_TIME=shadow` for 7 days**, and follow the same pattern before `live`.

Rollback at any step: unset the gate (or set `off`) and reinstall the plist. Off is the pre-v2 path.

## The probe

`human prospective probe` runs the daemon's own functions against a fixture database. `--db` is required; it never opens `~/.human/memory.db` by default.

```bash
D=$(mktemp -d)
human prospective init --db "$D/pm.db"
human prospective probe --full --db "$D/pm.db" --contact +15550100001 --now 1790000000 \
  --inbound "the taco place was packed" --history "$D/history.txt" --judge model
human prospective probe --db "$D/pm.db" --contact +15550100001 --deliver "how was the taco place?"
```

Output contract: a `candidates=… bytes=…` header, then with `--full` one `item id=… verdict=…` line per judged intention and the directive text. `--deliver` prints `surfaced=… used=… ignored=… expired=…`.

## Known limits

- SHADOW calls the model on cued turns, adding latency before those replies. The cost is bounded at 3 calls of at most 16 tokens each.
- A reply sent as a voice memo or a tapback never reaches the delivered-text hook. Its intention counts as an attempt and retries once.
- Time cues are only considered on ticks where the contact already passed the proactive check-in gate. The shadow report's `time_miss_rate` measures what that misses.
- An action whose words are all short or non-ASCII has no key terms. It can never be proven done, so it expires after two attempts.
````

- [ ] **Step 3: Docs gate**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cd "$W" && bash scripts/doc-fleet.sh 2>&1 | tail -5`
Expected: passes, with frontmatter and markdown links resolving. If `doc-fleet.sh` requires the guide to be indexed (for example in `docs/CONCEPT_INDEX.md` or `docs/guides/README.md`), add one line there that names `docs/guides/prospective-memory.md`, and re-run.

- [ ] **Step 4: Final full verification and commit**

Run: `W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2; cd "$W" && cmake --build build --target human human_tests -j8 && ./build/human_tests 2>&1 | grep -E 'Results:' && python3 -m pytest -q tests/test_pm_bench_local.py tests/test_prospective_backfill.py tests/test_prospective_shadow_report.py tests/test_prospective_spot_check.py tests/test_insight_stream_*.py && bash scripts/check-dead-strip-ratchet.sh && bash scripts/check-file-size-ceiling.sh`
Expected: 0 failed C tests, every pytest passes, and both gates pass.

```bash
W=/Users/sethford/Projects/h-uman/.claude/worktrees/prospective-v2
git -C "$W" add docs/guides/prospective-memory.md .github/workflows/ci.yml
git -C "$W" commit -m "docs(prospective): operator guide for the v2 gates, backfill and promotion; CI step

The guide covers HU_PROSPECTIVE / HU_PROSPECTIVE_TIME states, the rollout
(backfill with backup, harness, 7-day shadow report, blind spot check) and
the probe. CI runs the four hermetic Python suites in capability-gate-check.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Hand the branch to the verifier (`/verify`) with the Global Constraints as the contract before opening a PR. The PR description should state that both gates are off and that `off` is byte-identical, citing the Task 2 and Task 7 pins.
