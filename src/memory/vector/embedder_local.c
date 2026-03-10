#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/vector.h"
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HU_EMBED_DIM HU_EMBEDDING_DIM

typedef struct local_embedder_ctx {
    hu_allocator_t *alloc;
    /* Vocabulary not used in simplified TF; we use hash projection */
} local_embedder_ctx_t;

static uint32_t hash_str(const char *s, size_t len) {
    uint32_t h = 5381u;
    for (size_t i = 0; i < len; i++)
        h = ((h << 5) + h) + (unsigned char)s[i]; /* h * 33 + c */
    return h;
}

#if HU_IS_TEST
/* Simple LCG for deterministic "random" in test mode */
static float next_rand(uint32_t *state) {
    *state = *state * 1103515245u + 12345u;
    return (float)((*state >> 16) & 0x7FFFu) / 32767.0f;
}
#endif

#if HU_IS_TEST
/* In test mode: deterministic pseudo-random 384-dim vector from text hash */
static hu_error_t embed_test_impl(void *ctx, hu_allocator_t *alloc, const char *text,
                                  size_t text_len, hu_embedding_t *out) {
    (void)ctx;
    if (!alloc || !text || !out)
        return HU_ERR_INVALID_ARGUMENT;

    uint32_t seed = hash_str(text, text_len);
    if (seed == 0)
        seed = 1;

    out->values = (float *)alloc->alloc(alloc->ctx, HU_EMBED_DIM * sizeof(float));
    if (!out->values)
        return HU_ERR_OUT_OF_MEMORY;
    out->dim = HU_EMBED_DIM;

    for (size_t i = 0; i < HU_EMBED_DIM; i++)
        out->values[i] = next_rand(&seed);

    /* L2-normalize */
    double norm = 0.0;
    for (size_t i = 0; i < HU_EMBED_DIM; i++)
        norm += (double)out->values[i] * (double)out->values[i];
    norm = sqrt(norm);
    if (norm > 1e-20) {
        for (size_t i = 0; i < HU_EMBED_DIM; i++)
            out->values[i] = (float)((double)out->values[i] / norm);
    }
    return HU_OK;
}
#else
static int is_word_char(unsigned char c) {
    return isalnum(c) || c == '_';
}

/* Non-test: hash-projected bag-of-words "embedding" */
static hu_error_t embed_impl(void *ctx, hu_allocator_t *alloc, const char *text, size_t text_len,
                             hu_embedding_t *out) {
    (void)ctx;
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    out->values = (float *)alloc->alloc(alloc->ctx, HU_EMBED_DIM * sizeof(float));
    if (!out->values)
        return HU_ERR_OUT_OF_MEMORY;
    out->dim = HU_EMBED_DIM;
    memset(out->values, 0, HU_EMBED_DIM * sizeof(float));

    if (!text || text_len == 0)
        return HU_OK;

    const char *p = text;
    const char *end = text + text_len;

    while (p < end) {
        while (p < end && !is_word_char((unsigned char)*p))
            p++;
        if (p >= end)
            break;

        const char *start = p;
        while (p < end && is_word_char((unsigned char)*p))
            p++;

        size_t word_len = (size_t)(p - start);
        uint32_t h = hash_str(start, word_len);
        size_t idx = (size_t)(h % (uint32_t)HU_EMBED_DIM);
        out->values[idx] += 1.0f;
    }

    /* L2-normalize */
    double norm = 0.0;
    for (size_t i = 0; i < HU_EMBED_DIM; i++)
        norm += (double)out->values[i] * (double)out->values[i];
    norm = sqrt(norm);
    if (norm > 1e-20) {
        for (size_t i = 0; i < HU_EMBED_DIM; i++)
            out->values[i] = (float)((double)out->values[i] / norm);
    }
    return HU_OK;
}
#endif

static hu_error_t embed_wrapper(void *ctx, hu_allocator_t *alloc, const char *text, size_t text_len,
                                hu_embedding_t *out) {
#if HU_IS_TEST
    return embed_test_impl(ctx, alloc, text, text_len, out);
#else
    return embed_impl(ctx, alloc, text, text_len, out);
#endif
}

static hu_error_t embed_batch_impl(void *ctx, hu_allocator_t *alloc, const char **texts,
                                   const size_t *text_lens, size_t count, hu_embedding_t *out) {
    for (size_t i = 0; i < count; i++) {
        hu_error_t err = embed_wrapper(ctx, alloc, texts[i], text_lens[i], &out[i]);
        if (err != HU_OK) {
            for (size_t j = 0; j < i; j++)
                hu_embedding_free(alloc, &out[j]);
            return err;
        }
    }
    return HU_OK;
}

static size_t dimensions_impl(void *ctx) {
    (void)ctx;
    return HU_EMBED_DIM;
}

static void deinit_impl(void *ctx, hu_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, sizeof(local_embedder_ctx_t));
}

static const hu_embedder_vtable_t local_vtable = {
    .embed = embed_wrapper,
    .embed_batch = embed_batch_impl,
    .dimensions = dimensions_impl,
    .deinit = deinit_impl,
};

hu_embedder_t hu_embedder_local_create(hu_allocator_t *alloc) {
    hu_embedder_t emb = {.ctx = NULL, .vtable = &local_vtable};
    if (!alloc)
        return emb;

    local_embedder_ctx_t *ctx =
        (local_embedder_ctx_t *)alloc->alloc(alloc->ctx, sizeof(local_embedder_ctx_t));
    if (!ctx)
        return emb;
    ctx->alloc = alloc;

    emb.ctx = ctx;
    return emb;
}
