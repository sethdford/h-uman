/*
 * Web search tool — Brave, DuckDuckGo, Tavily providers.
 * Provider selection via config (tools.web_search_provider), env (WEB_SEARCH_PROVIDER), or arg.
 * Default: duckduckgo.
 */
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#include "human/tools/validation.h"
#include "human/tools/web_search_providers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_WEB_SEARCH_NAME "web_search"
#define HU_WEB_SEARCH_DESC                                                                         \
    "Search the web for information. Returns search results with titles, URLs, and snippets. Use " \
    "this for factual questions, current events, locations, or finding information online. "       \
    "Preferred over browser for all information retrieval."
#define HU_WEB_SEARCH_PARAMS                                                                     \
    "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"minLength\":1},"      \
    "\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":10,\"default\":5},\"provider\":{" \
    "\"type\":\"string\"}},\"required\":[\"query\"]}"
#define HU_WEB_QUERY_MAX 2048
#define HU_DEFAULT_COUNT 5
#define HU_MAX_COUNT     10

typedef struct hu_web_search_ctx {
    char *provider;
    size_t provider_len;
    char *api_key;
    size_t api_key_len;
} hu_web_search_ctx_t;

#if !HU_IS_TEST
static const char *get_env(const char *name) {
#ifdef _WIN32
    (void)name;
    return NULL;
#else
    return getenv(name);
#endif
}
#endif

static hu_error_t web_search_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                     hu_tool_result_t *out) {
    hu_web_search_ctx_t *wctx = (hu_web_search_ctx_t *)ctx;
    if (!args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *query = hu_json_get_string(args, "query");
    if (!query || strlen(query) == 0) {
        *out = hu_tool_result_fail("Missing required 'query' parameter", 33);
        return HU_OK;
    }
    size_t qlen = strlen(query);
    /* Check not all whitespace */
    size_t i;
    for (i = 0;
         i < qlen && (query[i] == ' ' || query[i] == '\t' || query[i] == '\n' || query[i] == '\r');
         i++) {}
    if (i >= qlen) {
        *out = hu_tool_result_fail("'query' must not be empty", 25);
        return HU_OK;
    }
    if (qlen > HU_WEB_QUERY_MAX) {
        *out = hu_tool_result_fail("query too long", 14);
        return HU_OK;
    }

    double count_val = hu_json_get_number(args, "count", HU_DEFAULT_COUNT);
    int count = (int)count_val;
    if (count < 1)
        count = 1;
    if (count > HU_MAX_COUNT)
        count = HU_MAX_COUNT;

    const char *provider = hu_json_get_string(args, "provider");
    if (!provider || !provider[0]) {
        provider =
            (wctx && wctx->provider && wctx->provider_len > 0) ? wctx->provider : "duckduckgo";
    }

#if HU_IS_TEST
    /* Mock search results in test mode */
    size_t cap = 512;
    char *msg = (char *)alloc->alloc(alloc->ctx, cap);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(msg, cap,
                     "Results for: %.*s\n\n1. Example result (mock)\n   https://example.com/1\n   "
                     "Mock snippet for testing.\n\n2. Second mock result\n   "
                     "https://example.com/2\n   Another mock snippet.\n",
                     (int)(qlen > 64 ? 64 : qlen), query);
    if (n > 0) {
        size_t len = (size_t)n;
        if (len >= cap)
            len = cap - 1;
        msg[len] = '\0';
        *out = hu_tool_result_ok_owned(msg, len);
    } else {
        alloc->free(alloc->ctx, msg, cap);
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    return HU_OK;
#else
    hu_error_t err;
    if (strcmp(provider, "brave") == 0 ||
        (strcmp(provider, "auto") == 0 && get_env("BRAVE_API_KEY"))) {
        const char *key = get_env("BRAVE_API_KEY");
        if (key && key[0]) {
            err = hu_web_search_brave(alloc, query, qlen, count, key, out);
            if (err == HU_OK)
                return HU_OK;
        }
    }
    if (strcmp(provider, "duckduckgo") == 0 || strcmp(provider, "ddg") == 0 ||
        strcmp(provider, "auto") == 0) {
        err = hu_web_search_duckduckgo(alloc, query, qlen, count, out);
        if (err == HU_OK)
            return HU_OK;
    }
    if (strcmp(provider, "tavily") == 0 ||
        (strcmp(provider, "auto") == 0 && get_env("TAVILY_API_KEY"))) {
        const char *key = get_env("TAVILY_API_KEY");
        if (!key || !key[0])
            key = get_env("WEB_SEARCH_API_KEY");
        if (key && key[0]) {
            err = hu_web_search_tavily(alloc, query, qlen, count, key, out);
            if (err == HU_OK)
                return HU_OK;
        }
    }
    if (strcmp(provider, "exa") == 0 || (strcmp(provider, "auto") == 0 && get_env("EXA_API_KEY"))) {
        const char *key = get_env("EXA_API_KEY");
        if (!key || !key[0])
            key = get_env("WEB_SEARCH_API_KEY");
        if (key && key[0]) {
            err = hu_web_search_exa(alloc, query, qlen, count, key, out);
            if (err == HU_OK)
                return HU_OK;
        }
    }
    if (strcmp(provider, "firecrawl") == 0 ||
        (strcmp(provider, "auto") == 0 && get_env("FIRECRAWL_API_KEY"))) {
        const char *key = get_env("FIRECRAWL_API_KEY");
        if (!key || !key[0])
            key = get_env("WEB_SEARCH_API_KEY");
        if (key && key[0]) {
            err = hu_web_search_firecrawl(alloc, query, qlen, count, key, out);
            if (err == HU_OK)
                return HU_OK;
        }
    }
    if (strcmp(provider, "perplexity") == 0 ||
        (strcmp(provider, "auto") == 0 && get_env("PERPLEXITY_API_KEY"))) {
        const char *key = get_env("PERPLEXITY_API_KEY");
        if (!key || !key[0])
            key = get_env("WEB_SEARCH_API_KEY");
        if (key && key[0]) {
            err = hu_web_search_perplexity(alloc, query, qlen, count, key, out);
            if (err == HU_OK)
                return HU_OK;
        }
    }
    if (strcmp(provider, "jina") == 0 ||
        (strcmp(provider, "auto") == 0 && get_env("JINA_API_KEY"))) {
        const char *key = get_env("JINA_API_KEY");
        if (!key || !key[0])
            key = get_env("WEB_SEARCH_API_KEY");
        if (key && key[0]) {
            err = hu_web_search_jina(alloc, query, qlen, count, key, out);
            if (err == HU_OK)
                return HU_OK;
        }
    }
    if (strcmp(provider, "searxng") == 0 ||
        (strcmp(provider, "auto") == 0 && get_env("SEARXNG_BASE_URL"))) {
        const char *base = get_env("SEARXNG_BASE_URL");
        if (base && base[0]) {
            if (hu_tool_validate_url(base) != HU_OK) {
                *out = hu_tool_result_fail(
                    "SEARXNG_BASE_URL must be HTTPS and must not resolve to private/loopback IPs",
                    68);
                return HU_OK;
            }
            err = hu_web_search_searxng(alloc, query, qlen, count, base, out);
            if (err == HU_OK)
                return HU_OK;
        }
    }

    *out = hu_tool_result_fail("All web_search providers failed or none configured", 49);
    return HU_OK;
#endif
}

static const char *web_search_name(void *ctx) {
    (void)ctx;
    return HU_WEB_SEARCH_NAME;
}
static const char *web_search_description(void *ctx) {
    (void)ctx;
    return HU_WEB_SEARCH_DESC;
}
static const char *web_search_parameters_json(void *ctx) {
    (void)ctx;
    return HU_WEB_SEARCH_PARAMS;
}
static void web_search_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)alloc;
    hu_web_search_ctx_t *c = (hu_web_search_ctx_t *)ctx;
    if (c) {
        if (c->provider)
            free(c->provider);
        if (c->api_key)
            free(c->api_key);
        free(c);
    }
}

static const hu_tool_vtable_t web_search_vtable = {
    .execute = web_search_execute,
    .name = web_search_name,
    .description = web_search_description,
    .parameters_json = web_search_parameters_json,
    .deinit = web_search_deinit,
};

hu_error_t hu_web_search_create(hu_allocator_t *alloc, const hu_config_t *config,
                                const char *api_key, size_t api_key_len, hu_tool_t *out) {
    hu_web_search_ctx_t *c = (hu_web_search_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    const char *prov = config ? hu_config_get_web_search_provider(config) : "duckduckgo";
    size_t prov_len = strlen(prov);
    if (prov_len > 0) {
        c->provider = (char *)alloc->alloc(alloc->ctx, prov_len + 1);
        if (!c->provider) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->provider, prov, prov_len + 1);
        c->provider_len = prov_len;
    }
    if (api_key && api_key_len > 0) {
        c->api_key = (char *)alloc->alloc(alloc->ctx, api_key_len + 1);
        if (!c->api_key) {
            if (c->provider)
                alloc->free(alloc->ctx, c->provider, prov_len + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->api_key, api_key, api_key_len);
        c->api_key[api_key_len] = '\0';
        c->api_key_len = api_key_len;
    }
    out->ctx = c;
    out->vtable = &web_search_vtable;
    return HU_OK;
}
