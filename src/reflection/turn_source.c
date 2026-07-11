/* src/reflection/turn_source.c — Production SQLite turn source
 * (T9-followup).
 *
 * Spec: docs/plans/2026-05-26-reflection-loop/{design.md, tasks.md}.
 *
 * Replaces stub_turn_iter in src/daemon_reflection_tick.c with a real
 * cursor over the canonical `messages` conversation ledger that the
 * memory engine's save_message path writes (src/memory/engines/sqlite.c).
 * Each row is adapted to hu_reflection_turn_t so the reflection prompt
 * builder can derive patterns from actual conversations in production.
 *
 * This module reads only `struct sqlite3 *` — no daemon/agent coupling —
 * so it stays in the daemon-free reflection module and is unit-testable
 * against an in-memory db (tests/test_reflection_turn_source.c).
 *
 * Design notes:
 *   - STREAMING, not materialization. We hold a prepared sqlite3_stmt
 *     and step it lazily on each iter call. The iter contract only
 *     requires the char pointers to outlive the call's return (the
 *     builder copies before re-calling), so sqlite3_column_text's
 *     "valid until next step" guarantee is exactly enough. This keeps
 *     memory flat regardless of turn count.
 *   - The MOST-RECENT max_turns rows are selected (inner ORDER BY id
 *     DESC LIMIT), then re-ordered OLDEST-FIRST (outer ORDER BY id ASC)
 *     because hu_reflection_build_input drops the oldest on overflow —
 *     so the freshest, most signal-rich context survives truncation. */

#include "human/reflection.h"

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

struct hu_reflection_sqlite_turn_source {
    sqlite3 *db;          /* borrowed — not closed on dispose */
    sqlite3_stmt *stmt;   /* owned — finalized on dispose */
    hu_allocator_t alloc; /* copy, so dispose can free the struct */
    char turn_id_buf[32]; /* "msg-<id>" synthesized per row */
    bool done;            /* sticky end-of-stream / error flag */
    int turns_yielded;    /* count of turns returned via iter (for observability) */
};

hu_error_t hu_reflection_sqlite_turn_source_init(hu_reflection_sqlite_turn_source_t **out_src,
                                                 struct sqlite3 *db, struct hu_allocator *alloc,
                                                 int max_turns) {
    if (!out_src || !db || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    *out_src = NULL;

    if (max_turns <= 0)
        max_turns = HU_REFLECTION_TURN_SOURCE_DEFAULT_MAX;

    hu_reflection_sqlite_turn_source_t *src = alloc->alloc(alloc->ctx, sizeof(*src));
    if (!src)
        return HU_ERR_OUT_OF_MEMORY;
    memset(src, 0, sizeof(*src));
    src->db = db;
    src->alloc = *alloc;
    src->done = false;

    /* Inner subquery picks the most-recent N rows; outer re-sorts them
     * oldest-first. ts_ms is computed in SQL via strftime so the test's
     * expected_ts_ms probe and the implementation agree by construction.
     * created_at may be NULL/unparseable → strftime yields NULL →
     * column_int64 returns 0, matching the documented "0 when NULL". */
    static const char *sql = "SELECT id, session_id, role, content, "
                             "CAST(strftime('%s', created_at) AS INTEGER) * 1000 AS ts_ms "
                             "FROM (SELECT id, session_id, role, content, created_at "
                             "FROM messages ORDER BY id DESC LIMIT ?1) "
                             "ORDER BY id ASC";

    int rc = sqlite3_prepare_v2(src->db, sql, -1, &src->stmt, NULL);
    if (rc != SQLITE_OK || !src->stmt) {
        /* Most common cause: the `messages` table doesn't exist yet. */
        if (src->stmt) {
            sqlite3_finalize(src->stmt);
            src->stmt = NULL;
        }
        alloc->free(alloc->ctx, src, sizeof(*src));
        return HU_ERR_MEMORY_BACKEND;
    }

    sqlite3_bind_int(src->stmt, 1, max_turns);

    *out_src = src;
    return HU_OK;
}

bool hu_reflection_sqlite_turn_iter(void *ctx, hu_reflection_turn_t *out_turn) {
    hu_reflection_sqlite_turn_source_t *src = (hu_reflection_sqlite_turn_source_t *)ctx;
    if (!src || !out_turn || src->done || !src->stmt)
        return false;

    int rc = sqlite3_step(src->stmt);
    if (rc != SQLITE_ROW) {
        /* SQLITE_DONE or any error: end of stream, sticky from now on. */
        src->done = true;
        return false;
    }

    sqlite3_int64 id = sqlite3_column_int64(src->stmt, 0);
    const unsigned char *session_id = sqlite3_column_text(src->stmt, 1);
    const unsigned char *role = sqlite3_column_text(src->stmt, 2);
    const unsigned char *content = sqlite3_column_text(src->stmt, 3);
    sqlite3_int64 ts_ms = sqlite3_column_int64(src->stmt, 4);

    snprintf(src->turn_id_buf, sizeof(src->turn_id_buf), "msg-%lld", (long long)id);

    out_turn->turn_id = src->turn_id_buf;
    out_turn->channel = session_id ? (const char *)session_id : "";
    out_turn->sender = role ? (const char *)role : "";
    out_turn->content = content ? (const char *)content : "";
    out_turn->ts_ms = ts_ms < 0 ? 0u : (uint64_t)ts_ms;

    src->turns_yielded++;
    return true;
}

int hu_reflection_sqlite_turn_source_turns_yielded(const hu_reflection_sqlite_turn_source_t *src) {
    if (!src)
        return 0;
    return src->turns_yielded;
}

void hu_reflection_sqlite_turn_source_dispose(hu_reflection_sqlite_turn_source_t *src) {
    if (!src)
        return;
    if (src->stmt)
        sqlite3_finalize(src->stmt);
    hu_allocator_t alloc = src->alloc;
    alloc.free(alloc.ctx, src, sizeof(*src));
}
