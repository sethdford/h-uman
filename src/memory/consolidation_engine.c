typedef int hu_consolidation_engine_unused_;

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/consolidation_engine.h"
#include "human/memory/episodic.h"
#include "human/memory/forgetting_curve.h"
#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEC_PER_HOUR  3600
#define SEC_PER_DAY   86400
#define NIGHTLY_GAP  (20 * SEC_PER_HOUR)
#define WEEKLY_GAP   (6 * SEC_PER_DAY)
#define MONTHLY_GAP  (25 * SEC_PER_DAY)
#define WEEK_SEC     (7 * SEC_PER_DAY)
#define MONTH_90_SEC (90 * SEC_PER_DAY)
#define FORGETTING_RATE 0.1

/* Substring match: a and b are similar if one contains the other. */
static bool summaries_similar(const char *a, size_t a_len, const char *b, size_t b_len) {
    if (!a || !b)
        return false;
    if (a_len == 0 || b_len == 0)
        return false;
    /* Ensure null-terminated for strstr. Episodes have fixed buffers, so we can use
     * the lengths. summary/len may not be null-terminated in theory; episodic uses
     * copy with null. We'll assume they're null-terminated. */
    if (a_len >= b_len && strstr(a, b) != NULL)
        return true;
    if (b_len >= a_len && strstr(b, a) != NULL)
        return true;
    return false;
}

hu_error_t hu_consolidation_engine_nightly(hu_consolidation_engine_t *engine, int64_t now_ts) {
    if (!engine || !engine->alloc || !engine->db)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = hu_forgetting_apply_batch_decay(engine->db, now_ts, FORGETTING_RATE);
    if (err != HU_OK)
        return err;

    /* Deduplicate: per contact, find episodes with similar summaries (substring match),
     * keep higher impact, delete the rest. */
    sqlite3_stmt *sel = NULL;
    int rc = sqlite3_prepare_v2(engine->db,
                                "SELECT id, contact_id, summary, impact_score FROM episodes "
                                "ORDER BY contact_id, impact_score DESC, created_at DESC",
                                -1, &sel, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    /* Collect ids to delete. For each contact, walk episodes by impact desc; if current
     * summary is substring-similar to any we've kept, mark for deletion. */
    size_t del_cap = 64;
    int64_t *to_delete = (int64_t *)engine->alloc->alloc(engine->alloc->ctx,
                                                        del_cap * sizeof(int64_t));
    if (!to_delete) {
        sqlite3_finalize(sel);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t del_count = 0;

    char prev_contact[128] = {0};
    size_t prev_contact_len = 0;
    struct {
        char summary[2048];
        size_t summary_len;
    } kept[32];
    size_t kept_count = 0;

    while (sqlite3_step(sel) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(sel, 0);
        const char *cid = (const char *)sqlite3_column_text(sel, 1);
        size_t cid_len = cid ? (size_t)sqlite3_column_bytes(sel, 1) : 0;
        const char *sum = (const char *)sqlite3_column_text(sel, 2);
        size_t sum_len = sum ? (size_t)sqlite3_column_bytes(sel, 2) : 0;

        if (!cid || cid_len == 0)
            continue;

        /* New contact: reset kept list. Cap comparison to prev_contact size to avoid overread.
         * If either contact_id exceeds buffer, treat as different (conservative for privacy). */
        bool is_new_contact = (prev_contact_len != cid_len);
        if (!is_new_contact && cid_len <= sizeof(prev_contact) - 1) {
            is_new_contact = (memcmp(prev_contact, cid, cid_len) != 0);
        } else if (cid_len > sizeof(prev_contact) - 1) {
            is_new_contact = true;
        }
        if (is_new_contact) {
            memcpy(prev_contact, cid, cid_len < sizeof(prev_contact) - 1 ? cid_len
                                                                          : sizeof(prev_contact) - 1);
            prev_contact[cid_len < sizeof(prev_contact) ? cid_len : sizeof(prev_contact) - 1] = '\0';
            prev_contact_len = cid_len;
            kept_count = 0;
        }

        bool is_dup = false;
        for (size_t k = 0; k < kept_count; k++) {
            if (summaries_similar(sum, sum_len, kept[k].summary, kept[k].summary_len)) {
                is_dup = true;
                break;
            }
        }
        if (is_dup) {
            if (del_count >= del_cap) {
                del_cap *= 2;
                int64_t *n = (int64_t *)engine->alloc->alloc(engine->alloc->ctx,
                                                             del_cap * sizeof(int64_t));
                if (!n) {
                    engine->alloc->free(engine->alloc->ctx, to_delete,
                                        (del_cap / 2) * sizeof(int64_t));
                    sqlite3_finalize(sel);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                memcpy(n, to_delete, del_count * sizeof(int64_t));
                engine->alloc->free(engine->alloc->ctx, to_delete,
                                   (del_cap / 2) * sizeof(int64_t));
                to_delete = n;
            }
            to_delete[del_count++] = id;
        } else {
            if (kept_count < sizeof(kept) / sizeof(kept[0])) {
                size_t copy = sum_len < sizeof(kept[0].summary) - 1 ? sum_len
                                                                    : sizeof(kept[0].summary) - 1;
                memcpy(kept[kept_count].summary, sum, copy);
                kept[kept_count].summary[copy] = '\0';
                kept[kept_count].summary_len = copy;
                kept_count++;
            }
        }
    }
    sqlite3_finalize(sel);

    for (size_t i = 0; i < del_count; i++) {
        sqlite3_stmt *del = NULL;
        rc = sqlite3_prepare_v2(engine->db, "DELETE FROM episodes WHERE id=?", -1, &del, NULL);
        if (rc != SQLITE_OK) {
            engine->alloc->free(engine->alloc->ctx, to_delete, del_cap * sizeof(int64_t));
            return HU_ERR_MEMORY_BACKEND;
        }
        sqlite3_bind_int64(del, 1, to_delete[i]);
        sqlite3_step(del);
        sqlite3_finalize(del);
    }
    engine->alloc->free(engine->alloc->ctx, to_delete, del_cap * sizeof(int64_t));
    return HU_OK;
}

hu_error_t hu_consolidation_engine_weekly(hu_consolidation_engine_t *engine, int64_t now_ts) {
    if (!engine || !engine->alloc || !engine->db)
        return HU_ERR_INVALID_ARGUMENT;

    int64_t since = now_ts - (int64_t)WEEK_SEC;

    sqlite3_stmt *count_stmt = NULL;
    int rc = sqlite3_prepare_v2(engine->db,
                               "SELECT contact_id, COUNT(*) FROM episodes "
                               "WHERE created_at >= ? GROUP BY contact_id HAVING COUNT(*) > 5",
                               -1, &count_stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_int64(count_stmt, 1, since);

    while (sqlite3_step(count_stmt) == SQLITE_ROW) {
        const char *contact_id = (const char *)sqlite3_column_text(count_stmt, 0);
        size_t cid_len = contact_id ? (size_t)sqlite3_column_bytes(count_stmt, 0) : 0;
        if (!contact_id || cid_len == 0)
            continue;

        hu_episode_sqlite_t *episodes = NULL;
        size_t ep_count = 0;
        hu_error_t err = hu_episode_get_by_contact(engine->alloc, engine->db, contact_id,
                                                   cid_len, 64, since, &episodes, &ep_count);
        if (err != HU_OK || !episodes || ep_count == 0)
            continue;

        /* Aggregate key_moments into summary. */
        char key_agg[2048];
        size_t agg_len = 0;
        key_agg[0] = '\0';
        for (size_t i = 0; i < ep_count && agg_len < sizeof(key_agg) - 2; i++) {
            if (episodes[i].key_moments_len > 0) {
                if (agg_len > 0) {
                    key_agg[agg_len++] = ' ';
                    key_agg[agg_len] = '\0';
                }
                size_t add = episodes[i].key_moments_len;
                if (agg_len + add >= sizeof(key_agg) - 1)
                    add = sizeof(key_agg) - 1 - agg_len;
                memcpy(key_agg + agg_len, episodes[i].key_moments, add);
                agg_len += add;
                key_agg[agg_len] = '\0';
            }
        }
        hu_episode_free(engine->alloc, episodes, ep_count);

        char summary[512];
        int n = snprintf(summary, sizeof(summary), "This week with %.*s: %.*s",
                        (int)cid_len, contact_id,
                        (int)(agg_len < 200 ? agg_len : 200), key_agg);
        if (n < 0 || (size_t)n >= sizeof(summary))
            continue;

        int64_t out_id = 0;
        err = hu_episode_store_insert(engine->alloc, engine->db, contact_id, cid_len,
                                      summary, (size_t)n, NULL, 0, key_agg, agg_len,
                                      0.5, "weekly_summary", 14, &out_id);
        if (err != HU_OK)
            continue;
    }
    sqlite3_finalize(count_stmt);
    return HU_OK;
}

hu_error_t hu_consolidation_engine_monthly(hu_consolidation_engine_t *engine, int64_t now_ts) {
    if (!engine || !engine->db)
        return HU_ERR_INVALID_ARGUMENT;

    int64_t cutoff = now_ts - (int64_t)MONTH_90_SEC;

    sqlite3_stmt *del = NULL;
    int rc = sqlite3_prepare_v2(engine->db,
                               "DELETE FROM episodes WHERE salience_score < 0.05 AND created_at < ?",
                               -1, &del, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_int64(del, 1, cutoff);
    rc = sqlite3_step(del);
    sqlite3_finalize(del);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_consolidation_engine_run_scheduled(hu_consolidation_engine_t *engine,
                                                int64_t now_ts, int64_t last_nightly,
                                                int64_t last_weekly, int64_t last_monthly) {
    if (!engine)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = HU_OK;
    if (last_nightly == 0 || (now_ts - last_nightly) >= (int64_t)NIGHTLY_GAP) {
        err = hu_consolidation_engine_nightly(engine, now_ts);
        if (err != HU_OK)
            return err;
    }
    if (last_weekly == 0 || (now_ts - last_weekly) >= (int64_t)WEEKLY_GAP) {
        err = hu_consolidation_engine_weekly(engine, now_ts);
        if (err != HU_OK)
            return err;
    }
    if (last_monthly == 0 || (now_ts - last_monthly) >= (int64_t)MONTHLY_GAP) {
        err = hu_consolidation_engine_monthly(engine, now_ts);
        if (err != HU_OK)
            return err;
    }
    return err;
}

#endif /* HU_ENABLE_SQLITE */
