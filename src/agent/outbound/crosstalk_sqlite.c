/* src/agent/outbound/crosstalk_sqlite.c
 *
 * SQLite-backed lookup for the outbound crosstalk stage's cross-
 * contact bleed check. Implements the contract declared in
 * include/human/agent/outbound_crosstalk_sqlite.h, which closes the
 * "degraded mode" item #3 from
 * docs/plans/2026-05-26-sprint-59-outbound-safety/STATUS.md.
 *
 * Stage hook contract: see include/human/agent/outbound_pipeline.h:
 *   - We MUST allocate out_texts and each string via the provided
 *     allocator (the stage frees both via the same allocator).
 *   - Empty case returns 0 with *out_texts == NULL.
 *   - Errors return -1 with cleared out parameters.
 */

#ifdef HU_ENABLE_SQLITE

#include "human/agent/outbound_crosstalk_sqlite.h"
#include "human/agent/outbound_pipeline.h"

#include <stdlib.h>
#include <string.h>

/* Bounded result size — the crosstalk stage Jaccards against each
 * returned row, so the total cost is O(LIMIT * |msg.content|). At 64
 * rows and typical message lengths, this is sub-millisecond. The 7-day
 * window plus this cap keeps the worst case predictable even when a
 * contact-storm fills the messages table. */
#define HU_OUTBOUND_CROSSTALK_SQLITE_LIMIT 64

int hu_outbound_crosstalk_sqlite_lookup(void *userdata, hu_allocator_t *alloc,
                                        const char *exclude_id, size_t exclude_id_len,
                                        char ***out_texts, size_t *out_count) {
    if (!userdata || !alloc || !exclude_id || !out_texts || !out_count)
        return -1;
    (void)exclude_id_len; /* SQLite bind uses NUL-terminated string */
    *out_texts = NULL;
    *out_count = 0;

    sqlite3 *db = (sqlite3 *)userdata;
    /* Filter: rows from any contact OTHER than the recipient in the
     * last 7 days. ORDER BY DESC + LIMIT bounds the cost and prefers
     * recent corpus — the closer in time, the higher the risk of a
     * verbatim phrasing carry-over (the Annie/Mindy/Betty pattern). */
    const char *sql = "SELECT content FROM messages "
                      "WHERE session_id != ? AND "
                      "created_at > datetime('now', '-7 days') "
                      "ORDER BY created_at DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_bind_text(stmt, 1, exclude_id, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 2, HU_OUTBOUND_CROSSTALK_SQLITE_LIMIT) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }

    /* Two-phase allocation: collect into a stack scratch buffer first
     * so we can hand back an array sized exactly to row-count. The
     * stage's free path uses other_count * sizeof(char *); a
     * pre-sized array would leak the slack on partial fills. */
    char *scratch[HU_OUTBOUND_CROSSTALK_SQLITE_LIMIT];
    size_t count = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && count < HU_OUTBOUND_CROSSTALK_SQLITE_LIMIT) {
        const char *content = (const char *)sqlite3_column_text(stmt, 0);
        if (!content)
            continue;
        size_t len = strlen(content);
        char *buf = (char *)alloc->alloc(alloc->ctx, len + 1);
        if (!buf) {
            for (size_t i = 0; i < count; i++)
                alloc->free(alloc->ctx, scratch[i], strlen(scratch[i]) + 1);
            sqlite3_finalize(stmt);
            return -1;
        }
        memcpy(buf, content, len + 1);
        scratch[count++] = buf;
    }
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        for (size_t i = 0; i < count; i++)
            alloc->free(alloc->ctx, scratch[i], strlen(scratch[i]) + 1);
        return -1;
    }

    if (count == 0) {
        /* Empty result — no allocation, NULL out_texts. Matches the
         * fake_lookup convention in tests/test_outbound_crosstalk.c. */
        return 0;
    }

    char **arr = (char **)alloc->alloc(alloc->ctx, count * sizeof(char *));
    if (!arr) {
        for (size_t i = 0; i < count; i++)
            alloc->free(alloc->ctx, scratch[i], strlen(scratch[i]) + 1);
        return -1;
    }
    memcpy(arr, scratch, count * sizeof(char *));
    *out_texts = arr;
    *out_count = count;
    return 0;
}

void hu_outbound_crosstalk_register_sqlite(sqlite3 *db) {
    if (!db) {
        hu_outbound_crosstalk_set_lookup(NULL, NULL);
        return;
    }
    hu_outbound_crosstalk_set_lookup(hu_outbound_crosstalk_sqlite_lookup, db);
}

void hu_outbound_crosstalk_unregister_sqlite(void) {
    hu_outbound_crosstalk_set_lookup(NULL, NULL);
}

#endif /* HU_ENABLE_SQLITE */

/* ISO C forbids empty translation units (-Werror=pedantic). When
 * HU_ENABLE_SQLITE is OFF, the entire body above compiles away. A
 * single typedef keeps the TU non-empty under every CMake variant
 * without introducing any linkage symbols. */
typedef int hu_crosstalk_sqlite_tu_marker_t;
