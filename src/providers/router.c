#include "seaclaw/providers/router.h"
#include "seaclaw/core/string.h"
#include <stdlib.h>
#include <string.h>

typedef struct sc_router_resolved {
    size_t provider_index;
    const char *model;
    size_t model_len;
} sc_router_resolved_t;

typedef struct sc_router_route_internal {
    char *hint;
    size_t hint_len;
    size_t provider_index;
    char *model;
    size_t model_len;
} sc_router_route_internal_t;

typedef struct sc_router_ctx {
    sc_allocator_t *alloc;
    sc_provider_t *providers;
    size_t provider_count;
    sc_router_route_internal_t *routes;
    size_t route_count;
    char *default_model;
    size_t default_model_len;
} sc_router_ctx_t;

static void resolve_model(sc_router_ctx_t *r, const char *model, size_t model_len,
    sc_router_resolved_t *out)
{
    if (model_len >= SC_HINT_PREFIX_LEN && memcmp(model, SC_HINT_PREFIX, SC_HINT_PREFIX_LEN) == 0) {
        const char *hint = model + SC_HINT_PREFIX_LEN;
        size_t hint_len = model_len - SC_HINT_PREFIX_LEN;
        for (size_t i = 0; i < r->route_count; i++) {
            if (r->routes[i].hint_len == hint_len && memcmp(r->routes[i].hint, hint, hint_len) == 0) {
                out->provider_index = r->routes[i].provider_index;
                out->model = r->routes[i].model;
                out->model_len = r->routes[i].model_len;
                return;
            }
        }
    }
    out->provider_index = 0;
    out->model = model;
    out->model_len = model_len;
}

static sc_error_t router_chat_with_system(void *ctx, sc_allocator_t *alloc,
    const char *system_prompt, size_t system_prompt_len,
    const char *message, size_t message_len,
    const char *model, size_t model_len,
    double temperature,
    char **out, size_t *out_len)
{
    sc_router_ctx_t *r = (sc_router_ctx_t *)ctx;
    sc_router_resolved_t res;
    resolve_model(r, model, model_len, &res);
    if (res.provider_index >= r->provider_count) return SC_ERR_PROVIDER_RESPONSE;
    const sc_provider_vtable_t *vt = r->providers[res.provider_index].vtable;
    if (!vt || !vt->chat_with_system) return SC_ERR_INVALID_ARGUMENT;
    return vt->chat_with_system(r->providers[res.provider_index].ctx, alloc,
        system_prompt, system_prompt_len, message, message_len,
        res.model, res.model_len, temperature, out, out_len);
}

static sc_error_t router_chat(void *ctx, sc_allocator_t *alloc,
    const sc_chat_request_t *request,
    const char *model, size_t model_len,
    double temperature,
    sc_chat_response_t *out)
{
    sc_router_ctx_t *r = (sc_router_ctx_t *)ctx;
    sc_router_resolved_t res;
    resolve_model(r, model, model_len, &res);
    if (res.provider_index >= r->provider_count) return SC_ERR_PROVIDER_RESPONSE;
    const sc_provider_vtable_t *vt = r->providers[res.provider_index].vtable;
    if (!vt || !vt->chat) return SC_ERR_INVALID_ARGUMENT;
    return vt->chat(r->providers[res.provider_index].ctx, alloc, request,
        res.model, res.model_len, temperature, out);
}

static bool router_supports_native_tools(void *ctx) {
    sc_router_ctx_t *r = (sc_router_ctx_t *)ctx;
    if (r->provider_count == 0) return false;
    const sc_provider_vtable_t *vt = r->providers[0].vtable;
    return vt && vt->supports_native_tools && vt->supports_native_tools(r->providers[0].ctx);
}

static bool router_supports_vision(void *ctx) {
    sc_router_ctx_t *r = (sc_router_ctx_t *)ctx;
    if (r->provider_count == 0) return false;
    const sc_provider_vtable_t *vt = r->providers[0].vtable;
    if (vt && vt->supports_vision) return vt->supports_vision(r->providers[0].ctx);
    return false;
}

static bool router_supports_vision_for_model(void *ctx, const char *model, size_t model_len) {
    sc_router_ctx_t *r = (sc_router_ctx_t *)ctx;
    sc_router_resolved_t res;
    resolve_model(r, model, model_len, &res);
    if (res.provider_index >= r->provider_count) return false;
    const sc_provider_vtable_t *vt = r->providers[res.provider_index].vtable;
    return vt && vt->supports_vision_for_model && vt->supports_vision_for_model(r->providers[res.provider_index].ctx, model, model_len);
}

static const char *router_get_name(void *ctx) { (void)ctx; return "router"; }

static void router_deinit(void *ctx, sc_allocator_t *alloc) {
    sc_router_ctx_t *r = (sc_router_ctx_t *)ctx;
    for (size_t i = 0; i < r->route_count; i++) {
        if (r->routes[i].hint) alloc->free(alloc->ctx, r->routes[i].hint, r->routes[i].hint_len + 1);
        if (r->routes[i].model) alloc->free(alloc->ctx, r->routes[i].model, r->routes[i].model_len + 1);
    }
    if (r->routes) alloc->free(alloc->ctx, r->routes, sizeof(sc_router_route_internal_t) * r->route_count);
    if (r->default_model) alloc->free(alloc->ctx, r->default_model, r->default_model_len + 1);
    if (r->providers) alloc->free(alloc->ctx, r->providers, sizeof(sc_provider_t) * r->provider_count);
    alloc->free(alloc->ctx, r, sizeof(*r));
}

static const sc_provider_vtable_t router_vtable = {
    .chat_with_system = router_chat_with_system,
    .chat = router_chat,
    .supports_native_tools = router_supports_native_tools,
    .get_name = router_get_name,
    .deinit = router_deinit,
    .warmup = NULL, .chat_with_tools = NULL,
    .supports_streaming = NULL,
    .supports_vision = router_supports_vision,
    .supports_vision_for_model = router_supports_vision_for_model,
    .stream_chat = NULL,
};

sc_error_t sc_router_create(sc_allocator_t *alloc,
    const char * const *provider_names,
    const size_t *provider_name_lens,
    size_t provider_count,
    sc_provider_t *providers,
    const sc_router_route_entry_t *routes,
    size_t route_count,
    const char *default_model,
    size_t default_model_len,
    sc_provider_t *out)
{
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    if (provider_count == 0 || !providers) return SC_ERR_INVALID_ARGUMENT;

    sc_router_ctx_t *r = (sc_router_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*r));
    if (!r) return SC_ERR_OUT_OF_MEMORY;
    memset(r, 0, sizeof(*r));
    r->alloc = alloc;

    sc_provider_t *prov_copy = (sc_provider_t *)alloc->alloc(alloc->ctx, sizeof(sc_provider_t) * provider_count);
    if (!prov_copy) { alloc->free(alloc->ctx, r, sizeof(*r)); return SC_ERR_OUT_OF_MEMORY; }
    memcpy(prov_copy, providers, sizeof(sc_provider_t) * provider_count);
    r->providers = prov_copy;
    r->provider_count = provider_count;

    r->default_model = sc_strndup(alloc, default_model ? default_model : "default", default_model_len ? default_model_len : 7);
    r->default_model_len = default_model_len ? default_model_len : 7;

    if (route_count > 0 && routes && provider_names && provider_name_lens) {
        sc_router_route_internal_t *ri = (sc_router_route_internal_t *)alloc->alloc(alloc->ctx, sizeof(sc_router_route_internal_t) * route_count);
        if (!ri) {
            alloc->free(alloc->ctx, prov_copy, sizeof(sc_provider_t) * provider_count);
            if (r->default_model) alloc->free(alloc->ctx, r->default_model, r->default_model_len + 1);
            alloc->free(alloc->ctx, r, sizeof(*r));
            return SC_ERR_OUT_OF_MEMORY;
        }
        memset(ri, 0, sizeof(sc_router_route_internal_t) * route_count);
        r->routes = ri;
        r->route_count = route_count;
        for (size_t i = 0; i < route_count; i++) {
            size_t hint_len = routes[i].hint_len;
            ri[i].hint = sc_strndup(alloc, routes[i].hint, hint_len);
            ri[i].hint_len = hint_len;
            /* Resolve provider_name -> index */
            for (size_t j = 0; j < provider_count; j++) {
                if (provider_name_lens[j] == routes[i].route.provider_name_len &&
                    memcmp(provider_names[j], routes[i].route.provider_name, provider_name_lens[j]) == 0) {
                    ri[i].provider_index = j;
                    break;
                }
            }
            ri[i].model = sc_strndup(alloc, routes[i].route.model, routes[i].route.model_len);
            ri[i].model_len = routes[i].route.model_len;
        }
    }

    out->ctx = r;
    out->vtable = &router_vtable;
    return SC_OK;
}
