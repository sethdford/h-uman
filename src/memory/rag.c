#include "human/rag.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define RAG_INITIAL_CAP 16

static const char *strstr_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0])
        return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && (char)tolower((unsigned char)*h) == (char)tolower((unsigned char)*n)) {
            h++;
            n++;
        }
        if (!*n)
            return (const char *)haystack;
    }
    return NULL;
}

static int board_matches(const char *chunk_board, const char *const *boards, size_t board_count) {
    if (!chunk_board || board_count == 0)
        return 0;
    for (size_t i = 0; i < board_count; i++) {
        if (boards[i] && strcmp(chunk_board, boards[i]) == 0)
            return 1;
    }
    return 0;
}

/* Tokenize query into terms (>2 chars, lowercase). Caller frees out_terms and each term. */
static hu_error_t tokenize_query(hu_allocator_t *alloc, const char *query, size_t query_len,
                                 char ***out_terms, size_t *out_count) {
    *out_terms = NULL;
    *out_count = 0;
    if (!query || query_len == 0)
        return HU_OK;

    size_t cap = 16;
    char **terms = (char **)alloc->alloc(alloc->ctx, cap * sizeof(char *));
    if (!terms)
        return HU_ERR_OUT_OF_MEMORY;
    size_t count = 0;

    size_t i = 0;
    while (i < query_len) {
        while (i < query_len &&
               (isspace((unsigned char)query[i]) || ispunct((unsigned char)query[i])))
            i++;
        if (i >= query_len)
            break;
        size_t start = i;
        while (i < query_len && !isspace((unsigned char)query[i]) &&
               !ispunct((unsigned char)query[i]))
            i++;
        size_t len = i - start;
        if (len <= 2)
            continue;

        char *term = (char *)alloc->alloc(alloc->ctx, len + 1);
        if (!term) {
            for (size_t j = 0; j < count; j++)
                alloc->free(alloc->ctx, terms[j], strlen(terms[j]) + 1);
            alloc->free(alloc->ctx, terms, cap * sizeof(char *));
            return HU_ERR_OUT_OF_MEMORY;
        }
        for (size_t k = 0; k < len; k++)
            term[k] = (char)tolower((unsigned char)query[start + k]);
        term[len] = '\0';

        if (count >= cap) {
            size_t new_cap = cap * 2;
            char **new_terms = (char **)alloc->realloc(alloc->ctx, terms, cap * sizeof(char *),
                                                       new_cap * sizeof(char *));
            if (!new_terms) {
                for (size_t j = 0; j < count; j++)
                    alloc->free(alloc->ctx, terms[j], strlen(terms[j]) + 1);
                alloc->free(alloc->ctx, terms, cap * sizeof(char *));
                return HU_ERR_OUT_OF_MEMORY;
            }
            terms = new_terms;
            cap = new_cap;
        }
        terms[count++] = term;
    }

    /* Shrink to exact size so caller can free with count * sizeof(char *) */
    if (count == 0) {
        alloc->free(alloc->ctx, terms, cap * sizeof(char *));
        terms = NULL;
    } else if (count < cap) {
        char **shrunk = (char **)alloc->realloc(alloc->ctx, terms, cap * sizeof(char *),
                                                count * sizeof(char *));
        if (shrunk)
            terms = shrunk;
    }
    *out_terms = terms;
    *out_count = count;
    return HU_OK;
}

typedef struct {
    const hu_datasheet_chunk_t *chunk;
    int score;
} scored_chunk_t;

static int scored_cmp(const void *a, const void *b) {
    int sa = ((const scored_chunk_t *)a)->score;
    int sb = ((const scored_chunk_t *)b)->score;
    return (sa > sb) ? -1 : (sa < sb) ? 1 : 0;
}

hu_error_t hu_rag_init(hu_rag_t *rag, hu_allocator_t *alloc) {
    if (!rag || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    memset(rag, 0, sizeof(*rag));
    rag->alloc = alloc;
    return HU_OK;
}

void hu_rag_free(hu_rag_t *rag) {
    if (!rag)
        return;
    for (size_t i = 0; i < rag->chunk_count; i++) {
        hu_datasheet_chunk_t *c = &rag->chunks[i];
        if (c->board)
            rag->alloc->free(rag->alloc->ctx, (void *)c->board, strlen(c->board) + 1);
        if (c->source)
            rag->alloc->free(rag->alloc->ctx, (void *)c->source, strlen(c->source) + 1);
        if (c->content)
            rag->alloc->free(rag->alloc->ctx, (void *)c->content, strlen(c->content) + 1);
    }
    if (rag->chunks)
        rag->alloc->free(rag->alloc->ctx, rag->chunks,
                         rag->chunk_cap * sizeof(hu_datasheet_chunk_t));
    rag->chunks = NULL;
    rag->chunk_count = 0;
    rag->chunk_cap = 0;
}

hu_error_t hu_rag_add_chunk(hu_rag_t *rag, const char *board, const char *source,
                            const char *content) {
    if (!rag || !rag->alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!source || !content)
        return HU_ERR_INVALID_ARGUMENT;

    if (rag->chunk_count >= rag->chunk_cap) {
        size_t new_cap = rag->chunk_cap == 0 ? RAG_INITIAL_CAP : rag->chunk_cap * 2;
        hu_datasheet_chunk_t *new_chunks = (hu_datasheet_chunk_t *)rag->alloc->realloc(
            rag->alloc->ctx, rag->chunks, rag->chunk_cap * sizeof(hu_datasheet_chunk_t),
            new_cap * sizeof(hu_datasheet_chunk_t));
        if (!new_chunks)
            return HU_ERR_OUT_OF_MEMORY;
        rag->chunks = new_chunks;
        rag->chunk_cap = new_cap;
    }

    char *board_dup = board ? hu_strdup(rag->alloc, board) : NULL;
    char *source_dup = hu_strdup(rag->alloc, source);
    char *content_dup = hu_strdup(rag->alloc, content);
    if (!source_dup || !content_dup) {
        if (board_dup)
            rag->alloc->free(rag->alloc->ctx, board_dup, strlen(board_dup) + 1);
        if (source_dup)
            rag->alloc->free(rag->alloc->ctx, source_dup, strlen(source_dup) + 1);
        if (content_dup)
            rag->alloc->free(rag->alloc->ctx, content_dup, strlen(content_dup) + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }

    hu_datasheet_chunk_t *c = &rag->chunks[rag->chunk_count++];
    c->board = board_dup;
    c->source = source_dup;
    c->content = content_dup;
    return HU_OK;
}

hu_error_t hu_rag_retrieve(hu_rag_t *rag, hu_allocator_t *alloc, const char *query,
                           size_t query_len, const char *const *boards, size_t board_count,
                           size_t limit, const hu_datasheet_chunk_t ***out_results,
                           size_t *out_count) {
    *out_results = NULL;
    *out_count = 0;
    if (!rag || !alloc || !out_results || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    if (rag->chunk_count == 0 || limit == 0)
        return HU_OK;

    char **terms = NULL;
    size_t term_count = 0;
    hu_error_t err = tokenize_query(alloc, query, query_len, &terms, &term_count);
    if (err != HU_OK)
        return err;

    scored_chunk_t *scored =
        (scored_chunk_t *)alloc->alloc(alloc->ctx, rag->chunk_count * sizeof(scored_chunk_t));
    if (!scored) {
        for (size_t i = 0; i < term_count; i++)
            alloc->free(alloc->ctx, terms[i], strlen(terms[i]) + 1);
        if (terms)
            alloc->free(alloc->ctx, terms, term_count * sizeof(char *));
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t scored_count = 0;

    for (size_t i = 0; i < rag->chunk_count; i++) {
        const hu_datasheet_chunk_t *c = &rag->chunks[i];
        int score = 0;
        const char *cont = c->content;
        if (!cont)
            continue;

        for (size_t t = 0; t < term_count; t++) {
            if (strstr_ci(cont, terms[t]) != NULL)
                score += 1;
        }
        if (score > 0) {
            if (board_matches(c->board, boards, board_count))
                score += 2;
            scored[scored_count].chunk = c;
            scored[scored_count].score = score;
            scored_count++;
        }
    }

    /* Free tokenize output */
    for (size_t i = 0; i < term_count; i++)
        alloc->free(alloc->ctx, terms[i], strlen(terms[i]) + 1);
    if (terms)
        alloc->free(alloc->ctx, terms, term_count * sizeof(char *));

    if (scored_count == 0) {
        alloc->free(alloc->ctx, scored, rag->chunk_count * sizeof(scored_chunk_t));
        return HU_OK;
    }

    qsort(scored, scored_count, sizeof(scored_chunk_t), scored_cmp);

    size_t result_count = limit < scored_count ? limit : scored_count;
    const hu_datasheet_chunk_t **results = (const hu_datasheet_chunk_t **)alloc->alloc(
        alloc->ctx, result_count * sizeof(const hu_datasheet_chunk_t *));
    if (!results) {
        alloc->free(alloc->ctx, scored, rag->chunk_count * sizeof(scored_chunk_t));
        return HU_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < result_count; i++)
        results[i] = scored[i].chunk;

    alloc->free(alloc->ctx, scored, rag->chunk_count * sizeof(scored_chunk_t));
    *out_results = results;
    *out_count = result_count;
    return HU_OK;
}

hu_error_t hu_rag_query(hu_allocator_t *alloc, const char *query, size_t query_len,
                        char **out_response, size_t *out_len) {
    if (!alloc || !out_response || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_response = NULL;
    *out_len = 0;

    hu_rag_t rag;
    hu_error_t err = hu_rag_init(&rag, alloc);
    if (err != HU_OK)
        return err;

    const hu_datasheet_chunk_t **results = NULL;
    size_t count = 0;
    err = hu_rag_retrieve(&rag, alloc, query, query_len, NULL, 0, 3, &results, &count);
    hu_rag_free(&rag);
    if (err != HU_OK)
        return err;
    if (count == 0) {
        char *empty = hu_strndup(alloc, "", 0);
        if (empty) {
            *out_response = empty;
            *out_len = 0;
        }
        return HU_OK;
    }

    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (results[i]->source)
            total += strlen(results[i]->source) + 2;
        if (results[i]->content)
            total += strlen(results[i]->content) + 2;
    }
    char *buf = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!buf) {
        alloc->free(alloc->ctx, (void *)results, count * sizeof(const hu_datasheet_chunk_t *));
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        if (results[i]->source) {
            size_t slen = strlen(results[i]->source);
            memcpy(buf + pos, results[i]->source, slen + 1);
            pos += slen;
            buf[pos++] = ':';
            buf[pos++] = ' ';
        }
        if (results[i]->content) {
            size_t clen = strlen(results[i]->content);
            memcpy(buf + pos, results[i]->content, clen + 1);
            pos += clen;
            buf[pos++] = '\n';
        }
    }
    buf[pos] = '\0';

    alloc->free(alloc->ctx, (void *)results, count * sizeof(const hu_datasheet_chunk_t *));
    *out_response = buf;
    *out_len = pos;
    return HU_OK;
}
