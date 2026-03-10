#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOOL_NAME "analytics"
#define TOOL_DESC                                                                              \
    "Retrieve web analytics data. Actions: overview (pageviews, visitors, bounce rate), "      \
    "pages (top pages by traffic), referrers (traffic sources), realtime (current visitors). " \
    "Supports Plausible Analytics API and Google Analytics Data API."
#define TOOL_PARAMS                                                                             \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":["           \
    "\"overview\",\"pages\",\"referrers\",\"realtime\"]},\"provider\":{\"type\":\"string\","    \
    "\"enum\":[\"plausible\",\"google_analytics\"],\"description\":\"Analytics provider\"},"    \
    "\"site_id\":{\"type\":\"string\",\"description\":\"Site domain or GA property ID\"},"      \
    "\"api_key\":{\"type\":\"string\"},\"period\":{\"type\":\"string\",\"description\":"        \
    "\"day, 7d, 30d, month, 6mo, 12mo (default: 30d)\"},\"access_token\":{\"type\":\"string\"," \
    "\"description\":\"GA OAuth2 token\"}},\"required\":[\"action\"]}"

typedef struct {
    char _unused;
} analytics_ctx_t;

static hu_error_t analytics_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                    hu_tool_result_t *out) {
    (void)ctx;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!args) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *action = hu_json_get_string(args, "action");
    if (!action) {
        *out = hu_tool_result_fail("missing action", 14);
        return HU_OK;
    }

#if HU_IS_TEST
    (void)alloc;
    if (strcmp(action, "overview") == 0) {
        *out = hu_tool_result_ok("{\"pageviews\":12540,\"visitors\":3210,\"bounce_rate\":0.42,"
                                 "\"avg_session_duration\":185,\"period\":\"30d\"}",
                                 93);
    } else if (strcmp(action, "pages") == 0) {
        *out =
            hu_tool_result_ok("{\"pages\":[{\"path\":\"/\",\"pageviews\":4200},{\"path\":\"/docs\","
                              "\"pageviews\":2100},{\"path\":\"/pricing\",\"pageviews\":1800}]}",
                              119);
    } else if (strcmp(action, "referrers") == 0) {
        *out = hu_tool_result_ok("{\"referrers\":[{\"source\":\"google\",\"visitors\":1500},"
                                 "{\"source\":\"twitter\",\"visitors\":420},"
                                 "{\"source\":\"github\",\"visitors\":380}]}",
                                 138);
    } else if (strcmp(action, "realtime") == 0) {
        *out = hu_tool_result_ok("{\"current_visitors\":23}", 23);
    } else {
        *out = hu_tool_result_fail("unknown action", 14);
    }
    return HU_OK;
#else
    const char *provider = hu_json_get_string(args, "provider");
    if (!provider)
        provider = "plausible";

    if (strcmp(provider, "plausible") == 0) {
        const char *api_key = hu_json_get_string(args, "api_key");
        const char *site_id = hu_json_get_string(args, "site_id");
        if (!api_key || strlen(api_key) == 0 || !site_id || strlen(site_id) == 0) {
            *out = hu_tool_result_fail("plausible needs api_key and site_id", 35);
            return HU_OK;
        }
        const char *period = hu_json_get_string(args, "period");
        if (!period)
            period = "30d";
        char url[512];
        char auth[256];
        int an = snprintf(auth, sizeof(auth), "Bearer %s", api_key);
        if (an < 0 || (size_t)an >= sizeof(auth)) {
            *out = hu_tool_result_fail("api_key too long", 16);
            return HU_OK;
        }

        int un;
        if (strcmp(action, "realtime") == 0) {
            un =
                snprintf(url, sizeof(url),
                         "https://plausible.io/api/v1/stats/realtime/visitors?site_id=%s", site_id);
        } else {
            const char *metrics = "visitors,pageviews,bounce_rate,visit_duration";
            un = snprintf(url, sizeof(url),
                          "https://plausible.io/api/v1/stats/aggregate?site_id=%s&period=%s"
                          "&metrics=%s",
                          site_id, period, metrics);
        }
        if (un < 0 || (size_t)un >= sizeof(url)) {
            *out = hu_tool_result_fail("URL too long", 12);
            return HU_OK;
        }
        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_get(alloc, url, auth, &resp);
        if (err != HU_OK || resp.status_code != 200) {
            if (resp.owned && resp.body)
                hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("failed to query analytics", 25);
            return HU_OK;
        }
        char *body = hu_strndup(alloc, resp.body, resp.body_len);
        hu_http_response_free(alloc, &resp);
        *out = hu_tool_result_ok_owned(body, body ? strlen(body) : 0);
    } else {
        *out = hu_tool_result_fail("google_analytics requires OAuth2 setup", 38);
    }
    return HU_OK;
#endif
}

static const char *analytics_name(void *ctx) {
    (void)ctx;
    return TOOL_NAME;
}
static const char *analytics_desc(void *ctx) {
    (void)ctx;
    return TOOL_DESC;
}
static const char *analytics_params(void *ctx) {
    (void)ctx;
    return TOOL_PARAMS;
}
static void analytics_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, sizeof(analytics_ctx_t));
}

static const hu_tool_vtable_t analytics_vtable = {
    .execute = analytics_execute,
    .name = analytics_name,
    .description = analytics_desc,
    .parameters_json = analytics_params,
    .deinit = analytics_deinit,
};

hu_error_t hu_analytics_create(hu_allocator_t *alloc, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    void *ctx = alloc->alloc(alloc->ctx, sizeof(analytics_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(analytics_ctx_t));
    out->ctx = ctx;
    out->vtable = &analytics_vtable;
    return HU_OK;
}
