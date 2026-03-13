/* iMessage feed — read recent messages from chat.db. F95. macOS only. */
#ifdef HU_ENABLE_FEEDS

#include "human/feeds/imessage.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/feeds/ingest.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#if HU_IS_TEST

hu_error_t hu_imessage_feed_fetch(hu_allocator_t *alloc, int64_t since_epoch,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    (void)alloc;
    (void)since_epoch;
    if (!items || !out_count || items_cap < 2)
        return HU_ERR_INVALID_ARGUMENT;
    memset(items, 0, sizeof(hu_feed_ingest_item_t) * 2);
    (void)strncpy(items[0].source, "imessage", sizeof(items[0].source) - 1);
    (void)strncpy(items[0].content_type, "message", sizeof(items[0].content_type) - 1);
    (void)strncpy(items[0].content,
        "Check out this new AI coding tool, it supports autonomous agents",
        sizeof(items[0].content) - 1);
    items[0].content_len = strlen(items[0].content);
    items[0].ingested_at = (int64_t)time(NULL);
    (void)strncpy(items[0].contact_id, "+15555550100", sizeof(items[0].contact_id) - 1);
    (void)strncpy(items[1].source, "imessage", sizeof(items[1].source) - 1);
    (void)strncpy(items[1].content_type, "message", sizeof(items[1].content_type) - 1);
    (void)strncpy(items[1].content,
        "Have you tried the new local LLM? 70B runs great on M4",
        sizeof(items[1].content) - 1);
    items[1].content_len = strlen(items[1].content);
    items[1].ingested_at = (int64_t)time(NULL);
    (void)strncpy(items[1].contact_id, "+15555550101", sizeof(items[1].contact_id) - 1);
    *out_count = 2;
    return HU_OK;
}

#else

#if defined(__APPLE__) && defined(HU_ENABLE_SQLITE)
#include <sqlite3.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * iMessage stores dates as "Apple Cocoa Core Data" timestamps:
 * nanoseconds since 2001-01-01 00:00:00 UTC.
 * Offset from Unix epoch: 978307200 seconds.
 */
#define IMESSAGE_EPOCH_OFFSET 978307200LL
#define IMESSAGE_NS_PER_SEC   1000000000LL

hu_error_t hu_imessage_feed_fetch(hu_allocator_t *alloc, int64_t since_epoch,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    (void)alloc;
    if (!items || !out_count || items_cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_NOT_FOUND;

    char db_path[512];
    int n = snprintf(db_path, sizeof(db_path), "%s/Library/Messages/chat.db", home);
    if (n <= 0 || (size_t)n >= sizeof(db_path))
        return HU_ERR_INVALID_ARGUMENT;

    if (access(db_path, R_OK) != 0)
        return HU_ERR_NOT_FOUND;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return HU_ERR_IO;
    }

    int64_t since_apple = (since_epoch - IMESSAGE_EPOCH_OFFSET) * IMESSAGE_NS_PER_SEC;

    const char *sql =
        "SELECT m.text, m.date, m.is_from_me, h.id AS handle_id "
        "FROM message m "
        "LEFT JOIN handle h ON m.handle_id = h.ROWID "
        "WHERE m.text IS NOT NULL AND m.text != '' AND m.date > ? "
        "ORDER BY m.date DESC LIMIT ?";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }

    sqlite3_bind_int64(stmt, 1, since_apple);
    sqlite3_bind_int(stmt, 2, (int)items_cap);

    size_t cnt = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && cnt < items_cap) {
        const char *text = (const char *)sqlite3_column_text(stmt, 0);
        int64_t date_ns = sqlite3_column_int64(stmt, 1);
        const char *handle = (const char *)sqlite3_column_text(stmt, 3);

        if (!text || text[0] == '\0')
            continue;

        memset(&items[cnt], 0, sizeof(items[cnt]));
        (void)strncpy(items[cnt].source, "imessage", sizeof(items[cnt].source) - 1);
        (void)strncpy(items[cnt].content_type, "message", sizeof(items[cnt].content_type) - 1);
        snprintf(items[cnt].content, sizeof(items[cnt].content), "%s", text);
        items[cnt].content_len = strlen(items[cnt].content);
        items[cnt].ingested_at = (date_ns / IMESSAGE_NS_PER_SEC) + IMESSAGE_EPOCH_OFFSET;
        if (handle)
            snprintf(items[cnt].contact_id, sizeof(items[cnt].contact_id), "%s", handle);
        cnt++;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    *out_count = cnt;
    return HU_OK;
}

#else

hu_error_t hu_imessage_feed_fetch(hu_allocator_t *alloc, int64_t since_epoch,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    (void)alloc;
    (void)since_epoch;
    (void)items;
    (void)items_cap;
    if (out_count) *out_count = 0;
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* __APPLE__ && HU_ENABLE_SQLITE */

#endif /* HU_IS_TEST */

#else
typedef int hu_imessage_feed_stub_avoid_empty_tu;
#endif /* HU_ENABLE_FEEDS */
