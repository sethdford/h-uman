#include "seaclaw/memory/retrieval.h"
#include "seaclaw/memory.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Case-insensitive character compare */
static int char_tolower_cmp(char a, char b) {
    return (int)(unsigned char)tolower((unsigned char)a) -
           (int)(unsigned char)tolower((unsigned char)b);
}

/* Count words in string (space-separated) */
static size_t count_query_words(const char *query, size_t query_len,
    char **words_out, size_t max_words, sc_allocator_t *alloc) {
    size_t count = 0;
    const char *p = query;
    const char *end = query + query_len;
    while (p < end && count < max_words) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
        if (p >= end) break;
        const char *start = p;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
            p++;
        if (p > start) {
            size_t len = (size_t)(p - start);
            char *w = (char *)alloc->alloc(alloc->ctx, len + 1);
            if (!w) break;
            memcpy(w, start, len);
            w[len] = '\0';
            for (size_t i = 0; i < len; i++)
                w[i] = (char)(unsigned char)tolower((unsigned char)w[i]);
            words_out[count++] = w;
        }
    }
    return count;
}

/* Check if word appears in text (case-insensitive) */
static bool word_in_text(const char *word, size_t word_len,
    const char *text, size_t text_len) {
    if (!word || word_len == 0 || !text) return false;
    for (size_t i = 0; i + word_len <= text_len; i++) {
        if (char_tolower_cmp(text[i], word[0]) != 0) continue;
        bool match = true;
        for (size_t j = 1; j < word_len && match; j++)
            match = (char_tolower_cmp(text[i + j], word[j]) == 0);
        if (match) {
            /* Word boundary: preceded and followed by non-alnum */
            bool start_ok = (i == 0) || !isalnum((unsigned char)text[i - 1]);
            bool end_ok = (i + word_len >= text_len) ||
                !isalnum((unsigned char)text[i + word_len]);
            if (start_ok && end_ok) return true;
        }
    }
    return false;
}

/* Score entry: matches / total_query_words. Search in key+content. */
static double score_entry_keyword(const sc_memory_entry_t *e,
    char **query_words, size_t query_word_count) {
    if (query_word_count == 0) return 0.0;
    size_t matches = 0;
    for (size_t w = 0; w < query_word_count; w++) {
        size_t wlen = strlen(query_words[w]);
        if (e->key && e->key_len >= wlen &&
            word_in_text(query_words[w], wlen, e->key, e->key_len)) {
            matches++;
            continue;
        }
        if (e->content && e->content_len >= wlen &&
            word_in_text(query_words[w], wlen, e->content, e->content_len)) {
            matches++;
        }
    }
    return (double)matches / (double)query_word_count;
}

sc_error_t sc_keyword_retrieve(sc_allocator_t *alloc, sc_memory_t *backend,
    const char *query, size_t query_len,
    const sc_retrieval_options_t *opts,
    sc_retrieval_result_t *out) {
    out->entries = NULL;
    out->count = 0;
    out->scores = NULL;

    if (!backend || !backend->ctx || !backend->vtable || !query || query_len == 0)
        return SC_OK;

    sc_memory_entry_t *all = NULL;
    size_t all_count = 0;
    sc_error_t err = backend->vtable->list(backend->ctx, alloc, NULL, NULL, 0,
        &all, &all_count);
    if (err != SC_OK || !all || all_count == 0) return err;

    /* Split query into words */
    char *words[64];
    size_t word_count = count_query_words(query, query_len, words, 64, alloc);
    if (word_count == 0) {
        for (size_t i = 0; i < all_count; i++) {
            sc_memory_entry_free_fields(alloc, &all[i]);
        }
        alloc->free(alloc->ctx, all, all_count * sizeof(sc_memory_entry_t));
        return SC_OK;
    }

    /* Score each entry, collect those above min_score */
    size_t limit = opts && opts->limit > 0 ? opts->limit : 10;
    double min_score = opts && opts->min_score >= 0.0 ? opts->min_score : 0.0;

    /* Simple approach: score all, then sort and take top limit */
    double *scores = (double *)alloc->alloc(alloc->ctx,
        all_count * sizeof(double));
    if (!scores) {
        for (size_t w = 0; w < word_count; w++)
            alloc->free(alloc->ctx, words[w], strlen(words[w]) + 1);
        for (size_t i = 0; i < all_count; i++)
            sc_memory_entry_free_fields(alloc, &all[i]);
        alloc->free(alloc->ctx, all, all_count * sizeof(sc_memory_entry_t));
        return SC_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < all_count; i++) {
        scores[i] = score_entry_keyword(&all[i], words, word_count);
    }

    /* Free query words */
    for (size_t w = 0; w < word_count; w++)
        alloc->free(alloc->ctx, words[w], strlen(words[w]) + 1);

    /* Filter by min_score, sort by score descending, take limit */
    /* Build result: indices of entries that pass, sorted by score */
    size_t *idx = (size_t *)alloc->alloc(alloc->ctx, all_count * sizeof(size_t));
    if (!idx) {
        alloc->free(alloc->ctx, scores, all_count * sizeof(double));
        for (size_t i = 0; i < all_count; i++)
            sc_memory_entry_free_fields(alloc, &all[i]);
        alloc->free(alloc->ctx, all, all_count * sizeof(sc_memory_entry_t));
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t pass_count = 0;
    for (size_t i = 0; i < all_count; i++) {
        if (scores[i] > 0.0 && scores[i] >= min_score) idx[pass_count++] = i;
    }
    /* Sort by score descending (bubble sort for simplicity) */
    for (size_t i = 0; i < pass_count; i++) {
        for (size_t j = i + 1; j < pass_count; j++) {
            if (scores[idx[j]] > scores[idx[i]]) {
                size_t tmp = idx[i];
                idx[i] = idx[j];
                idx[j] = tmp;
            }
        }
    }
    if (pass_count > limit) pass_count = limit;

    if (pass_count == 0) {
        alloc->free(alloc->ctx, idx, all_count * sizeof(size_t));
        alloc->free(alloc->ctx, scores, all_count * sizeof(double));
        for (size_t i = 0; i < all_count; i++)
            sc_memory_entry_free_fields(alloc, &all[i]);
        alloc->free(alloc->ctx, all, all_count * sizeof(sc_memory_entry_t));
        return SC_OK;
    }

    sc_memory_entry_t *result_entries = (sc_memory_entry_t *)alloc->alloc(
        alloc->ctx, pass_count * sizeof(sc_memory_entry_t));
    double *result_scores = (double *)alloc->alloc(alloc->ctx,
        pass_count * sizeof(double));
    if (!result_entries || !result_scores) {
        if (result_entries) alloc->free(alloc->ctx, result_entries,
            pass_count * sizeof(sc_memory_entry_t));
        if (result_scores) alloc->free(alloc->ctx, result_scores,
            pass_count * sizeof(double));
        alloc->free(alloc->ctx, idx, all_count * sizeof(size_t));
        alloc->free(alloc->ctx, scores, all_count * sizeof(double));
        for (size_t i = 0; i < all_count; i++)
            sc_memory_entry_free_fields(alloc, &all[i]);
        alloc->free(alloc->ctx, all, all_count * sizeof(sc_memory_entry_t));
        return SC_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < pass_count; i++) {
        result_entries[i] = all[idx[i]];
        result_scores[i] = scores[idx[i]];
    }
    /* Free entries we didn't keep */
    for (size_t i = 0; i < all_count; i++) {
        bool kept = false;
        for (size_t k = 0; k < pass_count && !kept; k++)
            kept = (idx[k] == i);
        if (!kept) sc_memory_entry_free_fields(alloc, &all[i]);
    }
    alloc->free(alloc->ctx, all, all_count * sizeof(sc_memory_entry_t));
    alloc->free(alloc->ctx, scores, all_count * sizeof(double));
    alloc->free(alloc->ctx, idx, all_count * sizeof(size_t));

    out->entries = result_entries;
    out->count = pass_count;
    out->scores = result_scores;
    return SC_OK;
}
