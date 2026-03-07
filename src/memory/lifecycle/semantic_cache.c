#include "seaclaw/memory/lifecycle/semantic_cache.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory/vector_math.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct cache_entry {
    char *key;
    char *response;
    float *embedding;
    size_t embedding_dims;
    char *model;
    unsigned token_count;
    time_t created_at;
} cache_entry_t;

struct sc_semantic_cache {
    sc_allocator_t *alloc;
    cache_entry_t *entries;
    size_t count;
    size_t capacity;
    size_t max_entries;
    int ttl_seconds;
    float similarity_threshold;
    sc_embedding_provider_t embedding_provider;
};

static void free_entry(sc_allocator_t *alloc, cache_entry_t *e) {
    if (e->key)
        alloc->free(alloc->ctx, e->key, strlen(e->key) + 1);
    if (e->response)
        alloc->free(alloc->ctx, e->response, strlen(e->response) + 1);
    if (e->embedding)
        alloc->free(alloc->ctx, e->embedding, e->embedding_dims * sizeof(float));
    if (e->model)
        alloc->free(alloc->ctx, e->model, strlen(e->model) + 1);
    memset(e, 0, sizeof(*e));
}

static bool entry_expired(const cache_entry_t *e, int ttl_seconds) {
    if (ttl_seconds <= 0)
        return false;
    return (time(NULL) - e->created_at) > ttl_seconds;
}

static void evict_expired(sc_semantic_cache_t *cache) {
    if (cache->ttl_seconds <= 0)
        return;
    size_t dst = 0;
    for (size_t src = 0; src < cache->count; src++) {
        if (entry_expired(&cache->entries[src], cache->ttl_seconds)) {
            free_entry(cache->alloc, &cache->entries[src]);
        } else {
            if (dst != src)
                cache->entries[dst] = cache->entries[src];
            dst++;
        }
    }
    cache->count = dst;
}

static void evict_oldest(sc_semantic_cache_t *cache) {
    if (cache->count == 0)
        return;
    size_t oldest = 0;
    for (size_t i = 1; i < cache->count; i++) {
        if (cache->entries[i].created_at < cache->entries[oldest].created_at)
            oldest = i;
    }
    free_entry(cache->alloc, &cache->entries[oldest]);
    if (oldest < cache->count - 1) {
        memmove(&cache->entries[oldest], &cache->entries[oldest + 1],
                (cache->count - 1 - oldest) * sizeof(cache_entry_t));
    }
    cache->count--;
}

sc_semantic_cache_t *sc_semantic_cache_create(sc_allocator_t *alloc, int ttl_minutes,
                                              size_t max_entries, float similarity_threshold,
                                              sc_embedding_provider_t *embedding_provider) {
    if (!alloc)
        return NULL;
    sc_semantic_cache_t *c =
        (sc_semantic_cache_t *)alloc->alloc(alloc->ctx, sizeof(sc_semantic_cache_t));
    if (!c)
        return NULL;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->similarity_threshold = similarity_threshold;
    c->ttl_seconds = ttl_minutes > 0 ? ttl_minutes * 60 : 0;
    c->max_entries = max_entries > 0 ? max_entries : 1024;
    if (embedding_provider)
        c->embedding_provider = *embedding_provider;
    return c;
}

void sc_semantic_cache_destroy(sc_allocator_t *alloc, sc_semantic_cache_t *cache) {
    if (!cache || !alloc)
        return;
    for (size_t i = 0; i < cache->count; i++)
        free_entry(alloc, &cache->entries[i]);
    if (cache->entries)
        alloc->free(alloc->ctx, cache->entries, cache->capacity * sizeof(cache_entry_t));
    alloc->free(alloc->ctx, cache, sizeof(sc_semantic_cache_t));
}

sc_error_t sc_semantic_cache_get(sc_semantic_cache_t *cache, sc_allocator_t *alloc,
                                 const char *key_hex, size_t key_len, const char *query_text,
                                 size_t query_len, sc_semantic_cache_hit_t *out) {
    if (!cache || !alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    evict_expired(cache);

    if (cache->embedding_provider.ctx && cache->embedding_provider.vtable &&
        cache->embedding_provider.vtable->embed && query_text && query_len > 0) {
        sc_embedding_provider_result_t qemb = {0};
        sc_error_t err = cache->embedding_provider.vtable->embed(
            cache->embedding_provider.ctx, alloc, query_text, query_len, &qemb);
        if (err == SC_OK && qemb.values && qemb.dimensions > 0) {
            float best_sim = 0.0f;
            cache_entry_t *best = NULL;

            for (size_t i = 0; i < cache->count; i++) {
                cache_entry_t *e = &cache->entries[i];
                if (!e->embedding || e->embedding_dims != qemb.dimensions)
                    continue;

                float sim = sc_vector_cosine_similarity(qemb.values, e->embedding, qemb.dimensions);
                if (sim > best_sim && sim >= cache->similarity_threshold) {
                    best_sim = sim;
                    best = e;
                }
            }
            sc_embedding_provider_free(alloc, &qemb);

            if (best) {
                out->response = sc_strdup(alloc, best->response);
                out->similarity = best_sim;
                out->semantic = 1;
                return SC_OK;
            }
        }
    }

    for (size_t i = 0; i < cache->count; i++) {
        cache_entry_t *e = &cache->entries[i];
        if (e->key && key_len == strlen(e->key) && memcmp(e->key, key_hex, key_len) == 0) {
            out->response = sc_strdup(alloc, e->response);
            out->similarity = 1.0f;
            out->semantic = 0;
            return SC_OK;
        }
    }

    return SC_ERR_NOT_FOUND;
}

sc_error_t sc_semantic_cache_put(sc_semantic_cache_t *cache, sc_allocator_t *alloc,
                                 const char *key_hex, size_t key_len, const char *model,
                                 size_t model_len, const char *response, size_t response_len,
                                 unsigned token_count, const char *query_text, size_t query_len) {
    if (!cache || !alloc || !key_hex)
        return SC_ERR_INVALID_ARGUMENT;

    evict_expired(cache);

    for (size_t i = 0; i < cache->count; i++) {
        if (cache->entries[i].key && key_len == strlen(cache->entries[i].key) &&
            memcmp(cache->entries[i].key, key_hex, key_len) == 0) {
            free_entry(alloc, &cache->entries[i]);
            memmove(&cache->entries[i], &cache->entries[i + 1],
                    (cache->count - 1 - i) * sizeof(cache_entry_t));
            cache->count--;
            break;
        }
    }

    while (cache->max_entries > 0 && cache->count >= cache->max_entries)
        evict_oldest(cache);

    if (cache->count >= cache->capacity) {
        size_t new_cap = cache->capacity == 0 ? 16 : cache->capacity * 2;
        size_t old_sz = cache->capacity * sizeof(cache_entry_t);
        size_t new_sz = new_cap * sizeof(cache_entry_t);
        cache_entry_t *tmp =
            (cache_entry_t *)alloc->realloc(alloc->ctx, cache->entries, old_sz, new_sz);
        if (!tmp)
            return SC_ERR_OUT_OF_MEMORY;
        cache->entries = tmp;
        cache->capacity = new_cap;
    }

    cache_entry_t *e = &cache->entries[cache->count];
    memset(e, 0, sizeof(*e));
    e->key = sc_strndup(alloc, key_hex, key_len);
    e->response = sc_strndup(alloc, response, response_len);
    e->model = model && model_len > 0 ? sc_strndup(alloc, model, model_len) : NULL;
    e->token_count = token_count;
    e->created_at = time(NULL);

    if (query_text && query_len > 0 && cache->embedding_provider.ctx &&
        cache->embedding_provider.vtable && cache->embedding_provider.vtable->embed) {
        sc_embedding_provider_result_t res = {0};
        if (cache->embedding_provider.vtable->embed(cache->embedding_provider.ctx, alloc,
                                                    query_text, query_len, &res) == SC_OK &&
            res.values) {
            e->embedding = res.values;
            e->embedding_dims = res.dimensions;
        }
    }

    cache->count++;
    return SC_OK;
}

void sc_semantic_cache_hit_free(sc_allocator_t *alloc, sc_semantic_cache_hit_t *hit) {
    if (!alloc || !hit)
        return;
    if (hit->response)
        alloc->free(alloc->ctx, hit->response, strlen(hit->response) + 1);
    hit->response = NULL;
}
