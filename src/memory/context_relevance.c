/* context_relevance.c — HU_CONTEXT_RELEVANCE: one relevance decision in place
 * of the word-count / tier cliffs that kept memory out of short turns. See
 * include/human/memory/context_relevance.h for the contract. */
#include "human/memory/context_relevance.h"

#include "human/core/log.h"
#include "human/memory/semantic_recall.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

hu_gate_mode_t hu_context_relevance_mode(void) {
    return hu_gate_mode_from_env("HU_CONTEXT_RELEVANCE", HU_GATE_OFF);
}

double hu_context_relevance_min_score(void) {
    const char *v = getenv("HU_CONTEXT_RELEVANCE_MIN_SCORE");
    if (!v || !v[0])
        return HU_CONTEXT_RELEVANCE_DEFAULT_MIN_SCORE;
    char *end = NULL;
    errno = 0;
    double d = strtod(v, &end);
    if (errno != 0 || end == v || (end && *end != '\0') || d < -1.0 || d > 1.0)
        return HU_CONTEXT_RELEVANCE_DEFAULT_MIN_SCORE; /* fail closed to the default */
    return d;
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
    if (s < 0.2)
        return 0;
    if (s < 0.4)
        return 1;
    if (s < 0.6)
        return 2;
    if (s < 0.8)
        return 3;
    return 4;
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

/* One aggregate line per decision: counts, bytes and scores only. */
static void relevance_log(const char *src, hu_gate_mode_t mode, bool casual, double min_score,
                          const hu_context_relevance_stats_t *st) {
    hu_log_info("memory", NULL,
                "[context_relevance %s] src=%s casual=%d items_considered=%zu "
                "items_passing=%zu bytes_would_inject=%zu top_score=%.3f min_score=%.3f "
                "hist=%u/%u/%u/%u/%u",
                mode == HU_GATE_LIVE ? "live" : "shadow", src, casual ? 1 : 0, st->items_considered,
                st->items_passing, st->bytes_would_inject, st->top_score, min_score, st->hist[0],
                st->hist[1], st->hist[2], st->hist[3], st->hist[4]);
}

bool hu_context_relevance_semantic(hu_allocator_t *alloc, hu_retrieval_result_t *sem, bool casual,
                                   size_t *budget_bytes) {
    hu_gate_mode_t mode = hu_context_relevance_mode();
    if (mode == HU_GATE_OFF || !alloc || !sem || !budget_bytes)
        return false;
    double min_score = hu_context_relevance_min_score();
    size_t budget = *budget_bytes;
    if (casual) {
        size_t cb = hu_context_relevance_casual_bytes();
        if (cb < budget)
            budget = cb;
    }
    hu_context_relevance_stats_t st;
    hu_context_relevance_assess(sem, min_score, budget, HU_SEMANTIC_RECALL_HIT_MAX_BYTES, &st);
    relevance_log("semantic", mode, casual, min_score, &st);
    if (mode != HU_GATE_LIVE)
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
    relevance_log("graph", mode, true, 1.0, &st);
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
