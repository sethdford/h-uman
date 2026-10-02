/* context_relevance.c — HU_CONTEXT_RELEVANCE: one relevance decision in place
 * of the word-count / tier cliffs that kept memory out of short turns. See
 * include/human/memory/context_relevance.h for the contract. */
#include "human/memory/context_relevance.h"

#include "human/core/log.h"
#include "human/memory/semantic_recall.h"
#include "human/memory/vector/store_sqlite_vec.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hu_gate_mode_t hu_context_relevance_mode(void) {
    return hu_gate_mode_from_env("HU_CONTEXT_RELEVANCE", HU_GATE_OFF);
}

/* ── null pool: what unrelated memories score against real queries ───── */

static pthread_mutex_t s_null_mu = PTHREAD_MUTEX_INITIALIZER;
static float s_null[HU_CONTEXT_RELEVANCE_NULL_POOL];
static size_t s_null_n, s_null_head;

void hu_context_relevance_null_add(const float *scores, size_t n) {
    if (!scores || n == 0)
        return;
    pthread_mutex_lock(&s_null_mu);
    for (size_t i = 0; i < n; i++) {
        s_null[s_null_head] = scores[i];
        s_null_head = (s_null_head + 1) % HU_CONTEXT_RELEVANCE_NULL_POOL;
        if (s_null_n < HU_CONTEXT_RELEVANCE_NULL_POOL)
            s_null_n++;
    }
    pthread_mutex_unlock(&s_null_mu);
}

size_t hu_context_relevance_null_count(void) {
    pthread_mutex_lock(&s_null_mu);
    size_t n = s_null_n;
    pthread_mutex_unlock(&s_null_mu);
    return n;
}

void hu_context_relevance_null_reset(void) {
    pthread_mutex_lock(&s_null_mu);
    s_null_n = 0;
    s_null_head = 0;
    pthread_mutex_unlock(&s_null_mu);
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

double hu_context_relevance_p95(const float *v, size_t n) {
    if (!v || n == 0)
        return 0.0;
    float *tmp = (float *)malloc(n * sizeof(float));
    if (!tmp)
        return 0.0;
    memcpy(tmp, v, n * sizeof(float));
    qsort(tmp, n, sizeof(float), cmp_float);
    size_t rank = (size_t)((95 * n + 99) / 100); /* nearest rank, 1-based */
    double q = (double)tmp[(rank ? rank : 1) - 1];
    free(tmp);
    return q;
}

static bool env_min_score(double *out) {
    const char *v = getenv("HU_CONTEXT_RELEVANCE_MIN_SCORE");
    if (!v || !v[0])
        return false;
    char *end = NULL;
    errno = 0;
    double d = strtod(v, &end);
    if (errno != 0 || end == v || (end && *end != '\0') || d < -1.0 || d > 1.0)
        return false; /* ignored: fall through to the null pool / cold start */
    *out = d;
    return true;
}

double hu_context_relevance_threshold(hu_context_relevance_threshold_src_t *src) {
    double t = HU_CONTEXT_RELEVANCE_DEFAULT_MIN_SCORE;
    hu_context_relevance_threshold_src_t from = HU_CR_THRESHOLD_COLD;
    if (env_min_score(&t)) {
        from = HU_CR_THRESHOLD_ENV;
    } else {
        pthread_mutex_lock(&s_null_mu);
        if (s_null_n >= HU_CONTEXT_RELEVANCE_NULL_MIN) {
            t = hu_context_relevance_p95(s_null, s_null_n);
            from = HU_CR_THRESHOLD_NULL;
        }
        pthread_mutex_unlock(&s_null_mu);
    }
    if (src)
        *src = from;
    return t;
}

double hu_context_relevance_min_score(void) {
    return hu_context_relevance_threshold(NULL);
}

size_t hu_context_relevance_casual_bytes(void) {
    const char *v = getenv("HU_CONTEXT_RELEVANCE_CASUAL_BYTES");
    if (!v || !v[0])
        return HU_CONTEXT_RELEVANCE_DEFAULT_CASUAL_BYTES;
    char *end = NULL;
    errno = 0;
    long n = strtol(v, &end, 10);
    if (errno != 0 || end == v || (end && *end != '\0') || n <= 0)
        return HU_CONTEXT_RELEVANCE_DEFAULT_CASUAL_BYTES;
    return (size_t)n;
}

static size_t score_band(double s) {
    if (s <= 0.0)
        return 0;
    size_t b = (size_t)(s * (double)HU_CONTEXT_RELEVANCE_HIST_BANDS + 1e-9);
    return b < HU_CONTEXT_RELEVANCE_HIST_BANDS ? b : HU_CONTEXT_RELEVANCE_HIST_BANDS - 1;
}

void hu_context_relevance_assess(const hu_retrieval_result_t *res, double min_score,
                                 size_t budget_bytes, size_t per_hit_bytes,
                                 hu_context_relevance_stats_t *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!res || !res->entries)
        return;
    bool budget_open = true; /* the clamp stops at the first hit that does not fit */
    for (size_t i = 0; i < res->count; i++) {
        const hu_memory_entry_t *e = &res->entries[i];
        if (hu_semantic_recall_hit_is_excluded(e->key, e->key_len, e->content, e->content_len))
            continue;
        double s = res->scores ? res->scores[i] : e->score;
        out->items_considered++;
        out->hist[score_band(s)]++;
        if (out->items_considered == 1 || s > out->top_score)
            out->top_score = s;
        if (s < min_score)
            continue;
        out->items_passing++;
        if (!budget_open)
            continue;
        size_t len =
            e->content ? hu_semantic_recall_truncate_len(e->content, e->content_len, per_hit_bytes)
                       : 0;
        if (out->bytes_would_inject + len > budget_bytes) {
            budget_open = false;
            continue;
        }
        out->bytes_would_inject += len;
    }
}

static const char *src_name(hu_context_relevance_threshold_src_t src) {
    return src == HU_CR_THRESHOLD_ENV ? "env" : (src == HU_CR_THRESHOLD_NULL ? "null" : "cold");
}

/* "a/b/c..." of n counts, or the top scores at 0.05 resolution. */
static void fmt_hist(char *buf, size_t cap, const uint32_t *h, size_t n) {
    size_t pos = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < n && pos < cap; i++) {
        int w = snprintf(buf + pos, cap - pos, i ? "/%u" : "%u", h[i]);
        if (w <= 0 || (size_t)w >= cap - pos)
            break;
        pos += (size_t)w;
    }
}

static void fmt_top(char *buf, size_t cap, const hu_retrieval_result_t *res) {
    double top[HU_CONTEXT_RELEVANCE_TOP_LOGGED];
    size_t n = 0;
    for (size_t i = 0; res && res->entries && i < res->count; i++) {
        double s = res->scores ? res->scores[i] : res->entries[i].score;
        if (n < HU_CONTEXT_RELEVANCE_TOP_LOGGED)
            top[n++] = s;
        else if (s > top[n - 1])
            top[n - 1] = s;
        else
            continue;
        for (size_t j = n - 1; j > 0 && top[j] > top[j - 1]; j--) { /* keep descending */
            double t = top[j];
            top[j] = top[j - 1];
            top[j - 1] = t;
        }
    }
    size_t pos = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < n && pos < cap; i++) {
        double q = (double)(long)(top[i] * 20.0 + (top[i] >= 0 ? 0.5 : -0.5)) / 20.0;
        int w = snprintf(buf + pos, cap - pos, i ? ",%.2f" : "%.2f", q);
        if (w <= 0 || (size_t)w >= cap - pos)
            break;
        pos += (size_t)w;
    }
}

/* Score the message against K random stored vectors: the null sample. */
static size_t null_sample(const hu_vector_store_t *store, const hu_embedding_t *query, float *out) {
    if (!store || !query || !query->values)
        return 0;
#ifndef HU_ENABLE_SQLITE
    /* store_sqlite_vec.c is not compiled without SQLite: no null sample. */
    (void)out;
    return 0;
#else
    size_t n = 0;
    if (hu_vector_store_sqlite_vec_sample_scores(store, query, HU_CONTEXT_RELEVANCE_NULL_K, out,
                                                 &n) != HU_OK)
        return 0;
    hu_context_relevance_null_add(out, n);
    return n;
#endif
}

bool hu_context_relevance_semantic(hu_allocator_t *alloc, hu_retrieval_result_t *sem, bool casual,
                                   size_t *budget_bytes, const hu_vector_store_t *store,
                                   const hu_embedding_t *query) {
    hu_gate_mode_t mode = hu_context_relevance_mode();
    if (mode == HU_GATE_OFF || !alloc || !sem || !budget_bytes)
        return false;
    float nulls[HU_CONTEXT_RELEVANCE_NULL_K];
    size_t null_n = null_sample(store, query, nulls);
    hu_context_relevance_threshold_src_t src = HU_CR_THRESHOLD_COLD;
    double min_score = hu_context_relevance_threshold(&src);
    size_t budget = *budget_bytes;
    if (casual) {
        size_t cb = hu_context_relevance_casual_bytes();
        if (cb < budget)
            budget = cb;
    }
    hu_context_relevance_stats_t st;
    hu_context_relevance_assess(sem, min_score, budget, HU_SEMANTIC_RECALL_HIT_MAX_BYTES, &st);
    char hist[HU_CONTEXT_RELEVANCE_HIST_BANDS * 6], top[HU_CONTEXT_RELEVANCE_TOP_LOGGED * 7];
    fmt_hist(hist, sizeof(hist), st.hist, HU_CONTEXT_RELEVANCE_HIST_BANDS);
    fmt_top(top, sizeof(top), sem);
    /* Substantive turns are not decided until the threshold is calibrated. */
    bool decides = mode == HU_GATE_LIVE && casual;
    hu_log_info("memory", NULL,
                "[context_relevance %s] src=semantic casual=%d applied=%d items_considered=%zu "
                "items_passing=%zu bytes_would_inject=%zu top_score=%.3f min_score=%.3f "
                "threshold_src=%s null_k=%zu null_p95=%.3f null_pool=%zu top=%s hist=%s",
                mode == HU_GATE_LIVE ? "live" : "shadow", casual ? 1 : 0, decides ? 1 : 0,
                st.items_considered, st.items_passing, st.bytes_would_inject, st.top_score,
                min_score, src_name(src), null_n, hu_context_relevance_p95(nulls, null_n),
                hu_context_relevance_null_count(), top[0] ? top : "-", hist);
    if (!decides)
        return false;
    (void)hu_semantic_recall_drop_below(alloc, sem, min_score);
    *budget_bytes = budget;
    return true;
}

size_t hu_context_relevance_line_cut(const char *s, size_t len, size_t max_bytes) {
    if (!s || len == 0)
        return 0;
    if (len <= max_bytes)
        return len;
    for (size_t i = max_bytes; i > 0; i--) {
        if (s[i - 1] == '\n')
            return i;
    }
    return 0;
}

bool hu_context_relevance_graph(hu_allocator_t *alloc, char **ctx, size_t *ctx_len,
                                size_t relevance) {
    hu_gate_mode_t mode = hu_context_relevance_mode();
    if (mode == HU_GATE_OFF || !alloc || !ctx || !ctx_len)
        return false;
    size_t budget = hu_context_relevance_casual_bytes();
    hu_context_relevance_stats_t st;
    memset(&st, 0, sizeof(st));
    st.items_considered = (*ctx && *ctx_len > 0) ? 1 : 0;
    st.items_passing = (st.items_considered && relevance > 0) ? 1 : 0;
    st.bytes_would_inject =
        st.items_passing ? hu_context_relevance_line_cut(*ctx, *ctx_len, budget) : 0;
    st.top_score = (double)relevance;
    hu_log_info("memory", NULL,
                "[context_relevance %s] src=graph casual=1 items_considered=%zu items_passing=%zu "
                "bytes_would_inject=%zu matched_full_name=%zu",
                mode == HU_GATE_LIVE ? "live" : "shadow", st.items_considered, st.items_passing,
                st.bytes_would_inject, relevance);
    if (mode != HU_GATE_LIVE || st.bytes_would_inject == 0)
        return false;
    if (st.bytes_would_inject < *ctx_len) {
        char *shrunk =
            (char *)alloc->realloc(alloc->ctx, *ctx, *ctx_len + 1, st.bytes_would_inject + 1);
        if (shrunk)
            *ctx = shrunk;
        (*ctx)[st.bytes_would_inject] = '\0';
        *ctx_len = st.bytes_would_inject;
    }
    return true;
}
