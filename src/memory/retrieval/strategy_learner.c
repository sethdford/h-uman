#ifdef HU_ENABLE_SQLITE

#include "human/memory/retrieval/strategy_learner.h"
#include <ctype.h>
#include <string.h>

static int strncasecmp_prefix(const char *s, size_t len, const char *prefix) {
    size_t pl = strlen(prefix);
    if (pl > len)
        return -1;
    for (size_t i = 0; i < pl; i++) {
        char a = (char)tolower((unsigned char)s[i]);
        char b = (char)tolower((unsigned char)prefix[i]);
        if (a != b)
            return (int)(unsigned char)a - (unsigned char)b;
    }
    return 0;
}

static int strncasecmp_substr(const char *s, size_t len, const char *sub) {
    size_t sl = strlen(sub);
    if (sl > len)
        return -1;
    for (size_t i = 0; i <= len - sl; i++) {
        int cmp = strncasecmp_prefix(s + i, len - i, sub);
        if (cmp == 0)
            return 0;
    }
    return -1;
}

hu_query_category_t hu_strategy_classify_query(const char *query, size_t query_len) {
    if (!query || query_len == 0)
        return HU_QCAT_SEMANTIC;

    /* FACTUAL: starts with "what is", "who is", "when did", "where is" */
    if (strncasecmp_prefix(query, query_len, "what is") == 0 ||
        strncasecmp_prefix(query, query_len, "what are") == 0 ||
        strncasecmp_prefix(query, query_len, "who is") == 0 ||
        strncasecmp_prefix(query, query_len, "when did") == 0 ||
        strncasecmp_prefix(query, query_len, "when was") == 0 ||
        strncasecmp_prefix(query, query_len, "where is") == 0 ||
        strncasecmp_prefix(query, query_len, "which ") == 0)
        return HU_QCAT_FACTUAL;

    /* PROCEDURAL: starts with "how to", "how do", "steps for", "guide to" */
    if (strncasecmp_prefix(query, query_len, "how to") == 0 ||
        strncasecmp_prefix(query, query_len, "how do ") == 0 ||
        strncasecmp_prefix(query, query_len, "steps for") == 0 ||
        strncasecmp_prefix(query, query_len, "guide to") == 0)
        return HU_QCAT_PROCEDURAL;

    /* PERSONAL: contains "my ", "I ", "me ", "last time I" */
    if (strncasecmp_substr(query, query_len, "my ") == 0 ||
        strncasecmp_substr(query, query_len, " I ") == 0 ||
        strncasecmp_substr(query, query_len, " me ") == 0 ||
        strncasecmp_substr(query, query_len, "last time i") == 0)
        return HU_QCAT_PERSONAL;

    /* TEMPORAL: contains "yesterday", "last week", "today", "ago", date patterns */
    if (strncasecmp_substr(query, query_len, "yesterday") == 0 ||
        strncasecmp_substr(query, query_len, "last week") == 0 ||
        strncasecmp_substr(query, query_len, "last month") == 0 ||
        strncasecmp_substr(query, query_len, "today") == 0 ||
        strncasecmp_substr(query, query_len, " ago") == 0 ||
        strncasecmp_substr(query, query_len, "202") == 0)
        return HU_QCAT_TEMPORAL;

    /* EXACT: contains code-like chars (_, ::, ->, .), or quoted strings */
    for (size_t i = 0; i < query_len; i++) {
        char c = query[i];
        if (c == '_' || c == '"' || c == '\'')
            return HU_QCAT_EXACT;
        if (i + 1 < query_len) {
            if ((c == ':' && query[i + 1] == ':') || (c == '-' && query[i + 1] == '>'))
                return HU_QCAT_EXACT;
        }
    }

    return HU_QCAT_SEMANTIC;
}

hu_error_t hu_strategy_learner_create(hu_allocator_t *alloc, sqlite3 *db,
                                       hu_strategy_learner_t *out) {
    if (!alloc || !db || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->alloc = alloc;
    out->db = db;
    return HU_OK;
}

void hu_strategy_learner_deinit(hu_strategy_learner_t *learner) {
    (void)learner;
}

hu_error_t hu_strategy_learner_init_tables(hu_strategy_learner_t *learner) {
    if (!learner || !learner->db)
        return HU_ERR_INVALID_ARGUMENT;

    const char *sql =
        "CREATE TABLE IF NOT EXISTS strategy_outcomes(id INTEGER PRIMARY KEY, category INTEGER, "
        "strategy INTEGER, success INTEGER, timestamp INTEGER)";
    char *errmsg = NULL;
    int rc = sqlite3_exec(learner->db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        if (errmsg) {
            sqlite3_free(errmsg);
        }
        return HU_ERR_MEMORY_BACKEND;
    }
    return HU_OK;
}

hu_error_t hu_strategy_learner_record(hu_strategy_learner_t *learner,
                                       hu_query_category_t category,
                                       hu_retrieval_strategy_t strategy,
                                       bool success, int64_t now_ts) {
    if (!learner || !learner->db)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO strategy_outcomes(category, strategy, success, timestamp) VALUES(?,?,?,?)";
    int rc = sqlite3_prepare_v2(learner->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_int(stmt, 1, (int)category);
    sqlite3_bind_int(stmt, 2, (int)strategy);
    sqlite3_bind_int(stmt, 3, success ? 1 : 0);
    sqlite3_bind_int64(stmt, 4, now_ts);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;
    return HU_OK;
}

hu_retrieval_strategy_t hu_strategy_learner_recommend(hu_strategy_learner_t *learner,
                                                       hu_query_category_t category) {
    if (!learner || !learner->db)
        return HU_RSTRAT_HYBRID;

    const char *sql =
        "SELECT strategy, COUNT(*) as attempts, SUM(success) as wins FROM strategy_outcomes "
        "WHERE category=? GROUP BY strategy ORDER BY CAST(SUM(success) AS REAL)/COUNT(*) DESC "
        "LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(learner->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_RSTRAT_HYBRID;

    sqlite3_bind_int(stmt, 1, (int)category);
    rc = sqlite3_step(stmt);
    hu_retrieval_strategy_t result = HU_RSTRAT_HYBRID;
    if (rc == SQLITE_ROW) {
        int strat = sqlite3_column_int(stmt, 0);
        if (strat >= 0 && strat < HU_RSTRAT_COUNT)
            result = (hu_retrieval_strategy_t)strat;
    }
    sqlite3_finalize(stmt);
    return result;
}

hu_error_t hu_strategy_learner_get_stats(hu_strategy_learner_t *learner,
                                          hu_query_category_t category,
                                          hu_retrieval_strategy_t strategy,
                                          hu_strategy_stats_t *out) {
    if (!learner || !learner->db || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    out->strategy = strategy;

    const char *sql =
        "SELECT COUNT(*), SUM(success) FROM strategy_outcomes WHERE category=? AND strategy=?";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(learner->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_int(stmt, 1, (int)category);
    sqlite3_bind_int(stmt, 2, (int)strategy);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        out->attempts = (int32_t)sqlite3_column_int(stmt, 0);
        out->successes = (int32_t)sqlite3_column_int(stmt, 1);
        if (out->attempts > 0)
            out->precision = (double)out->successes / (double)out->attempts;
    }
    sqlite3_finalize(stmt);
    return HU_OK;
}

const char *hu_query_category_str(hu_query_category_t cat) {
    static const char *names[] = {
        "factual", "procedural", "personal", "temporal", "semantic", "exact",
    };
    if (cat < HU_QCAT_COUNT)
        return names[cat];
    return "unknown";
}

const char *hu_retrieval_strategy_str(hu_retrieval_strategy_t strat) {
    static const char *names[] = {
        "keyword", "vector", "hybrid", "temporal", "graph",
    };
    if (strat < HU_RSTRAT_COUNT)
        return names[strat];
    return "unknown";
}

#endif /* HU_ENABLE_SQLITE */

typedef int hu_strategy_learner_empty_unit_guard;
