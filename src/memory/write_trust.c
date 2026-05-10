#include "human/memory/write_trust.h"

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
struct sqlite3 *hu_graph__db_handle(hu_graph_t *g);
#endif

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Weights (sum to 1.0). Tunable via hu_write_trust_set_weights for tests. */
#define W_SOURCE   0.40f
#define W_RECENCY  0.10f
#define W_CONSIST  0.30f
#define W_ANOMALY  0.20f

#define LIVE_THRESHOLD 0.60f
#define DROP_THRESHOLD 0.30f

static float source_score(hu_write_source_t s) {
    switch (s) {
        case HU_WRITE_SOURCE_USER:             return 1.00f;
        case HU_WRITE_SOURCE_CHANNEL_TRUSTED:  return 0.85f;
        case HU_WRITE_SOURCE_CHANNEL_OPEN:     return 0.55f;
        case HU_WRITE_SOURCE_FEED_FILE:        return 0.70f;
        case HU_WRITE_SOURCE_FEED_WEB:         return 0.50f;
        case HU_WRITE_SOURCE_AGENT:            return 0.45f;
        case HU_WRITE_SOURCE_UNKNOWN:          return 0.30f;
    }
    return 0.30f;
}

const char *hu_write_source_str(hu_write_source_t s) {
    switch (s) {
        case HU_WRITE_SOURCE_USER:             return "user";
        case HU_WRITE_SOURCE_CHANNEL_TRUSTED:  return "channel-trusted";
        case HU_WRITE_SOURCE_CHANNEL_OPEN:     return "channel-open";
        case HU_WRITE_SOURCE_FEED_FILE:        return "feed-file";
        case HU_WRITE_SOURCE_FEED_WEB:         return "feed-web";
        case HU_WRITE_SOURCE_AGENT:            return "agent";
        case HU_WRITE_SOURCE_UNKNOWN:          return "unknown";
    }
    return "unknown";
}

const char *hu_write_outcome_str(hu_write_outcome_t o) {
    switch (o) {
        case HU_WRITE_OUTCOME_LIVE:       return "live";
        case HU_WRITE_OUTCOME_QUARANTINE: return "quarantine";
        case HU_WRITE_OUTCOME_DROP:       return "drop";
    }
    return "unknown";
}

/* Recency: 1.0 at t=now, ~0.6 at 24h, ~0.13 at 7d. Half-life ~36h. */
static float recency_score(int64_t observed_at, int64_t now) {
    if (observed_at <= 0 || now <= 0)
        return 0.5f;
    int64_t age_ms = now - observed_at;
    if (age_ms < 0)
        age_ms = 0;
    double age_h = (double)age_ms / 3600000.0;
    /* exp(-age_h / 36): 1.0 at 0h, 0.51 at 24h, 0.14 at 72h. */
    double s = exp(-age_h / 36.0);
    if (s > 1.0)
        s = 1.0;
    if (s < 0.0)
        s = 0.0;
    return (float)s;
}

static float consistency_score(bool flag, bool supersession) {
    /* Active contradiction with a high-confidence prior is a strong negative
     * signal. AutoDream (W2) re-evaluates with an LLM and may promote later. */
    if (flag)
        return 0.30f;
    if (supersession)
        return 0.70f;
    return 1.00f;
}

static float anomaly_score(uint32_t recent_writes, uint32_t rate_limit) {
    if (rate_limit == 0)
        return 1.0f;
    if (recent_writes <= rate_limit)
        return 1.0f;
    if (recent_writes > rate_limit * 5)
        return 0.0f;
    if (recent_writes > rate_limit * 2)
        return 0.5f;
    return 0.7f;
}

hu_write_trust_decision_t hu_write_trust_score(const hu_write_trust_input_t *in) {
    hu_write_trust_decision_t d = {0};
    if (!in) {
        d.score = 0.0f;
        d.outcome = HU_WRITE_OUTCOME_DROP;
        snprintf(d.reason, sizeof(d.reason), "null-input");
        return d;
    }

    float ss = source_score(in->source);
    float rs = recency_score(in->observed_at, in->now);
    float cs = consistency_score(in->contradiction_flag, in->supersession);
    float as = anomaly_score(in->recent_writes, in->rate_limit);

    d.score = W_SOURCE * ss + W_RECENCY * rs + W_CONSIST * cs + W_ANOMALY * as;
    if (d.score > 1.0f)
        d.score = 1.0f;
    if (d.score < 0.0f)
        d.score = 0.0f;

    if (d.score >= LIVE_THRESHOLD)
        d.outcome = HU_WRITE_OUTCOME_LIVE;
    else if (d.score >= DROP_THRESHOLD)
        d.outcome = HU_WRITE_OUTCOME_QUARANTINE;
    else
        d.outcome = HU_WRITE_OUTCOME_DROP;

    /* Flooding floor: a source exceeding 10x its rate limit is almost certainly
     * an automated scraper or replay attempt. Force DROP regardless of the
     * weighted score so this can never quarantine-bomb the review queue. */
    if (in->rate_limit > 0 && in->recent_writes > in->rate_limit * 10)
        d.outcome = HU_WRITE_OUTCOME_DROP;

    /* One short reason for the dominant negative factor. Useful for /memory
     * dashboards and adversarial test assertions. */
    if (in->recent_writes > in->rate_limit && in->rate_limit > 0)
        snprintf(d.reason, sizeof(d.reason), "rate-limit:%s", hu_write_source_str(in->source));
    else if (in->contradiction_flag)
        snprintf(d.reason, sizeof(d.reason), "contradiction:%s", hu_write_source_str(in->source));
    else if (ss < 0.6f)
        snprintf(d.reason, sizeof(d.reason), "low-trust-source:%s",
                 hu_write_source_str(in->source));
    else
        snprintf(d.reason, sizeof(d.reason), "ok:%s", hu_write_source_str(in->source));

    return d;
}

#ifdef HU_ENABLE_SQLITE

hu_error_t hu_write_trust_quarantine_relation(hu_graph_t *g, const char *contact_id,
                                              size_t contact_id_len, int64_t source_id,
                                              int64_t target_id, hu_relation_type_t type,
                                              float weight, int64_t event_start, int64_t event_end,
                                              float confidence, const char *context,
                                              size_t context_len, const char *provenance,
                                              size_t provenance_len,
                                              const hu_write_trust_decision_t *decision) {
    if (!g || !decision)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = hu_graph__db_handle(g);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;

    const char *cid = contact_id ? contact_id : "";
    int cid_len = contact_id ? (int)contact_id_len : 0;

    const char *sql = "INSERT INTO quarantine_relations ("
                      "contact_id, source_id, target_id, relation_type, weight,"
                      " first_seen, last_seen, event_start, event_end, confidence,"
                      " context, provenance, trust_score, trust_reason, quarantined_at) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    int64_t now_ms_v = event_start > 0 ? event_start : 0;
    sqlite3_bind_text(stmt, 1, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, source_id);
    sqlite3_bind_int64(stmt, 3, target_id);
    sqlite3_bind_int(stmt, 4, (int)type);
    sqlite3_bind_double(stmt, 5, (double)weight);
    sqlite3_bind_int64(stmt, 6, now_ms_v);
    sqlite3_bind_int64(stmt, 7, now_ms_v);
    sqlite3_bind_int64(stmt, 8, event_start);
    sqlite3_bind_int64(stmt, 9, event_end);
    sqlite3_bind_double(stmt, 10, (double)confidence);
    if (context && context_len > 0)
        sqlite3_bind_text(stmt, 11, context, (int)context_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 11);
    if (provenance && provenance_len > 0)
        sqlite3_bind_text(stmt, 12, provenance, (int)provenance_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 12);
    sqlite3_bind_double(stmt, 13, (double)decision->score);
    sqlite3_bind_text(stmt, 14, decision->reason, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 15, now_ms_v);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_IO;
}

hu_error_t hu_write_trust_quarantine_count(hu_graph_t *g, const char *contact_id,
                                           size_t contact_id_len, size_t *out_count) {
    if (!g || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = hu_graph__db_handle(g);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;

    const char *cid = contact_id ? contact_id : "";
    int cid_len = contact_id ? (int)contact_id_len : 0;

    const char *sql = "SELECT COUNT(*) FROM quarantine_relations WHERE contact_id = ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(stmt, 1, cid, cid_len, SQLITE_STATIC);

    *out_count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        *out_count = (size_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return HU_OK;
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t hu_write_trust_quarantine_relation(hu_graph_t *g, const char *contact_id,
                                              size_t contact_id_len, int64_t source_id,
                                              int64_t target_id, hu_relation_type_t type,
                                              float weight, int64_t event_start, int64_t event_end,
                                              float confidence, const char *context,
                                              size_t context_len, const char *provenance,
                                              size_t provenance_len,
                                              const hu_write_trust_decision_t *decision) {
    (void)g;
    (void)contact_id;
    (void)contact_id_len;
    (void)source_id;
    (void)target_id;
    (void)type;
    (void)weight;
    (void)event_start;
    (void)event_end;
    (void)confidence;
    (void)context;
    (void)context_len;
    (void)provenance;
    (void)provenance_len;
    (void)decision;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_write_trust_quarantine_count(hu_graph_t *g, const char *contact_id,
                                           size_t contact_id_len, size_t *out_count) {
    (void)g;
    (void)contact_id;
    (void)contact_id_len;
    (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
}

#endif
