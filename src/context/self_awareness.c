#include "human/context/self_awareness.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/self_awareness_repo.h"
/* DDD Phase 3: executor SQL moved behind hu_self_awareness_repo_t
 * (src/memory/repos/self_awareness_repo_sqlite.c). Ratio math, directive text,
 * and reciprocity-compute stay here; no <sqlite3.h>. */
#endif

#define HU_SELF_AWARENESS_ESCAPE_BUF 512

static void escape_sql_string(const char *s, size_t len, char *buf, size_t cap, size_t *out_len) {
    (void)hu_sql_quote_escape_into(s, len, buf, cap, out_len);
}

hu_error_t hu_self_awareness_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;

    static const char sql[] = "CREATE TABLE IF NOT EXISTS self_awareness_stats (\n"
                              "    contact_id TEXT PRIMARY KEY,\n"
                              "    messages_sent_week INTEGER DEFAULT 0,\n"
                              "    initiations_week INTEGER DEFAULT 0,\n"
                              "    last_topic TEXT,\n"
                              "    topic_repeat_count INTEGER DEFAULT 0,\n"
                              "    updated_at INTEGER\n"
                              ");\n"
                              "CREATE TABLE IF NOT EXISTS reciprocity_scores (\n"
                              "    contact_id TEXT NOT NULL,\n"
                              "    metric TEXT NOT NULL,\n"
                              "    value REAL,\n"
                              "    updated_at INTEGER,\n"
                              "    PRIMARY KEY (contact_id, metric)\n"
                              ");";
    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_self_awareness_record_send_sql(const char *contact_id, size_t contact_id_len,
                                             bool we_initiated, const char *topic, size_t topic_len,
                                             char *buf, size_t cap, size_t *out_len) {
    if (!contact_id || contact_id_len == 0 || !buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;

    char contact_esc[HU_SELF_AWARENESS_ESCAPE_BUF];
    char topic_esc[HU_SELF_AWARENESS_ESCAPE_BUF];
    size_t ce_len = 0;
    size_t te_len = 0;
    escape_sql_string(contact_id, contact_id_len, contact_esc, sizeof(contact_esc), &ce_len);
    if (topic && topic_len > 0) {
        escape_sql_string(topic, topic_len, topic_esc, sizeof(topic_esc), &te_len);
    } else {
        topic_esc[0] = '\0';
    }
    (void)ce_len;
    (void)te_len;

    int64_t now_ts = (int64_t)time(NULL);
    uint32_t init_inc = we_initiated ? 1u : 0u;

    const char *topic_sql = (topic && topic_len > 0) ? topic_esc : "";
    char topic_quoted[1024];
    if (topic && topic_len > 0) {
        int nq = snprintf(topic_quoted, sizeof(topic_quoted), "'%s'", topic_esc);
        if (nq < 0 || (size_t)nq >= sizeof(topic_quoted))
            return HU_ERR_INVALID_ARGUMENT;
        topic_sql = topic_quoted;
    }

    int n;
    if (topic && topic_len > 0) {
        n = snprintf(buf, cap,
                     "INSERT INTO self_awareness_stats (contact_id, messages_sent_week, "
                     "initiations_week, last_topic, topic_repeat_count, updated_at) VALUES "
                     "('%s', 1, %u, %s, 1, %lld) "
                     "ON CONFLICT(contact_id) DO UPDATE SET "
                     "messages_sent_week = messages_sent_week + excluded.messages_sent_week, "
                     "initiations_week = initiations_week + excluded.initiations_week, "
                     "last_topic = excluded.last_topic, "
                     "topic_repeat_count = CASE WHEN COALESCE(last_topic,'') = "
                     "COALESCE(excluded.last_topic,'') "
                     "THEN topic_repeat_count + 1 ELSE 1 END, "
                     "updated_at = excluded.updated_at",
                     contact_esc, init_inc, topic_sql, (long long)now_ts);
    } else {
        n = snprintf(buf, cap,
                     "INSERT INTO self_awareness_stats (contact_id, messages_sent_week, "
                     "initiations_week, last_topic, topic_repeat_count, updated_at) VALUES "
                     "('%s', 1, %u, NULL, 1, %lld) "
                     "ON CONFLICT(contact_id) DO UPDATE SET "
                     "messages_sent_week = messages_sent_week + excluded.messages_sent_week, "
                     "initiations_week = initiations_week + excluded.initiations_week, "
                     "last_topic = excluded.last_topic, "
                     "topic_repeat_count = CASE WHEN COALESCE(last_topic,'') = "
                     "COALESCE(excluded.last_topic,'') "
                     "THEN topic_repeat_count + 1 ELSE 1 END, "
                     "updated_at = excluded.updated_at",
                     contact_esc, init_inc, (long long)now_ts);
    }
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_self_awareness_query_sql(const char *contact_id, size_t contact_id_len, char *buf,
                                       size_t cap, size_t *out_len) {
    if (!contact_id || contact_id_len == 0 || !buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;

    char contact_esc[HU_SELF_AWARENESS_ESCAPE_BUF];
    size_t ce_len;
    escape_sql_string(contact_id, contact_id_len, contact_esc, sizeof(contact_esc), &ce_len);

    int n =
        snprintf(buf, cap,
                 "SELECT contact_id, messages_sent_week, initiations_week, last_topic, "
                 "topic_repeat_count, updated_at FROM self_awareness_stats WHERE contact_id = '%s'",
                 contact_esc);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

double hu_self_awareness_initiation_ratio(const hu_self_stats_t *stats) {
    if (!stats)
        return 0.5;
    uint32_t total = stats->initiations_week + stats->their_initiations_week;
    if (total == 0)
        return 0.5;
    return (double)stats->initiations_week / (double)total;
}

bool hu_self_awareness_topic_repeating(const hu_self_stats_t *stats, uint32_t threshold) {
    if (!stats)
        return false;
    return stats->topic_repeat_count >= threshold;
}

hu_error_t hu_self_awareness_build_directive(hu_allocator_t *alloc, const hu_self_stats_t *stats,
                                             char **out, size_t *out_len) {
    if (!alloc || !stats || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    double ratio = hu_self_awareness_initiation_ratio(stats);
    if (ratio < 0.3) {
        char *s = hu_strndup(
            alloc, "I've been kind of quiet with them — should probably reach out more", 56);
        if (!s)
            return HU_ERR_OUT_OF_MEMORY;
        *out = s;
        *out_len = 56;
        return HU_OK;
    }
    if (stats->topic_repeat_count >= 3 && stats->last_topic && stats->last_topic_len > 0) {
        char buf[256];
        int n =
            snprintf(buf, sizeof(buf), "I keep bringing up %.*s — should talk about something else",
                     (int)stats->last_topic_len, stats->last_topic);
        if (n < 0 || (size_t)n >= sizeof(buf))
            return HU_ERR_INVALID_ARGUMENT;
        char *s = hu_strndup(alloc, buf, (size_t)n);
        if (!s)
            return HU_ERR_OUT_OF_MEMORY;
        *out = s;
        *out_len = (size_t)n;
        return HU_OK;
    }
    if (stats->days_since_contact > 7) {
        char buf[128];
        int n = snprintf(buf, sizeof(buf), "Haven't talked to them in a while — it's been %u days",
                         stats->days_since_contact);
        if (n < 0 || (size_t)n >= sizeof(buf))
            return HU_ERR_INVALID_ARGUMENT;
        char *s = hu_strndup(alloc, buf, (size_t)n);
        if (!s)
            return HU_ERR_OUT_OF_MEMORY;
        *out = s;
        *out_len = (size_t)n;
        return HU_OK;
    }
    return HU_OK;
}

hu_reciprocity_metrics_t hu_reciprocity_compute(uint32_t our_initiations,
                                                uint32_t their_initiations, uint32_t our_questions,
                                                uint32_t their_questions, uint32_t our_shares,
                                                uint32_t their_shares, uint32_t our_responses,
                                                uint32_t their_messages) {
    hu_reciprocity_metrics_t m = {0};
    uint32_t total_init = our_initiations + their_initiations;
    m.initiation_ratio = (total_init > 0) ? (double)our_initiations / (double)total_init : 0.5;

    uint32_t total_q = our_questions + their_questions;
    m.question_balance = (total_q > 0) ? (double)our_questions / (double)total_q : 0.5;

    uint32_t total_s = our_shares + their_shares;
    m.share_balance = (total_s > 0) ? (double)our_shares / (double)total_s : 0.5;

    m.response_ratio = (their_messages > 0) ? (double)our_responses / (double)their_messages : 0.0;
    return m;
}

hu_error_t hu_reciprocity_upsert_sql(const char *contact_id, size_t contact_id_len,
                                     const hu_reciprocity_metrics_t *metrics, char *buf, size_t cap,
                                     size_t *out_len) {
    if (!contact_id || contact_id_len == 0 || !metrics || !buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;

    char contact_esc[HU_SELF_AWARENESS_ESCAPE_BUF];
    size_t ce_len;
    escape_sql_string(contact_id, contact_id_len, contact_esc, sizeof(contact_esc), &ce_len);

    int64_t now_ts = (int64_t)time(NULL);

    int n = snprintf(
        buf, cap,
        "INSERT OR REPLACE INTO reciprocity_scores (contact_id, metric, value, updated_at) "
        "VALUES ('%s', 'initiation_ratio', %f, %lld); "
        "INSERT OR REPLACE INTO reciprocity_scores (contact_id, metric, value, updated_at) "
        "VALUES ('%s', 'question_balance', %f, %lld); "
        "INSERT OR REPLACE INTO reciprocity_scores (contact_id, metric, value, updated_at) "
        "VALUES ('%s', 'share_balance', %f, %lld); "
        "INSERT OR REPLACE INTO reciprocity_scores (contact_id, metric, value, updated_at) "
        "VALUES ('%s', 'response_ratio', %f, %lld)",
        contact_esc, metrics->initiation_ratio, (long long)now_ts, contact_esc,
        metrics->question_balance, (long long)now_ts, contact_esc, metrics->share_balance,
        (long long)now_ts, contact_esc, metrics->response_ratio, (long long)now_ts);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_reciprocity_build_directive(hu_allocator_t *alloc,
                                          const hu_reciprocity_metrics_t *metrics, char **out,
                                          size_t *out_len) {
    if (!alloc || !metrics || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    char parts[4][512];
    size_t part_count = 0;

    if (metrics->initiation_ratio < 0.35) {
        snprintf(parts[part_count], sizeof(parts[part_count]),
                 "You've been receiving more than initiating. Consider reaching out.");
        part_count++;
    }
    if (metrics->question_balance < 0.4) {
        snprintf(parts[part_count], sizeof(parts[part_count]),
                 "They've asked more about you. Ask about them.");
        part_count++;
    }
    if (metrics->share_balance > 0.65) {
        snprintf(parts[part_count], sizeof(parts[part_count]),
                 "You've been sharing a lot — give them space to share.");
        part_count++;
    }

    if (part_count == 0)
        return HU_OK;

    char combined[1024];
    size_t pos = 0;
    for (size_t i = 0; i < part_count && pos < sizeof(combined) - 1; i++) {
        if (i > 0) {
            combined[pos++] = ' ';
            combined[pos++] = ' ';
        }
        size_t plen = strlen(parts[i]);
        if (pos + plen >= sizeof(combined))
            plen = sizeof(combined) - pos - 1;
        memcpy(combined + pos, parts[i], plen);
        pos += plen;
    }
    combined[pos] = '\0';

    char *s = hu_strndup(alloc, combined, pos);
    if (!s)
        return HU_ERR_OUT_OF_MEMORY;
    *out = s;
    *out_len = pos;
    return HU_OK;
}

void hu_self_stats_deinit(hu_allocator_t *alloc, hu_self_stats_t *stats) {
    if (!alloc || !stats)
        return;
    hu_str_free(alloc, stats->contact_id);
    stats->contact_id = NULL;
    stats->contact_id_len = 0;
    hu_str_free(alloc, stats->last_topic);
    stats->last_topic = NULL;
    stats->last_topic_len = 0;
}

#ifdef HU_ENABLE_SQLITE

/* These executors historically ignored `alloc` (they pulled the db straight
 * from `memory`). The repo factory needs a real allocator for its small ctx, so
 * fall back to the system allocator when a caller passes NULL — preserving the
 * prior NULL-tolerance. */
static hu_error_t open_self_awareness_repo(hu_allocator_t *alloc, hu_memory_t *memory,
                                           hu_self_awareness_repo_t *repo,
                                           hu_allocator_t *fallback) {
    if (!alloc) {
        *fallback = hu_system_allocator();
        alloc = fallback;
    }
    return hu_self_awareness_repo_create(memory, alloc, repo);
}

hu_error_t hu_self_awareness_record_send(hu_allocator_t *alloc, hu_memory_t *memory,
                                         const char *contact_id, size_t contact_id_len,
                                         bool we_initiated, const char *topic_hint,
                                         size_t topic_len) {
    if (!memory || !contact_id || contact_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    hu_self_awareness_repo_t repo;
    hu_allocator_t fb;
    hu_error_t e = open_self_awareness_repo(alloc, memory, &repo, &fb);
    if (e != HU_OK)
        return e; /* HU_ERR_NOT_SUPPORTED for non-sqlite, as before */

    int64_t now_ts = (int64_t)time(NULL);
    int init_inc = we_initiated ? 1 : 0;
    e = repo.vtable->stats_record_send(repo.ctx, contact_id, contact_id_len, init_inc, topic_hint,
                                       topic_len, now_ts);
    repo.vtable->deinit(repo.ctx);
    return e;
}

hu_error_t hu_self_awareness_build_directive_from_memory(hu_allocator_t *alloc, hu_memory_t *memory,
                                                         const char *contact_id,
                                                         size_t contact_id_len, int64_t now_ts,
                                                         char **out, size_t *out_len) {
    (void)now_ts;
    if (!alloc || !memory || !contact_id || contact_id_len == 0 || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    /* alloc is validated non-NULL above. */
    hu_self_awareness_repo_t repo;
    hu_allocator_t fb;
    hu_error_t e = open_self_awareness_repo(alloc, memory, &repo, &fb);
    if (e != HU_OK)
        return e;

    bool found = false;
    hu_self_awareness_stats_row_t row;
    e = repo.vtable->stats_get(repo.ctx, contact_id, contact_id_len, &found, &row);
    repo.vtable->deinit(repo.ctx);
    if (e != HU_OK || !found)
        return e;

    /* Business logic (unchanged): quiet-ratio and topic-repeat directives. */
    if (row.messages_sent_week > 0) {
        double ratio = (double)row.initiations_week / (double)row.messages_sent_week;
        if (ratio < 0.3) {
            const char *msg = "I've been kind of quiet lately, sorry";
            size_t len = 32;
            char *s = hu_strndup(alloc, msg, len);
            if (!s)
                return HU_ERR_OUT_OF_MEMORY;
            *out = s;
            *out_len = len;
            return HU_OK;
        }
    }
    if (row.topic_repeat_count > 3 && row.last_topic_len > 0) {
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "I know I keep talking about %.*s",
                         (int)row.last_topic_len, row.last_topic);
        if (n > 0 && (size_t)n < sizeof(buf)) {
            char *s = hu_strndup(alloc, buf, (size_t)n);
            if (!s)
                return HU_ERR_OUT_OF_MEMORY;
            *out = s;
            *out_len = (size_t)n;
            return HU_OK;
        }
    }
    return HU_OK;
}

static double get_reciprocity_metric(hu_self_awareness_repo_t *repo, const char *contact_id,
                                     size_t contact_id_len, const char *metric) {
    return repo->vtable->reciprocity_get(repo->ctx, contact_id, contact_id_len, metric);
}

static hu_error_t set_reciprocity_metric(hu_self_awareness_repo_t *repo, const char *contact_id,
                                         size_t contact_id_len, const char *metric, double value,
                                         int64_t now_ts) {
    return repo->vtable->reciprocity_set(repo->ctx, contact_id, contact_id_len, metric, value,
                                         now_ts);
}

hu_error_t hu_self_awareness_get_reciprocity(hu_allocator_t *alloc, hu_memory_t *memory,
                                             const char *contact_id, size_t contact_id_len,
                                             double *initiation_ratio, double *question_balance,
                                             double *share_balance) {
    if (!memory || !contact_id || contact_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    hu_self_awareness_repo_t repo;
    hu_allocator_t fb;
    hu_error_t e = open_self_awareness_repo(alloc, memory, &repo, &fb);
    if (e != HU_OK)
        return e;

    if (initiation_ratio)
        *initiation_ratio =
            get_reciprocity_metric(&repo, contact_id, contact_id_len, "initiation_ratio");
    if (question_balance)
        *question_balance =
            get_reciprocity_metric(&repo, contact_id, contact_id_len, "question_balance");
    if (share_balance)
        *share_balance = get_reciprocity_metric(&repo, contact_id, contact_id_len, "share_balance");
    repo.vtable->deinit(repo.ctx);
    return HU_OK;
}

static hu_error_t update_reciprocity_impl(hu_allocator_t *alloc, hu_memory_t *memory,
                                          const char *contact_id, size_t contact_id_len,
                                          bool we_init, bool we_q, bool we_s, bool they_init,
                                          bool they_q, bool they_s) {
    hu_self_awareness_repo_t repo;
    hu_allocator_t fb;
    hu_error_t e = open_self_awareness_repo(alloc, memory, &repo, &fb);
    if (e != HU_OK)
        return e;

    int64_t now_ts = (int64_t)time(NULL);

    double our_init = get_reciprocity_metric(&repo, contact_id, contact_id_len, "our_initiations");
    double their_init =
        get_reciprocity_metric(&repo, contact_id, contact_id_len, "their_initiations");
    double our_q = get_reciprocity_metric(&repo, contact_id, contact_id_len, "our_questions");
    double their_q = get_reciprocity_metric(&repo, contact_id, contact_id_len, "their_questions");
    double our_s = get_reciprocity_metric(&repo, contact_id, contact_id_len, "our_shares");
    double their_s = get_reciprocity_metric(&repo, contact_id, contact_id_len, "their_shares");

    if (we_init)
        our_init += 1.0;
    if (they_init)
        their_init += 1.0;
    if (we_q)
        our_q += 1.0;
    if (they_q)
        their_q += 1.0;
    if (we_s)
        our_s += 1.0;
    if (they_s)
        their_s += 1.0;

    double init_ratio = 0.5;
    if (our_init + their_init > 0)
        init_ratio = our_init / (our_init + their_init);
    double q_balance = 0.5;
    if (our_q + their_q > 0)
        q_balance = our_q / (our_q + their_q);
    double s_balance = 0.5;
    if (our_s + their_s > 0)
        s_balance = our_s / (our_s + their_s);

    /* Write all metrics; stop at first failure (same as the pre-repo code),
     * then deinit exactly once. */
    hu_error_t err = HU_OK;
    if (err == HU_OK)
        err = set_reciprocity_metric(&repo, contact_id, contact_id_len, "our_initiations", our_init,
                                     now_ts);
    if (err == HU_OK)
        err = set_reciprocity_metric(&repo, contact_id, contact_id_len, "their_initiations",
                                     their_init, now_ts);
    if (err == HU_OK)
        err = set_reciprocity_metric(&repo, contact_id, contact_id_len, "our_questions", our_q,
                                     now_ts);
    if (err == HU_OK)
        err = set_reciprocity_metric(&repo, contact_id, contact_id_len, "their_questions", their_q,
                                     now_ts);
    if (err == HU_OK)
        err =
            set_reciprocity_metric(&repo, contact_id, contact_id_len, "our_shares", our_s, now_ts);
    if (err == HU_OK)
        err = set_reciprocity_metric(&repo, contact_id, contact_id_len, "their_shares", their_s,
                                     now_ts);
    if (err == HU_OK)
        err = set_reciprocity_metric(&repo, contact_id, contact_id_len, "initiation_ratio",
                                     init_ratio, now_ts);
    if (err == HU_OK)
        err = set_reciprocity_metric(&repo, contact_id, contact_id_len, "question_balance",
                                     q_balance, now_ts);
    if (err == HU_OK)
        err = set_reciprocity_metric(&repo, contact_id, contact_id_len, "share_balance", s_balance,
                                     now_ts);

    repo.vtable->deinit(repo.ctx);
    return err;
}

hu_error_t hu_self_awareness_update_reciprocity(hu_allocator_t *alloc, hu_memory_t *memory,
                                                const char *contact_id, size_t contact_id_len,
                                                bool we_initiated, bool we_asked_question,
                                                bool we_shared) {
    if (!memory || !contact_id || contact_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    return update_reciprocity_impl(alloc, memory, contact_id, contact_id_len, we_initiated,
                                   we_asked_question, we_shared, false, false, false);
}

hu_error_t hu_self_awareness_record_their_reciprocity(hu_allocator_t *alloc, hu_memory_t *memory,
                                                      const char *contact_id, size_t contact_id_len,
                                                      bool they_initiated, bool they_asked_question,
                                                      bool they_shared) {
    if (!memory || !contact_id || contact_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    return update_reciprocity_impl(alloc, memory, contact_id, contact_id_len, false, false, false,
                                   they_initiated, they_asked_question, they_shared);
}

hu_error_t hu_self_awareness_build_reciprocity_directive(hu_allocator_t *alloc, hu_memory_t *memory,
                                                         const char *contact_id,
                                                         size_t contact_id_len, char **out,
                                                         size_t *out_len) {
    if (!alloc || !memory || !contact_id || contact_id_len == 0 || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    double init_ratio = 0.5, q_balance = 0.5, s_balance = 0.5;
    hu_error_t err = hu_self_awareness_get_reciprocity(alloc, memory, contact_id, contact_id_len,
                                                       &init_ratio, &q_balance, &s_balance);
    if (err != HU_OK)
        return err;

    hu_reciprocity_metrics_t m = {
        .initiation_ratio = init_ratio,
        .question_balance = q_balance,
        .share_balance = s_balance,
        .response_ratio = 0.5,
    };
    return hu_reciprocity_build_directive(alloc, &m, out, out_len);
}

#endif /* HU_ENABLE_SQLITE */
