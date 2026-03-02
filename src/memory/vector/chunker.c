#include "seaclaw/memory/vector.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdlib.h>
#include <string.h>

static int is_sentence_end(char c) {
    return c == '.' || c == '!' || c == '?';
}

static int is_space(char c) {
    return c == ' ' || c == '\t';
}

/* Find next split point: sentence end (.!? + space), paragraph (\n\n), or word boundary. */
static const char *find_split_point(const char *p, const char *end,
    size_t max_from_start, size_t min_chunk) {
    const char *best = NULL;
    size_t best_dist = 0;

    for (const char *s = p; s < end && (size_t)(s - p) < max_from_start; s++) {
        if (s + 1 < end && *s == '\n' && *(s + 1) == '\n') {
            size_t d = (size_t)(s - p);
            if (d >= min_chunk) return s + 2;
            best = s + 2;
            best_dist = d;
        }
        if (is_sentence_end(*s) && s + 1 < end && is_space(*(s + 1))) {
            size_t d = (size_t)(s - p) + 1;
            if (d >= min_chunk) return s + 1;
            if (d > best_dist) { best = s + 1; best_dist = d; }
        }
        if (*s == ' ' && (size_t)(s - p) >= min_chunk)
            if (!best || (size_t)(s - p) > best_dist) { best = s + 1; best_dist = (size_t)(s - p); }
    }
    return best ? best : end;
}

sc_error_t sc_chunker_split(sc_allocator_t *alloc,
    const char *text, size_t text_len,
    const sc_chunker_options_t *opts,
    sc_text_chunk_t **out, size_t *out_count) {
    if (!alloc || !opts || !out || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    if (!text && text_len > 0)
        return SC_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_count = 0;

    if (!text || text_len == 0)
        return SC_OK;

    size_t max_chunk = opts->max_chunk_size > 0 ? opts->max_chunk_size : 512;
    size_t overlap = opts->overlap;

    /* Build chunks dynamically (simple: over-allocate, trim later) */
    sc_text_chunk_t *chunks = (sc_text_chunk_t *)alloc->alloc(alloc->ctx,
        sizeof(sc_text_chunk_t) * ((text_len / (max_chunk / 2)) + 2));
    if (!chunks)
        return SC_ERR_OUT_OF_MEMORY;

    size_t count = 0;
    const char *p = text;
    const char *end = text + text_len;

    while (p < end) {
        size_t remaining = (size_t)(end - p);
        if (remaining <= max_chunk) {
            chunks[count].text = p;
            chunks[count].text_len = remaining;
            chunks[count].offset = (size_t)(p - text);
            count++;
            break;
        }

        const char *split = find_split_point(p, end, max_chunk, max_chunk / 4);
        size_t chunk_len = (size_t)(split - p);

        chunks[count].text = p;
        chunks[count].text_len = chunk_len;
        chunks[count].offset = (size_t)(p - text);
        count++;

        if (split >= end) break;

        /* Advance with overlap */
        if (overlap > 0 && chunk_len > overlap) {
            p = split - overlap;
            if (p < text) p = text;
        } else {
            p = split;
        }
    }

    if (count == 0) {
        alloc->free(alloc->ctx, chunks, sizeof(sc_text_chunk_t) * ((text_len / (max_chunk / 2)) + 2));
        return SC_OK;
    }

    /* Trim to exact size */
    sc_text_chunk_t *trimmed = (sc_text_chunk_t *)alloc->alloc(alloc->ctx,
        sizeof(sc_text_chunk_t) * count);
    if (!trimmed) {
        alloc->free(alloc->ctx, chunks, sizeof(sc_text_chunk_t) * ((text_len / (max_chunk / 2)) + 2));
        return SC_ERR_OUT_OF_MEMORY;
    }
    memcpy(trimmed, chunks, sizeof(sc_text_chunk_t) * count);
    alloc->free(alloc->ctx, chunks, sizeof(sc_text_chunk_t) * ((text_len / (max_chunk / 2)) + 2));

    *out = trimmed;
    *out_count = count;
    return SC_OK;
}

void sc_chunker_free(sc_allocator_t *alloc, sc_text_chunk_t *chunks, size_t count) {
    if (alloc && chunks && count > 0)
        alloc->free(alloc->ctx, chunks, sizeof(sc_text_chunk_t) * count);
}
