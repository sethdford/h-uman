#include "seaclaw/memory/retrieval.h"
#include "seaclaw/memory.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SC_MMR_LAMBDA_DEFAULT 0.7

/* Extract words from text into a simple structure for Jaccard */
typedef struct {
    char **words;
    size_t count;
    size_t cap;
} word_set_t;

static void word_set_init(word_set_t *ws) {
    ws->words = NULL;
    ws->count = 0;
    ws->cap = 0;
}

static void word_set_add(word_set_t *ws, const char *start, size_t len,
    sc_allocator_t *alloc) {
    if (len == 0) return;
    if (ws->count >= ws->cap) {
        size_t new_cap = ws->cap ? ws->cap * 2 : 16;
        char **n = (char **)alloc->realloc(alloc->ctx, ws->words,
            ws->cap * sizeof(char *), new_cap * sizeof(char *));
        if (!n) return;
        ws->words = n;
        ws->cap = new_cap;
    }
    char *w = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!w) return;
    memcpy(w, start, len);
    w[len] = '\0';
    for (size_t i = 0; i < len; i++)
        w[i] = (char)(unsigned char)tolower((unsigned char)w[i]);
    ws->words[ws->count++] = w;
}

static void word_set_from_text(word_set_t *ws, const char *text, size_t len,
    sc_allocator_t *alloc) {
    word_set_init(ws);
    const char *p = text;
    const char *end = text + len;
    while (p < end) {
        while (p < end && !isalnum((unsigned char)*p) && *p != '_') p++;
        if (p >= end) break;
        const char *start = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '_')) p++;
        word_set_add(ws, start, (size_t)(p - start), alloc);
    }
}

static void word_set_cleanup(word_set_t *ws, sc_allocator_t *alloc) {
    if (ws->words) {
        for (size_t i = 0; i < ws->count; i++)
            alloc->free(alloc->ctx, ws->words[i], strlen(ws->words[i]) + 1);
        alloc->free(alloc->ctx, ws->words, ws->cap * sizeof(char *));
        ws->words = NULL;
        ws->count = 0;
        ws->cap = 0;
    }
}

/* Jaccard similarity = |intersection| / |union| */
static double jaccard_similarity(const word_set_t *a, const word_set_t *b) {
    if (a->count == 0 && b->count == 0) return 0.0;
    if (a->count == 0 || b->count == 0) return 0.0;
    size_t inter = 0;
    for (size_t i = 0; i < a->count; i++) {
        size_t alen = strlen(a->words[i]);
        for (size_t j = 0; j < b->count; j++) {
            if (alen == strlen(b->words[j]) &&
                memcmp(a->words[i], b->words[j], alen) == 0) {
                inter++;
                break;
            }
        }
    }
    size_t un = a->count + b->count - inter;
    if (un == 0) return 0.0;
    return (double)inter / (double)un;
}

/* Text form for query/entry - combine key + content */
static void entry_to_text(const sc_memory_entry_t *e, const char **out, size_t *len) {
    if (e->key && e->key_len > 0 && e->content && e->content_len > 0) {
        *out = e->content; /* use content as primary for similarity */
        *len = e->content_len;
    } else if (e->content && e->content_len > 0) {
        *out = e->content;
        *len = e->content_len;
    } else if (e->key && e->key_len > 0) {
        *out = e->key;
        *len = e->key_len;
    } else {
        *out = "";
        *len = 0;
    }
}

sc_error_t sc_mmr_rerank(sc_allocator_t *alloc,
    const char *query, size_t query_len,
    sc_memory_entry_t *entries, double *scores, size_t count,
    double lambda) {
    if (!entries || !scores || count == 0) return SC_OK;

    double lam = (lambda >= 0.0 && lambda <= 1.0) ? lambda : SC_MMR_LAMBDA_DEFAULT;

    word_set_t query_ws;
    word_set_from_text(&query_ws, query, query_len, alloc);

    word_set_t *doc_sets = (word_set_t *)alloc->alloc(alloc->ctx,
        count * sizeof(word_set_t));
    if (!doc_sets) {
        word_set_cleanup(&query_ws, alloc);
        return SC_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < count; i++) {
        const char *text;
        size_t len;
        entry_to_text(&entries[i], &text, &len);
        word_set_from_text(&doc_sets[i], text, len, alloc);
    }

    /* MMR: iteratively select doc that maximizes MMR score */
    bool *selected = (bool *)alloc->alloc(alloc->ctx, count * sizeof(bool));
    if (!selected) {
        for (size_t i = 0; i < count; i++) word_set_cleanup(&doc_sets[i], alloc);
        alloc->free(alloc->ctx, doc_sets, count * sizeof(word_set_t));
        word_set_cleanup(&query_ws, alloc);
        return SC_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < count; i++) selected[i] = false;

    sc_memory_entry_t *out_entries = (sc_memory_entry_t *)alloc->alloc(
        alloc->ctx, count * sizeof(sc_memory_entry_t));
    double *out_scores = (double *)alloc->alloc(alloc->ctx,
        count * sizeof(double));
    if (!out_entries || !out_scores) {
        if (out_entries) alloc->free(alloc->ctx, out_entries,
            count * sizeof(sc_memory_entry_t));
        if (out_scores) alloc->free(alloc->ctx, out_scores,
            count * sizeof(double));
        alloc->free(alloc->ctx, selected, count * sizeof(bool));
        for (size_t i = 0; i < count; i++) word_set_cleanup(&doc_sets[i], alloc);
        alloc->free(alloc->ctx, doc_sets, count * sizeof(word_set_t));
        word_set_cleanup(&query_ws, alloc);
        return SC_ERR_OUT_OF_MEMORY;
    }

    for (size_t out_i = 0; out_i < count; out_i++) {
        double best_mmr = -1e99;
        size_t best_idx = 0;
        for (size_t i = 0; i < count; i++) {
            if (selected[i]) continue;
            double sim_qd = jaccard_similarity(&query_ws, &doc_sets[i]);
            double max_sim_dj = 0.0;
            for (size_t j = 0; j < count; j++) {
                if (!selected[j] || j == i) continue;
                double s = jaccard_similarity(&doc_sets[i], &doc_sets[j]);
                if (s > max_sim_dj) max_sim_dj = s;
            }
            double mmr = lam * sim_qd - (1.0 - lam) * max_sim_dj;
            if (mmr > best_mmr) {
                best_mmr = mmr;
                best_idx = i;
            }
        }
        selected[best_idx] = true;
        out_entries[out_i] = entries[best_idx];
        out_scores[out_i] = scores[best_idx];
    }

    memcpy(entries, out_entries, count * sizeof(sc_memory_entry_t));
    memcpy(scores, out_scores, count * sizeof(double));

    alloc->free(alloc->ctx, out_scores, count * sizeof(double));
    alloc->free(alloc->ctx, out_entries, count * sizeof(sc_memory_entry_t));
    alloc->free(alloc->ctx, selected, count * sizeof(bool));
    for (size_t i = 0; i < count; i++) word_set_cleanup(&doc_sets[i], alloc);
    alloc->free(alloc->ctx, doc_sets, count * sizeof(word_set_t));
    word_set_cleanup(&query_ws, alloc);
    return SC_OK;
}
