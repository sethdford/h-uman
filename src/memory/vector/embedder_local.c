#include "seaclaw/memory/vector.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define SC_EMBED_DIM SC_EMBEDDING_DIM

typedef struct local_embedder_ctx {
    sc_allocator_t *alloc;
    /* Vocabulary not used in simplified TF; we use hash projection */
} local_embedder_ctx_t;

static uint32_t hash_str(const char *s, size_t len) {
    uint32_t h = 5381u;
    for (size_t i = 0; i < len; i++)
        h = ((h << 5) + h) + (unsigned char)s[i]; /* h * 33 + c */
    return h;
}

#if SC_IS_TEST
/* Simple LCG for deterministic "random" in test mode */
static float next_rand(uint32_t *state) {
    *state = *state * 1103515245u + 12345u;
    return (float)((*state >> 16) & 0x7FFFu) / 32767.0f;
}
#endif

#if SC_IS_TEST
/* In test mode: deterministic pseudo-random 384-dim vector from text hash */
static sc_error_t embed_test_impl(void *ctx, sc_allocator_t *alloc,
    const char *text, size_t text_len,
    sc_embedding_t *out) {
    (void)ctx;
    if (!alloc || !text || !out)
        return SC_ERR_INVALID_ARGUMENT;

    uint32_t seed = hash_str(text, text_len);
    if (seed == 0) seed = 1;

    out->values = (float *)alloc->alloc(alloc->ctx, SC_EMBED_DIM * sizeof(float));
    if (!out->values)
        return SC_ERR_OUT_OF_MEMORY;
    out->dim = SC_EMBED_DIM;

    for (size_t i = 0; i < SC_EMBED_DIM; i++)
        out->values[i] = next_rand(&seed);

    /* L2-normalize */
    double norm = 0.0;
    for (size_t i = 0; i < SC_EMBED_DIM; i++)
        norm += (double)out->values[i] * (double)out->values[i];
    norm = sqrt(norm);
    if (norm > 1e-20) {
        for (size_t i = 0; i < SC_EMBED_DIM; i++)
            out->values[i] = (float)((double)out->values[i] / norm);
    }
    return SC_OK;
}
#else
static int is_word_char(unsigned char c) {
    return isalnum(c) || c == '_';
}

/* Non-test: hash-projected bag-of-words "embedding" */
static sc_error_t embed_impl(void *ctx, sc_allocator_t *alloc,
    const char *text, size_t text_len,
    sc_embedding_t *out) {
    (void)ctx;
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;

    out->values = (float *)alloc->alloc(alloc->ctx, SC_EMBED_DIM * sizeof(float));
    if (!out->values)
        return SC_ERR_OUT_OF_MEMORY;
    out->dim = SC_EMBED_DIM;
    memset(out->values, 0, SC_EMBED_DIM * sizeof(float));

    if (!text || text_len == 0)
        return SC_OK;

    const char *p = text;
    const char *end = text + text_len;

    while (p < end) {
        while (p < end && !is_word_char((unsigned char)*p))
            p++;
        if (p >= end) break;

        const char *start = p;
        while (p < end && is_word_char((unsigned char)*p))
            p++;

        size_t word_len = (size_t)(p - start);
        uint32_t h = hash_str(start, word_len);
        size_t idx = (size_t)(h % (uint32_t)SC_EMBED_DIM);
        out->values[idx] += 1.0f;
    }

    /* L2-normalize */
    double norm = 0.0;
    for (size_t i = 0; i < SC_EMBED_DIM; i++)
        norm += (double)out->values[i] * (double)out->values[i];
    norm = sqrt(norm);
    if (norm > 1e-20) {
        for (size_t i = 0; i < SC_EMBED_DIM; i++)
            out->values[i] = (float)((double)out->values[i] / norm);
    }
    return SC_OK;
}
#endif

static sc_error_t embed_wrapper(void *ctx, sc_allocator_t *alloc,
    const char *text, size_t text_len,
    sc_embedding_t *out) {
#if SC_IS_TEST
    return embed_test_impl(ctx, alloc, text, text_len, out);
#else
    return embed_impl(ctx, alloc, text, text_len, out);
#endif
}

static sc_error_t embed_batch_impl(void *ctx, sc_allocator_t *alloc,
    const char **texts, const size_t *text_lens, size_t count,
    sc_embedding_t *out) {
    for (size_t i = 0; i < count; i++) {
        sc_error_t err = embed_wrapper(ctx, alloc,
            texts[i], text_lens[i],
            &out[i]);
        if (err != SC_OK) {
            for (size_t j = 0; j < i; j++)
                sc_embedding_free(alloc, &out[j]);
            return err;
        }
    }
    return SC_OK;
}

static size_t dimensions_impl(void *ctx) {
    (void)ctx;
    return SC_EMBED_DIM;
}

static void deinit_impl(void *ctx, sc_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, sizeof(local_embedder_ctx_t));
}

static const sc_embedder_vtable_t local_vtable = {
    .embed = embed_wrapper,
    .embed_batch = embed_batch_impl,
    .dimensions = dimensions_impl,
    .deinit = deinit_impl,
};

sc_embedder_t sc_embedder_local_create(sc_allocator_t *alloc) {
    sc_embedder_t emb = { .ctx = NULL, .vtable = &local_vtable };
    if (!alloc) return emb;

    local_embedder_ctx_t *ctx = (local_embedder_ctx_t *)alloc->alloc(alloc->ctx,
        sizeof(local_embedder_ctx_t));
    if (!ctx) return emb;
    ctx->alloc = alloc;

    emb.ctx = ctx;
    return emb;
}
