/* src/channels/imessage_gaps.c
 *
 * Conversation-gap detection. See header for the contract.
 *
 * Pure classifier compiles unconditionally + is fully unit-testable.
 * SQL scanner is gated by !HU_IS_TEST so HU_IS_TEST builds get a stub
 * (consistent with hu_imessage_poll_reactions / observer pattern). */

#include "human/channels/imessage_gaps.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
#include <sqlite3.h>
#endif

#define HU_GAP_SECONDS_PER_DAY 86400

bool hu_imessage_gap_classify_stale(int64_t last_message_unix, int64_t now_unix,
                                    uint32_t historical_count, uint32_t min_history,
                                    int32_t min_gap_days, int32_t max_gap_days) {
    if (last_message_unix <= 0 || now_unix <= 0)
        return false;
    if (now_unix < last_message_unix)
        return false; /* clock skew / future timestamps — refuse to classify */
    if (historical_count < min_history)
        return false;
    int64_t gap_days = (now_unix - last_message_unix) / HU_GAP_SECONDS_PER_DAY;
    if (min_gap_days > 0 && gap_days < (int64_t)min_gap_days)
        return false;
    if (max_gap_days > 0 && gap_days > (int64_t)max_gap_days)
        return false;
    return true;
}

#if HU_IS_TEST || !defined(__APPLE__) || !defined(__MACH__) || !defined(HU_ENABLE_SQLITE)

hu_error_t hu_imessage_scan_stale_contacts(const char *db_path, int64_t now_unix,
                                           uint32_t min_history, int32_t min_gap_days,
                                           int32_t max_gap_days, hu_imessage_stale_contact_t *out,
                                           size_t cap, size_t *out_n) {
    (void)db_path;
    (void)now_unix;
    (void)min_history;
    (void)min_gap_days;
    (void)max_gap_days;
    (void)out;
    (void)cap;
    if (out_n)
        *out_n = 0;
    return HU_ERR_NOT_SUPPORTED;
}

#else

#define HU_MAC_EPOCH_OFFSET 978307200LL

hu_error_t hu_imessage_scan_stale_contacts(const char *db_path, int64_t now_unix,
                                           uint32_t min_history, int32_t min_gap_days,
                                           int32_t max_gap_days, hu_imessage_stale_contact_t *out,
                                           size_t cap, size_t *out_n) {
    if (!db_path || !out || !out_n)
        return HU_ERR_INVALID_ARGUMENT;
    *out_n = 0;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return HU_ERR_IO;
    }

    /* Aggregate by contact handle: most recent date + total inbound count.
     * We exclude is_from_me=1 from the count to focus on what THE CONTACT
     * sent — going silent matters more when it's the contact who stopped
     * initiating. Sort by gap-days desc; caller can re-sort if needed. */
    const char *sql = "SELECT h.id AS handle, "
                      "       MAX(m.date) AS latest_mac_ns, "
                      "       COUNT(*) AS msg_count "
                      "FROM message m "
                      "JOIN handle h ON h.ROWID = m.handle_id "
                      "WHERE m.is_from_me = 0 "
                      "  AND m.associated_message_type = 0 "
                      "GROUP BY h.id "
                      "HAVING COUNT(*) >= ? "
                      "ORDER BY MAX(m.date) ASC "
                      "LIMIT ?";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }
    sqlite3_bind_int(st, 1, (int)min_history);
    sqlite3_bind_int(st, 2, (int)cap * 4); /* over-fetch; we'll filter */

    while (sqlite3_step(st) == SQLITE_ROW && *out_n < cap) {
        const unsigned char *handle = sqlite3_column_text(st, 0);
        int64_t mac_ns = sqlite3_column_int64(st, 1);
        int msg_count = sqlite3_column_int(st, 2);
        if (!handle)
            continue;

        int64_t last_unix = (mac_ns / 1000000000) + HU_MAC_EPOCH_OFFSET;
        if (!hu_imessage_gap_classify_stale(last_unix, now_unix, (uint32_t)msg_count, min_history,
                                            min_gap_days, max_gap_days))
            continue;

        hu_imessage_stale_contact_t *row = &out[*out_n];
        memset(row, 0, sizeof(*row));
        strncpy(row->contact_handle, (const char *)handle, sizeof(row->contact_handle) - 1);
        row->contact_handle[sizeof(row->contact_handle) - 1] = '\0';
        row->last_message_unix = last_unix;
        row->days_since_last = (now_unix - last_unix) / HU_GAP_SECONDS_PER_DAY;
        row->historical_count = (uint32_t)msg_count;
        (*out_n)++;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return HU_OK;
}

#endif /* !HU_IS_TEST && Apple && SQLITE */
