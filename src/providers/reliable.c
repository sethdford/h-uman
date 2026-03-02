#include "seaclaw/providers/reliable.h"
#include <stdint.h>
#include "seaclaw/providers/error_classify.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef SC_GATEWAY_POSIX
#include <unistd.h>
#endif

#define SC_LAST_ERROR_MAX 256
#define SC_MODEL_CHAIN_MAX 32

typedef struct sc_model_ref {
    const char *model;
    size_t model_len;
} sc_model_ref_t;

typedef struct sc_reliable_ctx {
    sc_provider_t inner;
    sc_reliable_provider_entry_t *extras;
    size_t extras_count;
    sc_reliable_model_fallback_entry_t *model_fallbacks;
    size_t model_fallbacks_count;
    uint32_t max_retries;
    uint64_t base_backoff_ms;
    char last_error_msg[SC_LAST_ERROR_MAX];
    size_t last_error_len;
} sc_reliable_ctx_t;

/* Store error for retry-after / classification */
static void store_error(sc_reliable_ctx_t *r, sc_error_t err) {
    const char *name = sc_error_string(err);
    if (name) {
        size_t n = strlen(name);
        if (n >= SC_LAST_ERROR_MAX) n = SC_LAST_ERROR_MAX - 1;
        memcpy(r->last_error_msg, name, n);
        r->last_error_msg[n] = '\0';
        r->last_error_len = n;
    } else {
        r->last_error_len = 0;
    }
}

static uint64_t compute_backoff(sc_reliable_ctx_t *r, uint64_t base) {
    uint64_t retry_ms = sc_error_parse_retry_after_ms(r->last_error_msg, r->last_error_len);
    if (retry_ms > 0) {
        if (retry_ms > 30000) retry_ms = 30000;
        if (retry_ms < base) retry_ms = base;
        return retry_ms;
    }
    return base;
}

static sc_error_t final_failure(sc_reliable_ctx_t *r) {
    const char *msg = r->last_error_msg;
    size_t len = r->last_error_len;
    if (sc_error_is_context_exhausted(msg, len)) return SC_ERR_PROVIDER_RESPONSE;
    if (sc_error_is_rate_limited(msg, len)) return SC_ERR_PROVIDER_RATE_LIMITED;
    if (sc_error_is_vision_unsupported_text(msg, len)) return SC_ERR_PROVIDER_RESPONSE;
    return SC_ERR_PROVIDER_RESPONSE; /* AllProvidersFailed */
}

/* Check if model matches (length-aware comparison) */
static bool model_eq(const char *a, size_t a_len, const char *b, size_t b_len) {
    if (a_len != b_len) return false;
    return memcmp(a, b, a_len) == 0;
}

/* Build model chain: [model, fallback1, fallback2, ...]. Caller frees chain. */
static sc_error_t model_chain(sc_reliable_ctx_t *r, sc_allocator_t *alloc,
    const char *model, size_t model_len,
    sc_model_ref_t **out_chain, size_t *out_count)
{
    *out_chain = NULL;
    *out_count = 0;

    for (size_t i = 0; i < r->model_fallbacks_count; i++) {
        const sc_reliable_model_fallback_entry_t *e = &r->model_fallbacks[i];
        if (!model_eq(e->model, e->model_len, model, model_len)) continue;

        size_t total = 1 + (e->fallbacks ? e->fallbacks_count : 0);
        if (total > SC_MODEL_CHAIN_MAX) total = SC_MODEL_CHAIN_MAX;

        sc_model_ref_t *chain = (sc_model_ref_t *)alloc->alloc(alloc->ctx,
            total * sizeof(sc_model_ref_t));
        if (!chain) return SC_ERR_OUT_OF_MEMORY;

        chain[0].model = model;
        chain[0].model_len = model_len;
        for (size_t j = 0; j < total - 1 && e->fallbacks; j++) {
            chain[1 + j].model = e->fallbacks[j].model;
            chain[1 + j].model_len = e->fallbacks[j].model_len;
        }
        *out_chain = chain;
        *out_count = total;
        return SC_OK;
    }

    /* No fallbacks: single-element chain */
    sc_model_ref_t *chain = (sc_model_ref_t *)alloc->alloc(alloc->ctx, sizeof(sc_model_ref_t));
    if (!chain) return SC_ERR_OUT_OF_MEMORY;
    chain[0].model = model;
    chain[0].model_len = model_len;
    *out_chain = chain;
    *out_count = 1;
    return SC_OK;
}

/* Try a single provider with retries for chat_with_system. Returns SC_OK and sets out/out_len on success. */
static sc_error_t try_chat_with_system(sc_reliable_ctx_t *r, sc_allocator_t *alloc,
    sc_provider_t *prov,
    const char *system_prompt, size_t system_prompt_len,
    const char *message, size_t message_len,
    const char *model, size_t model_len,
    double temperature,
    char **out, size_t *out_len)
{
    const sc_provider_vtable_t *vt = prov->vtable;
    if (!vt || !vt->chat_with_system) return SC_ERR_INVALID_ARGUMENT;

    uint64_t backoff_ms = r->base_backoff_ms;
    if (backoff_ms < 50) backoff_ms = 50;

    for (uint32_t attempt = 0; attempt <= r->max_retries; attempt++) {
        sc_error_t err = vt->chat_with_system(prov->ctx, alloc,
            system_prompt, system_prompt_len,
            message, message_len,
            model, model_len,
            temperature,
            out, out_len);
        if (err == SC_OK) return SC_OK;

        store_error(r, err);
        const char *msg = r->last_error_msg;
        size_t len = r->last_error_len;

        if (sc_error_is_non_retryable(msg, len)) return err;
        if (sc_error_is_rate_limited(msg, len) && r->extras_count > 0) break; /* try next provider */

        if (attempt < r->max_retries) {
            uint64_t wait = compute_backoff(r, backoff_ms);
#ifndef SC_IS_TEST
#ifdef SC_GATEWAY_POSIX
            if (wait > 0) {
                struct timespec ts = { .tv_sec = (time_t)(wait / 1000), .tv_nsec = (long)((wait % 1000) * 1000000) };
                nanosleep(&ts, NULL);
            }
#endif
#else
            (void)wait;
#endif
            if (backoff_ms * 2 <= 10000) backoff_ms *= 2;
        }
    }
    return final_failure(r);
}

/* Try a single provider with retries for chat. Returns SC_OK on success. */
static sc_error_t try_chat(sc_reliable_ctx_t *r, sc_allocator_t *alloc,
    sc_provider_t *prov,
    const sc_chat_request_t *request,
    const char *model, size_t model_len,
    double temperature,
    sc_chat_response_t *out)
{
    const sc_provider_vtable_t *vt = prov->vtable;
    if (!vt || !vt->chat) return SC_ERR_INVALID_ARGUMENT;

    uint64_t backoff_ms = r->base_backoff_ms;
    if (backoff_ms < 50) backoff_ms = 50;

    for (uint32_t attempt = 0; attempt <= r->max_retries; attempt++) {
        memset(out, 0, sizeof(*out));
        sc_error_t err = vt->chat(prov->ctx, alloc, request, model, model_len, temperature, out);
        if (err == SC_OK) return SC_OK;

        store_error(r, err);
        const char *msg = r->last_error_msg;
        size_t len = r->last_error_len;

        if (sc_error_is_non_retryable(msg, len)) return err;
        if (sc_error_is_rate_limited(msg, len) && r->extras_count > 0) break;

        if (attempt < r->max_retries) {
            uint64_t wait = compute_backoff(r, backoff_ms);
#ifndef SC_IS_TEST
#ifdef SC_GATEWAY_POSIX
            if (wait > 0) {
                struct timespec ts = { .tv_sec = (time_t)(wait / 1000), .tv_nsec = (long)((wait % 1000) * 1000000) };
                nanosleep(&ts, NULL);
            }
#endif
#else
            (void)wait;
#endif
            if (backoff_ms * 2 <= 10000) backoff_ms *= 2;
        }
    }
    return final_failure(r);
}

static sc_error_t reliable_chat_with_system(void *ctx, sc_allocator_t *alloc,
    const char *system_prompt, size_t system_prompt_len,
    const char *message, size_t message_len,
    const char *model, size_t model_len,
    double temperature,
    char **out, size_t *out_len)
{
    sc_reliable_ctx_t *r = (sc_reliable_ctx_t *)ctx;
    *out = NULL;
    *out_len = 0;

    sc_model_ref_t *chain = NULL;
    size_t chain_count = 0;
    sc_error_t err = model_chain(r, alloc, model, model_len, &chain, &chain_count);
    if (err != SC_OK) return err;

    for (size_t m = 0; m < chain_count; m++) {
        const char *cur_model = chain[m].model;
        size_t cur_len = chain[m].model_len;

        /* Try primary provider */
        err = try_chat_with_system(r, alloc, &r->inner,
            system_prompt, system_prompt_len,
            message, message_len,
            cur_model, cur_len,
            temperature,
            out, out_len);
        if (err == SC_OK) {
            alloc->free(alloc->ctx, chain, chain_count * sizeof(sc_model_ref_t));
            return SC_OK;
        }

        /* Try extras */
        for (size_t e = 0; e < r->extras_count; e++) {
            err = try_chat_with_system(r, alloc, &r->extras[e].provider,
                system_prompt, system_prompt_len,
                message, message_len,
                cur_model, cur_len,
                temperature,
                out, out_len);
            if (err == SC_OK) {
                alloc->free(alloc->ctx, chain, chain_count * sizeof(sc_model_ref_t));
                return SC_OK;
            }
        }
    }

    alloc->free(alloc->ctx, chain, chain_count * sizeof(sc_model_ref_t));
    return final_failure(r);
}

static sc_error_t reliable_chat(void *ctx, sc_allocator_t *alloc,
    const sc_chat_request_t *request,
    const char *model, size_t model_len,
    double temperature,
    sc_chat_response_t *out)
{
    sc_reliable_ctx_t *r = (sc_reliable_ctx_t *)ctx;
    memset(out, 0, sizeof(*out));

    sc_model_ref_t *chain = NULL;
    size_t chain_count = 0;
    sc_error_t err = model_chain(r, alloc, model, model_len, &chain, &chain_count);
    if (err != SC_OK) return err;

    for (size_t m = 0; m < chain_count; m++) {
        const char *cur_model = chain[m].model;
        size_t cur_len = chain[m].model_len;

        err = try_chat(r, alloc, &r->inner, request, cur_model, cur_len, temperature, out);
        if (err == SC_OK) {
            alloc->free(alloc->ctx, chain, chain_count * sizeof(sc_model_ref_t));
            return SC_OK;
        }

        for (size_t e = 0; e < r->extras_count; e++) {
            err = try_chat(r, alloc, &r->extras[e].provider, request,
                cur_model, cur_len, temperature, out);
            if (err == SC_OK) {
                alloc->free(alloc->ctx, chain, chain_count * sizeof(sc_model_ref_t));
                return SC_OK;
            }
        }
    }

    alloc->free(alloc->ctx, chain, chain_count * sizeof(sc_model_ref_t));
    return final_failure(r);
}

static bool reliable_supports_native_tools(void *ctx) {
    sc_reliable_ctx_t *r = (sc_reliable_ctx_t *)ctx;
    if (r->inner.vtable && r->inner.vtable->supports_native_tools &&
        r->inner.vtable->supports_native_tools(r->inner.ctx))
        return true;
    for (size_t i = 0; i < r->extras_count; i++) {
        const sc_provider_vtable_t *vt = r->extras[i].provider.vtable;
        if (vt && vt->supports_native_tools && vt->supports_native_tools(r->extras[i].provider.ctx))
            return true;
    }
    return false;
}

static bool reliable_supports_vision(void *ctx) {
    sc_reliable_ctx_t *r = (sc_reliable_ctx_t *)ctx;
    if (r->inner.vtable && r->inner.vtable->supports_vision &&
        r->inner.vtable->supports_vision(r->inner.ctx))
        return true;
    for (size_t i = 0; i < r->extras_count; i++) {
        const sc_provider_vtable_t *vt = r->extras[i].provider.vtable;
        if (vt && vt->supports_vision && vt->supports_vision(r->extras[i].provider.ctx))
            return true;
    }
    return false;
}

static bool reliable_supports_vision_for_model(void *ctx, const char *model, size_t model_len) {
    (void)model;
    (void)model_len;
    return reliable_supports_vision(ctx);
}

static const char *reliable_get_name(void *ctx) {
    sc_reliable_ctx_t *r = (sc_reliable_ctx_t *)ctx;
    return r->inner.vtable && r->inner.vtable->get_name ?
        r->inner.vtable->get_name(r->inner.ctx) : "reliable";
}

static void reliable_deinit(void *ctx, sc_allocator_t *alloc) {
    sc_reliable_ctx_t *r = (sc_reliable_ctx_t *)ctx;
    if (r->inner.vtable && r->inner.vtable->deinit)
        r->inner.vtable->deinit(r->inner.ctx, alloc);
    for (size_t i = 0; i < r->extras_count; i++) {
        const sc_provider_vtable_t *vt = r->extras[i].provider.vtable;
        if (vt && vt->deinit)
            vt->deinit(r->extras[i].provider.ctx, alloc);
    }
    size_t alloc_size = sizeof(sc_reliable_ctx_t) +
        r->extras_count * sizeof(sc_reliable_provider_entry_t) +
        r->model_fallbacks_count * sizeof(sc_reliable_model_fallback_entry_t);
    alloc->free(alloc->ctx, ctx, alloc_size);
}

static void reliable_warmup(void *ctx) {
    sc_reliable_ctx_t *r = (sc_reliable_ctx_t *)ctx;
    if (r->inner.vtable && r->inner.vtable->warmup)
        r->inner.vtable->warmup(r->inner.ctx);
    for (size_t i = 0; i < r->extras_count; i++) {
        const sc_provider_vtable_t *vt = r->extras[i].provider.vtable;
        if (vt && vt->warmup)
            vt->warmup(r->extras[i].provider.ctx);
    }
}

static const sc_provider_vtable_t reliable_vtable = {
    .chat_with_system = reliable_chat_with_system,
    .chat = reliable_chat,
    .supports_native_tools = reliable_supports_native_tools,
    .get_name = reliable_get_name,
    .deinit = reliable_deinit,
    .warmup = reliable_warmup,
    .chat_with_tools = NULL,
    .supports_streaming = NULL,
    .supports_vision = reliable_supports_vision,
    .supports_vision_for_model = reliable_supports_vision_for_model,
    .stream_chat = NULL,
};

sc_error_t sc_reliable_create(sc_allocator_t *alloc,
    sc_provider_t inner,
    uint32_t max_retries,
    uint64_t backoff_ms,
    sc_provider_t *out)
{
    return sc_reliable_create_ex(alloc, inner, max_retries, backoff_ms,
        NULL, 0, NULL, 0, out);
}

sc_error_t sc_reliable_create_ex(sc_allocator_t *alloc,
    sc_provider_t inner,
    uint32_t max_retries,
    uint64_t backoff_ms,
    const sc_reliable_provider_entry_t *extras,
    size_t extras_count,
    const sc_reliable_model_fallback_entry_t *model_fallbacks,
    size_t model_fallbacks_count,
    sc_provider_t *out)
{
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;

    size_t ctx_size = sizeof(sc_reliable_ctx_t);
    if (extras_count > 0) {
        ctx_size += extras_count * sizeof(sc_reliable_provider_entry_t);
    }
    if (model_fallbacks_count > 0) {
        ctx_size += model_fallbacks_count * sizeof(sc_reliable_model_fallback_entry_t);
    }

    sc_reliable_ctx_t *r = (sc_reliable_ctx_t *)alloc->alloc(alloc->ctx, ctx_size);
    if (!r) return SC_ERR_OUT_OF_MEMORY;
    memset(r, 0, ctx_size);

    r->inner = inner;
    r->max_retries = max_retries;
    r->base_backoff_ms = (backoff_ms >= 50) ? backoff_ms : 50;

    if (extras_count > 0 && extras) {
        r->extras = (sc_reliable_provider_entry_t *)((char *)r + sizeof(sc_reliable_ctx_t));
        memcpy(r->extras, extras, extras_count * sizeof(sc_reliable_provider_entry_t));
        r->extras_count = extras_count;
    }
    if (model_fallbacks_count > 0 && model_fallbacks) {
        r->model_fallbacks = (sc_reliable_model_fallback_entry_t *)((char *)r +
            sizeof(sc_reliable_ctx_t) + extras_count * sizeof(sc_reliable_provider_entry_t));
        memcpy(r->model_fallbacks, model_fallbacks,
            model_fallbacks_count * sizeof(sc_reliable_model_fallback_entry_t));
        r->model_fallbacks_count = model_fallbacks_count;
    }

    out->ctx = r;
    out->vtable = &reliable_vtable;
    return SC_OK;
}
