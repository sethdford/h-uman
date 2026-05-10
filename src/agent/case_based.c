#include "human/agent/case_based.h"

#ifdef HU_ENABLE_SQLITE
#include "human/core/string.h"
#include <sqlite3.h>
struct sqlite3 *hu_graph__db_handle(hu_graph_t *g);
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

static int run_ddl(struct sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK)
        return rc;
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

static hu_error_t ensure_schema(struct sqlite3 *db) {
    static const char *const stmts[] = {
        "CREATE TABLE IF NOT EXISTS case_records ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "contact_id TEXT NOT NULL DEFAULT '',"
        "goal_verb TEXT NOT NULL,"
        "anchor_entity_ids TEXT NOT NULL DEFAULT '',"
        "plan_text TEXT,"
        "outcome TEXT,"
        "happened_at INTEGER NOT NULL)",
        "CREATE INDEX IF NOT EXISTS idx_cases_contact_verb "
        "ON case_records(contact_id, goal_verb)",
        "CREATE INDEX IF NOT EXISTS idx_cases_recent ON case_records(happened_at DESC)",
        NULL,
    };
    for (size_t i = 0; stmts[i]; i++) {
        if (run_ddl(db, stmts[i]) != SQLITE_OK)
            return HU_ERR_IO;
    }
    return HU_OK;
}

/* Anchor entity IDs are stored as a comma-joined string. Closed-form, no
 * separate table — keeps recall simple (LIKE / parse) and the DB compact. */
static int format_anchor_string(const int64_t *ids, size_t n, char *buf, size_t cap) {
    if (n == 0) {
        if (cap > 0)
            buf[0] = '\0';
        return 0;
    }
    size_t off = 0;
    for (size_t i = 0; i < n; i++) {
        int w = snprintf(buf + off, cap - off, "%s%lld", i == 0 ? "" : ",", (long long)ids[i]);
        if (w < 0 || (size_t)w >= cap - off)
            return -1;
        off += (size_t)w;
    }
    return (int)off;
}

/* Parse "12,34,567" into an array. Caller frees on success. */
static int parse_anchor_string(hu_allocator_t *alloc, const char *s, int64_t **out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    if (!s || !*s)
        return 0;
    /* Count commas + 1 for cap. */
    size_t cap = 1;
    for (const char *p = s; *p; p++)
        if (*p == ',')
            cap++;
    int64_t *arr = alloc->alloc(alloc->ctx, cap * sizeof(int64_t));
    if (!arr)
        return -1;
    size_t n = 0;
    const char *p = s;
    while (*p && n < cap) {
        char *end = NULL;
        long long v = strtoll(p, &end, 10);
        if (end == p)
            break;
        arr[n++] = (int64_t)v;
        p = end;
        while (*p == ',' || *p == ' ')
            p++;
    }
    *out = arr;
    *out_n = n;
    return 0;
}

hu_error_t hu_case_record(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                          const char *goal_verb, size_t goal_verb_len,
                          const int64_t *anchor_entity_ids, size_t anchor_count,
                          const char *plan_text, size_t plan_text_len, const char *outcome,
                          size_t outcome_len, int64_t happened_at, int64_t *out_id) {
    if (!g || !goal_verb || goal_verb_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = hu_graph__db_handle(g);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (ensure_schema(db) != HU_OK)
        return HU_ERR_IO;

    char anchors[1024];
    if (format_anchor_string(anchor_entity_ids, anchor_count, anchors, sizeof(anchors)) < 0)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO case_records"
                      " (contact_id, goal_verb, anchor_entity_ids, plan_text, outcome,"
                      "  happened_at) VALUES (?, ?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(st, 1, contact_id ? contact_id : "", (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, goal_verb, (int)goal_verb_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, anchors, -1, SQLITE_STATIC);
    if (plan_text && plan_text_len > 0)
        sqlite3_bind_text(st, 4, plan_text, (int)plan_text_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 4);
    if (outcome && outcome_len > 0)
        sqlite3_bind_text(st, 5, outcome, (int)outcome_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 5);
    sqlite3_bind_int64(st, 6, happened_at);

    int rc = sqlite3_step(st);
    if (rc == SQLITE_DONE && out_id)
        *out_id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_IO;
}

/* Compute case score: anchor-overlap (Jaccard-ish) + recency decay.
 *   overlap = |A ∩ B| / max(|A|, |B|)
 *   recency = exp(-age_days / 60)
 *   score   = 0.7 * overlap + 0.3 * recency
 */
static float score_case(const int64_t *q_anchors, size_t q_n, const int64_t *r_anchors, size_t r_n,
                        int64_t happened_at, int64_t now_ms) {
    size_t overlap = 0;
    size_t maxn = q_n > r_n ? q_n : r_n;
    if (maxn == 0)
        return 0.0f;
    for (size_t i = 0; i < q_n; i++)
        for (size_t j = 0; j < r_n; j++)
            if (q_anchors[i] == r_anchors[j]) {
                overlap++;
                break;
            }
    float ov = (float)overlap / (float)maxn;
    float rec = 1.0f;
    if (now_ms > 0 && happened_at > 0 && now_ms > happened_at) {
        double age_days = (double)(now_ms - happened_at) / (24.0 * 3600.0 * 1000.0);
        rec = (float)exp(-age_days / 60.0);
    }
    return 0.7f * ov + 0.3f * rec;
}

hu_error_t hu_case_recall(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                          size_t contact_id_len, const char *goal_verb, size_t goal_verb_len,
                          const int64_t *anchor_entity_ids, size_t anchor_count, int64_t now_ms,
                          size_t top_k, hu_case_record_t **out, size_t *out_count) {
    if (!g || !alloc || !goal_verb || !out || !out_count || top_k == 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    struct sqlite3 *db = hu_graph__db_handle(g);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    if (ensure_schema(db) != HU_OK)
        return HU_ERR_IO;

    /* Pull all records matching contact + goal_verb (cheap on a typical store).
     * Score in C, return top_k. For very large case stores we'd push scoring
     * into SQL; keep it simple at MVP. */
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT id, goal_verb, anchor_entity_ids, plan_text, outcome, happened_at "
                      "FROM case_records WHERE contact_id = ? AND goal_verb = ? "
                      "ORDER BY happened_at DESC LIMIT 1024";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(st, 1, contact_id ? contact_id : "", (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, goal_verb, (int)goal_verb_len, SQLITE_STATIC);

    /* Buffered scan. Keep at most 1024 candidates, score, sort, take top_k. */
    hu_case_record_t *cands = NULL;
    size_t cap = 0, n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n >= cap) {
            size_t new_cap = cap == 0 ? 16 : cap * 2;
            hu_case_record_t *t = alloc->alloc(alloc->ctx, new_cap * sizeof(hu_case_record_t));
            if (!t) {
                sqlite3_finalize(st);
                hu_case_records_free(alloc, cands, n);
                return HU_ERR_OUT_OF_MEMORY;
            }
            memset(t, 0, new_cap * sizeof(hu_case_record_t));
            if (cands) {
                memcpy(t, cands, n * sizeof(hu_case_record_t));
                alloc->free(alloc->ctx, cands, cap * sizeof(hu_case_record_t));
            }
            cands = t;
            cap = new_cap;
        }
        hu_case_record_t *r = &cands[n];
        memset(r, 0, sizeof(*r));
        r->id = sqlite3_column_int64(st, 0);
        const char *gv = (const char *)sqlite3_column_text(st, 1);
        size_t gv_len = gv ? (size_t)sqlite3_column_bytes(st, 1) : 0;
        const char *anchors = (const char *)sqlite3_column_text(st, 2);
        const char *pt = (const char *)sqlite3_column_text(st, 3);
        size_t pt_len = pt ? (size_t)sqlite3_column_bytes(st, 3) : 0;
        const char *oc = (const char *)sqlite3_column_text(st, 4);
        size_t oc_len = oc ? (size_t)sqlite3_column_bytes(st, 4) : 0;
        r->happened_at = sqlite3_column_int64(st, 5);

        if (gv && gv_len > 0) {
            r->goal_verb = hu_strndup(alloc, gv, gv_len);
            r->goal_verb_len = gv_len;
        }
        if (pt && pt_len > 0) {
            r->plan_text = hu_strndup(alloc, pt, pt_len);
            r->plan_text_len = pt_len;
        }
        if (oc && oc_len > 0) {
            r->outcome = hu_strndup(alloc, oc, oc_len);
            r->outcome_len = oc_len;
        }
        if (anchors)
            parse_anchor_string(alloc, anchors, &r->anchor_entity_ids, &r->anchor_count);

        r->score = score_case(anchor_entity_ids, anchor_count, r->anchor_entity_ids,
                              r->anchor_count, r->happened_at, now_ms);
        n++;
    }
    sqlite3_finalize(st);

    if (n == 0) {
        if (cands)
            alloc->free(alloc->ctx, cands, cap * sizeof(hu_case_record_t));
        return HU_OK;
    }

    /* Selection sort by score descending; cheap for n <= 1024. */
    for (size_t i = 0; i < n - 1 && i < top_k; i++) {
        size_t best = i;
        for (size_t j = i + 1; j < n; j++)
            if (cands[j].score > cands[best].score)
                best = j;
        if (best != i) {
            hu_case_record_t tmp = cands[i];
            cands[i] = cands[best];
            cands[best] = tmp;
        }
    }

    /* Truncate to top_k and free the discarded tail. */
    size_t keep = n < top_k ? n : top_k;
    if (keep < n) {
        for (size_t i = keep; i < n; i++) {
            if (cands[i].goal_verb)
                alloc->free(alloc->ctx, cands[i].goal_verb, cands[i].goal_verb_len + 1);
            if (cands[i].plan_text)
                alloc->free(alloc->ctx, cands[i].plan_text, cands[i].plan_text_len + 1);
            if (cands[i].outcome)
                alloc->free(alloc->ctx, cands[i].outcome, cands[i].outcome_len + 1);
            if (cands[i].anchor_entity_ids)
                alloc->free(alloc->ctx, cands[i].anchor_entity_ids,
                            cands[i].anchor_count * sizeof(int64_t));
        }
    }

    *out = cands;
    *out_count = keep;
    return HU_OK;
}

void hu_case_records_free(hu_allocator_t *alloc, hu_case_record_t *records, size_t count) {
    if (!alloc || !records)
        return;
    for (size_t i = 0; i < count; i++) {
        if (records[i].goal_verb)
            alloc->free(alloc->ctx, records[i].goal_verb, records[i].goal_verb_len + 1);
        if (records[i].plan_text)
            alloc->free(alloc->ctx, records[i].plan_text, records[i].plan_text_len + 1);
        if (records[i].outcome)
            alloc->free(alloc->ctx, records[i].outcome, records[i].outcome_len + 1);
        if (records[i].anchor_entity_ids)
            alloc->free(alloc->ctx, records[i].anchor_entity_ids,
                        records[i].anchor_count * sizeof(int64_t));
    }
    alloc->free(alloc->ctx, records, 0);
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t hu_case_record(hu_graph_t *g, const char *contact_id, size_t contact_id_len,
                          const char *goal_verb, size_t goal_verb_len,
                          const int64_t *anchor_entity_ids, size_t anchor_count,
                          const char *plan_text, size_t plan_text_len, const char *outcome,
                          size_t outcome_len, int64_t happened_at, int64_t *out_id) {
    (void)g;
    (void)contact_id;
    (void)contact_id_len;
    (void)goal_verb;
    (void)goal_verb_len;
    (void)anchor_entity_ids;
    (void)anchor_count;
    (void)plan_text;
    (void)plan_text_len;
    (void)outcome;
    (void)outcome_len;
    (void)happened_at;
    (void)out_id;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_case_recall(hu_graph_t *g, hu_allocator_t *alloc, const char *contact_id,
                          size_t contact_id_len, const char *goal_verb, size_t goal_verb_len,
                          const int64_t *anchor_entity_ids, size_t anchor_count, int64_t now_ms,
                          size_t top_k, hu_case_record_t **out, size_t *out_count) {
    (void)g;
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    (void)goal_verb;
    (void)goal_verb_len;
    (void)anchor_entity_ids;
    (void)anchor_count;
    (void)now_ms;
    (void)top_k;
    (void)out;
    (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
}

void hu_case_records_free(hu_allocator_t *alloc, hu_case_record_t *records, size_t count) {
    (void)alloc;
    (void)records;
    (void)count;
}

#endif
