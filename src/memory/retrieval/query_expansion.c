#include "seaclaw/memory/retrieval/query_expansion.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

/* Minimal stop words (English) */
static const char *const stop_words[] = {
    "the", "a", "an", "is", "are", "was", "were", "be", "been", "being",
    "have", "has", "had", "do", "does", "did", "will", "would", "could",
    "should", "may", "might", "must", "shall", "can", "need", "dare",
    "what", "which", "who", "whom", "this", "that", "these", "those",
    "and", "but", "or", "nor", "for", "yet", "so", "both", "each",
    "of", "in", "to", "on", "at", "by", "with", "from", "as", "into",
};
static const size_t n_stop = sizeof(stop_words) / sizeof(stop_words[0]);

static int is_stop_word(const char *s, size_t len) {
    for (size_t i = 0; i < n_stop; i++) {
        size_t wlen = strlen(stop_words[i]);
        if (wlen == len && strncasecmp(s, stop_words[i], len) == 0)
            return 1;
    }
    return 0;
}

sc_error_t sc_query_expand(sc_allocator_t *alloc, const char *raw_query, size_t raw_len,
    sc_expanded_query_t *out) {
    memset(out, 0, sizeof(*out));
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;

    /* Trim */
    while (raw_len > 0 && (raw_query[0] == ' ' || raw_query[0] == '\t' || raw_query[0] == '\n' || raw_query[0] == '\r')) {
        raw_query++;
        raw_len--;
    }
    while (raw_len > 0 && (raw_query[raw_len - 1] == ' ' || raw_query[raw_len - 1] == '\t' ||
            raw_query[raw_len - 1] == '\n' || raw_query[raw_len - 1] == '\r'))
        raw_len--;

    if (raw_len == 0) {
        out->fts5_query = sc_strndup(alloc, "", 0);
        return SC_OK;
    }

    /* Simple tokenize: split on whitespace, filter stop words */
    size_t cap = 32;
    char **orig = (char **)alloc->alloc(alloc->ctx, cap * sizeof(char *));
    char **filt = (char **)alloc->alloc(alloc->ctx, cap * sizeof(char *));
    if (!orig || !filt) {
        if (orig) alloc->free(alloc->ctx, orig, cap * sizeof(char *));
        if (filt) alloc->free(alloc->ctx, filt, cap * sizeof(char *));
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t no = 0, nf = 0;

    const char *p = raw_query;
    const char *end = raw_query + raw_len;
    while (p < end && no < cap) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (p >= end) break;
        const char *start = p;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        size_t tok_len = (size_t)(p - start);
        if (tok_len == 0) continue;

        char *tok = sc_strndup(alloc, start, tok_len);
        if (!tok) break;
        orig[no++] = tok;

        if (!is_stop_word(start, tok_len) && tok_len >= 2) {
            filt[nf++] = sc_strndup(alloc, start, tok_len);
        }
    }

    /* Build FTS5 query: space-separated filtered tokens, or original if none */
    char *fts5;
    if (nf > 0) {
        size_t total = 0;
        for (size_t i = 0; i < nf; i++) total += strlen(filt[i]) + 1;
        fts5 = (char *)alloc->alloc(alloc->ctx, total + 1);
        if (!fts5) {
            for (size_t i = 0; i < no; i++) alloc->free(alloc->ctx, orig[i], strlen(orig[i]) + 1);
            for (size_t i = 0; i < nf; i++) alloc->free(alloc->ctx, filt[i], strlen(filt[i]) + 1);
            alloc->free(alloc->ctx, orig, cap * sizeof(char *));
            alloc->free(alloc->ctx, filt, cap * sizeof(char *));
            return SC_ERR_OUT_OF_MEMORY;
        }
        char *q = fts5;
        for (size_t i = 0; i < nf; i++) {
            size_t len = strlen(filt[i]);
            memcpy(q, filt[i], len + 1);
            q += len;
            if (i + 1 < nf) *q++ = ' ';
        }
        *q = '\0';
    } else {
        fts5 = sc_strndup(alloc, raw_query, raw_len);
        if (!fts5) {
            for (size_t i = 0; i < no; i++) alloc->free(alloc->ctx, orig[i], strlen(orig[i]) + 1);
            alloc->free(alloc->ctx, orig, cap * sizeof(char *));
            alloc->free(alloc->ctx, filt, cap * sizeof(char *));
            return SC_ERR_OUT_OF_MEMORY;
        }
    }

    out->fts5_query = fts5;
    out->original_tokens = orig;
    out->original_count = no;
    out->filtered_tokens = filt;
    out->filtered_count = nf;
    return SC_OK;
}

void sc_expanded_query_free(sc_allocator_t *alloc, sc_expanded_query_t *eq) {
    if (!alloc || !eq) return;
    if (eq->fts5_query) alloc->free(alloc->ctx, eq->fts5_query, strlen(eq->fts5_query) + 1);
    for (size_t i = 0; i < eq->original_count; i++)
        if (eq->original_tokens[i])
            alloc->free(alloc->ctx, eq->original_tokens[i], strlen(eq->original_tokens[i]) + 1);
    if (eq->original_tokens) alloc->free(alloc->ctx, eq->original_tokens, eq->original_count * sizeof(char *));
    for (size_t i = 0; i < eq->filtered_count; i++)
        if (eq->filtered_tokens[i])
            alloc->free(alloc->ctx, eq->filtered_tokens[i], strlen(eq->filtered_tokens[i]) + 1);
    if (eq->filtered_tokens) alloc->free(alloc->ctx, eq->filtered_tokens, eq->filtered_count * sizeof(char *));
    memset(eq, 0, sizeof(*eq));
}
