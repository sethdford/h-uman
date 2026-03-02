#include "seaclaw/memory/vector/provider_router.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <stdlib.h>

typedef struct router_ctx {
    sc_allocator_t *alloc;
    sc_embedding_provider_t primary;
    sc_embedding_provider_t *fallbacks;
    size_t fallback_count;
    sc_embedding_route_t *routes;
    size_t route_count;
} router_ctx_t;

static sc_error_t router_embed(void *ctx, sc_allocator_t *alloc,
    const char *text, size_t text_len,
    sc_embedding_provider_result_t *out) {
    router_ctx_t *r = (router_ctx_t *)ctx;
    if (!r || !alloc || !out) return SC_ERR_INVALID_ARGUMENT;

    /* Try primary */
    sc_error_t err = r->primary.vtable->embed(r->primary.ctx, alloc, text, text_len, out);
    if (err == SC_OK) return SC_OK;

    /* Try fallbacks */
    for (size_t i = 0; i < r->fallback_count; i++) {
        sc_embedding_provider_t *fb = &r->fallbacks[i];
        if (fb->vtable && fb->vtable->embed) {
            err = fb->vtable->embed(fb->ctx, alloc, text, text_len, out);
            if (err == SC_OK) return SC_OK;
        }
    }

    return err;
}

static const char *router_name(void *ctx) { (void)ctx; return "auto"; }

static size_t router_dimensions(void *ctx) {
    router_ctx_t *r = (router_ctx_t *)ctx;
    if (!r || !r->primary.vtable || !r->primary.vtable->dimensions) return 0;
    return r->primary.vtable->dimensions(r->primary.ctx);
}

static void router_deinit(void *ctx, sc_allocator_t *alloc) {
    router_ctx_t *r = (router_ctx_t *)ctx;
    if (!r || !alloc) return;
    if (r->primary.vtable && r->primary.vtable->deinit)
        r->primary.vtable->deinit(r->primary.ctx, alloc);
    for (size_t i = 0; i < r->fallback_count; i++) {
        if (r->fallbacks[i].vtable && r->fallbacks[i].vtable->deinit)
            r->fallbacks[i].vtable->deinit(r->fallbacks[i].ctx, alloc);
    }
    if (r->fallbacks) alloc->free(alloc->ctx, r->fallbacks,
        r->fallback_count * sizeof(sc_embedding_provider_t));
    if (r->routes) alloc->free(alloc->ctx, r->routes,
        r->route_count * sizeof(sc_embedding_route_t));
    alloc->free(alloc->ctx, r, sizeof(router_ctx_t));
}

static const sc_embedding_provider_vtable_t router_vtable = {
    .embed = router_embed,
    .name = router_name,
    .dimensions = router_dimensions,
    .deinit = router_deinit,
};

sc_embedding_provider_t sc_embedding_provider_router_create(sc_allocator_t *alloc,
    sc_embedding_provider_t primary,
    const sc_embedding_provider_t *fallbacks,
    size_t fallback_count,
    const sc_embedding_route_t *routes,
    size_t route_count) {
    sc_embedding_provider_t p = { .ctx = NULL, .vtable = &router_vtable };
    if (!alloc) return p;

    router_ctx_t *r = (router_ctx_t *)alloc->alloc(alloc->ctx, sizeof(router_ctx_t));
    if (!r) return p;
    memset(r, 0, sizeof(*r));
    r->alloc = alloc;
    r->primary = primary;

    if (fallback_count > 0 && fallbacks) {
        r->fallbacks = (sc_embedding_provider_t *)alloc->alloc(alloc->ctx,
            fallback_count * sizeof(sc_embedding_provider_t));
        if (r->fallbacks) {
            memcpy(r->fallbacks, fallbacks, fallback_count * sizeof(sc_embedding_provider_t));
            r->fallback_count = fallback_count;
        }
    }

    if (route_count > 0 && routes) {
        r->routes = (sc_embedding_route_t *)alloc->alloc(alloc->ctx,
            route_count * sizeof(sc_embedding_route_t));
        if (r->routes) {
            memcpy(r->routes, routes, route_count * sizeof(sc_embedding_route_t));
            r->route_count = route_count;
        }
    }

    p.ctx = r;
    return p;
}

const char *sc_embedding_extract_hint(const char *model, size_t model_len) {
    if (!model || model_len < 5) return NULL;
    if (memcmp(model, "hint:", 5) != 0) return NULL;
    return model + 5;
}
