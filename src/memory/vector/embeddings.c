#include "seaclaw/memory/vector/embeddings.h"
#include "seaclaw/memory/vector/embeddings_gemini.h"
#include "seaclaw/memory/vector/embeddings_ollama.h"
#include "seaclaw/memory/vector/embeddings_voyage.h"
#include "seaclaw/core/string.h"
#include <string.h>

void sc_embedding_provider_free(sc_allocator_t *alloc,
    sc_embedding_provider_result_t *out) {
    if (!alloc || !out) return;
    if (out->values) {
        alloc->free(alloc->ctx, out->values, out->dimensions * sizeof(float));
        out->values = NULL;
        out->dimensions = 0;
    }
}

/* Noop: returns empty embedding */
static sc_error_t noop_embed(void *ctx, sc_allocator_t *alloc,
    const char *text, size_t text_len,
    sc_embedding_provider_result_t *out) {
    (void)ctx;
    (void)text;
    (void)text_len;
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    out->values = (float *)alloc->alloc(alloc->ctx, 0);
    out->dimensions = 0;
    return SC_OK;
}

static const char *noop_name(void *ctx) { (void)ctx; return "none"; }
static size_t noop_dimensions(void *ctx) { (void)ctx; return 0; }
static void noop_deinit(void *ctx, sc_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, 1);
}

static const sc_embedding_provider_vtable_t noop_vtable = {
    .embed = noop_embed,
    .name = noop_name,
    .dimensions = noop_dimensions,
    .deinit = noop_deinit,
};

sc_embedding_provider_t sc_embedding_provider_noop_create(sc_allocator_t *alloc) {
    sc_embedding_provider_t p = { .ctx = NULL, .vtable = &noop_vtable };
    if (!alloc) return p;
    void *ctx = alloc->alloc(alloc->ctx, 1);
    if (!ctx) return p;
    p.ctx = ctx;
    return p;
}

sc_embedding_provider_t sc_embedding_provider_create(sc_allocator_t *alloc,
    const char *provider_name,
    const char *api_key,
    const char *model,
    size_t dims) {
    if (!alloc || !provider_name) return sc_embedding_provider_noop_create(alloc);

    const char *key = api_key ? api_key : "";
    const char *mod = model ? model : "";

    if (strcmp(provider_name, "gemini") == 0) {
        return sc_embedding_gemini_create(alloc, key, mod, dims);
    }
    if (strcmp(provider_name, "ollama") == 0) {
        return sc_embedding_ollama_create(alloc, mod, dims);
    }
    if (strcmp(provider_name, "voyage") == 0) {
        return sc_embedding_voyage_create(alloc, key, mod, dims);
    }

    return sc_embedding_provider_noop_create(alloc);
}
