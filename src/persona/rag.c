/* src/persona/rag.c — RAG-over-own-messages voice grounding. See header. */

#include "human/persona/rag.h"

#include "human/core/json.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define HU_RAG_MAX_WORDS   64
#define HU_RAG_MAX_WORDLEN 24

/* Small stopword set — same spirit as the W7 Python eval. Word-boundary exact. */
static bool is_stopword(const char *w) {
    static const char *const stopwords[] = {
        "the", "and", "for",  "with", "are", "was", "you", "your", "that", "this",
        "but", "not", "have", "has",  "had", "its", "out", "get",  "got",  "can",
        "all", "any", "what", "when", "how", "who", "why", "into", "from", NULL,
    };
    for (size_t i = 0; stopwords[i]; i++)
        if (strcmp(w, stopwords[i]) == 0)
            return true;
    return false;
}

/* Tokenize into a deduped set of lowercased content words (len>2, non-stop).
 * Returns the count (≤ HU_RAG_MAX_WORDS). */
static size_t tokenize_unique(const char *text, char words[HU_RAG_MAX_WORDS][HU_RAG_MAX_WORDLEN]) {
    size_t n = 0;
    const char *p = text;
    while (*p && n < HU_RAG_MAX_WORDS) {
        while (*p && !isalnum((unsigned char)*p))
            p++;
        if (!*p)
            break;
        char buf[HU_RAG_MAX_WORDLEN];
        size_t len = 0;
        while (*p && (isalnum((unsigned char)*p) || *p == '\'') && len < HU_RAG_MAX_WORDLEN - 1) {
            buf[len++] = (char)tolower((unsigned char)*p);
            p++;
        }
        while (*p && (isalnum((unsigned char)*p) || *p == '\'')) /* skip overlong tail */
            p++;
        buf[len] = '\0';
        if (len <= 2 || is_stopword(buf))
            continue;
        bool dup = false;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(words[i], buf) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            memcpy(words[n], buf, len + 1);
            n++;
        }
    }
    return n;
}

double hu_persona_rag_relevance(const char *query, const char *candidate) {
    if (!query || !candidate)
        return 0.0;
    char qw[HU_RAG_MAX_WORDS][HU_RAG_MAX_WORDLEN];
    char cw[HU_RAG_MAX_WORDS][HU_RAG_MAX_WORDLEN];
    size_t qn = tokenize_unique(query, qw);
    size_t cn = tokenize_unique(candidate, cw);
    if (qn == 0 || cn == 0)
        return 0.0;
    size_t inter = 0;
    for (size_t i = 0; i < qn; i++) {
        for (size_t j = 0; j < cn; j++) {
            if (strcmp(qw[i], cw[j]) == 0) {
                inter++;
                break;
            }
        }
    }
    size_t uni = qn + cn - inter;
    return uni ? (double)inter / (double)uni : 0.0;
}

size_t hu_persona_rag_retrieve(const char *query, const char *const *corpus, size_t corpus_n,
                               size_t k, size_t *out_indices, size_t out_cap) {
    if (!query || !corpus || !out_indices || out_cap == 0 || k == 0)
        return 0;
    if (k > out_cap)
        k = out_cap;

    size_t chosen = 0;
    /* Selection: repeatedly take the highest-relevance not-yet-chosen index,
     * with strict-greater comparison so ties keep the lower (earlier) index. */
    for (size_t slot = 0; slot < k; slot++) {
        double best_score = 0.0;
        size_t best_idx = corpus_n; /* sentinel = none */
        for (size_t i = 0; i < corpus_n; i++) {
            bool already = false;
            for (size_t c = 0; c < chosen; c++) {
                if (out_indices[c] == i) {
                    already = true;
                    break;
                }
            }
            if (already || !corpus[i])
                continue;
            double s = hu_persona_rag_relevance(query, corpus[i]);
            if (s > best_score) { /* strict: first index wins ties */
                best_score = s;
                best_idx = i;
            }
        }
        if (best_idx == corpus_n || best_score <= 0.0)
            break; /* no more positively-relevant messages */
        out_indices[chosen++] = best_idx;
    }
    return chosen;
}

size_t hu_persona_rag_build_block(const char *const *examples, size_t n, char *buf, size_t cap) {
    if (!examples || !buf || cap == 0)
        return 0;
    buf[0] = '\0';
    if (n == 0)
        return 0;
    static const char hdr[] = "Here are examples of how I actually text — match this voice:\n";
    size_t off = 0;
    int w = snprintf(buf + off, cap - off, "%s", hdr);
    if (w < 0 || (size_t)w >= cap - off) {
        buf[0] = '\0';
        return 0; /* header alone doesn't fit — emit nothing rather than a bare header */
    }
    off += (size_t)w;
    for (size_t i = 0; i < n; i++) {
        if (!examples[i])
            continue;
        w = snprintf(buf + off, cap - off, "- %s\n", examples[i]);
        if (w < 0 || (size_t)w >= cap - off) {
            buf[off] = '\0'; /* don't half-write an example */
            break;
        }
        off += (size_t)w;
    }
    return off;
}

/* Bounds so a corrupt/huge corpus file can't blow memory or per-turn latency. */
#define HU_RAG_MAX_CORPUS_BYTES (4 * 1024 * 1024) /* 4 MB */
#define HU_RAG_MAX_CORPUS_MSGS  4000

size_t hu_persona_rag_ground_from_file(const char *query, const char *corpus_path, size_t k,
                                       char *out_buf, size_t out_cap, hu_allocator_t *alloc) {
    if (out_buf && out_cap > 0)
        out_buf[0] = '\0';
    if (!query || !*query || !corpus_path || !out_buf || out_cap == 0 || !alloc || k == 0)
        return 0;

    FILE *f = fopen(corpus_path, "rb");
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    long sz = ftell(f);
    if (sz <= 0 || sz > HU_RAG_MAX_CORPUS_BYTES || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    char *data = alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!data) {
        fclose(f);
        return 0;
    }
    size_t rd = fread(data, 1, (size_t)sz, f);
    fclose(f);
    data[rd] = '\0';

    char **corpus = alloc->alloc(alloc->ctx, HU_RAG_MAX_CORPUS_MSGS * sizeof(char *));
    if (!corpus) {
        alloc->free(alloc->ctx, data, (size_t)sz + 1);
        return 0;
    }

    /* Parse JSONL: one {"text": "..."} per line; tolerate plain-text lines. */
    size_t n = 0;
    char *line = data;
    while (line && *line && n < HU_RAG_MAX_CORPUS_MSGS) {
        char *nl = strchr(line, '\n');
        size_t llen = nl ? (size_t)(nl - line) : strlen(line);
        while (llen > 0 && (line[llen - 1] == '\r' || line[llen - 1] == ' '))
            llen--;
        if (llen > 0) {
            const char *msg = NULL;
            hu_json_value_t *jv = NULL;
            if (line[0] == '{' && hu_json_parse(alloc, line, llen, &jv) == HU_OK && jv) {
                msg = hu_json_get_string(jv, "text");
            }
            if (msg && *msg) {
                size_t mlen = strlen(msg);
                char *dup = alloc->alloc(alloc->ctx, mlen + 1);
                if (dup) {
                    memcpy(dup, msg, mlen + 1);
                    corpus[n++] = dup;
                }
            }
            if (jv)
                hu_json_free(alloc, jv);
        }
        line = nl ? nl + 1 : NULL;
    }

    size_t written = 0;
    if (n > 0) {
        size_t kk = k < n ? k : n;
        size_t *idx = alloc->alloc(alloc->ctx, kk * sizeof(size_t));
        if (idx) {
            size_t got =
                hu_persona_rag_retrieve(query, (const char *const *)corpus, n, kk, idx, kk);
            if (got > 0) {
                const char **ex = alloc->alloc(alloc->ctx, got * sizeof(char *));
                if (ex) {
                    for (size_t i = 0; i < got; i++)
                        ex[i] = corpus[idx[i]];
                    written = hu_persona_rag_build_block(ex, got, out_buf, out_cap);
                    alloc->free(alloc->ctx, ex, got * sizeof(char *));
                }
            }
            alloc->free(alloc->ctx, idx, kk * sizeof(size_t));
        }
    }

    for (size_t i = 0; i < n; i++)
        alloc->free(alloc->ctx, corpus[i], strlen(corpus[i]) + 1);
    alloc->free(alloc->ctx, corpus, HU_RAG_MAX_CORPUS_MSGS * sizeof(char *));
    alloc->free(alloc->ctx, data, (size_t)sz + 1);
    return written;
}
