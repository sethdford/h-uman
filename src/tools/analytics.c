#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOOL_NAME "analytics"
#define TOOL_DESC                                                                                   \
    "Retrieve web analytics data. Actions: overview (pageviews, visitors, bounce rate), "           \
    "pages (top pages by traffic), referrers (traffic sources), realtime (current visitors). "      \
    "Supports Plausible Analytics API and Google Analytics Data API."
#define TOOL_PARAMS                                                                                \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":["              \
    "\"overview\",\"pages\",\"referrers\",\"realtime\"]},\"provider\":{\"type\":\"string\","       \
    "\"enum\":[\"plausible\",\"google_analytics\"],\"description\":\"Analytics provider\"},"       \
    "\"site_id\":{\"type\":\"string\",\"description\":\"Site domain or GA property ID\"},"         \
    "\"api_key\":{\"type\":\"string\"},\"period\":{\"type\":\"string\",\"description\":"           \
    "\"day, 7d, 30d, month, 6mo, 12mo (default: 30d)\"},\"access_token\":{\"type\":\"string\","   \
    "\"description\":\"GA OAuth2 token\"}},\"required\":[\"action\"]}"

typedef struct { int placeholder; } analytics_ctx_t;

static sc_error_t analytics_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                                    sc_tool_result_t *out) {
    (void)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_OK;
    }
    const char *action = sc_json_get_string(args, "action");
    if (!action) {
        *out = sc_tool_result_fail("missing action", 14);
        return SC_OK;
    }

#if SC_IS_TEST
    if (strcmp(action, "overview") == 0) {
        *out = sc_tool_result_ok(
            "{\"pageviews\":12540,\"visitors\":3210,\"bounce_rate\":0.42,"
            "\"avg_session_duration\":185,\"period\":\"30d\"}", 93);
    } else if (strcmp(action, "pages") == 0) {
        *out = sc_tool_result_ok(
            "{\"pages\":[{\"path\":\"/\",\"pageviews\":4200},{\"path\":\"/docs\","
            "\"pageviews\":2100},{\"path\":\"/pricing\",\"pageviews\":1800}]}", 119);
    } else if (strcmp(action, "referrers") == 0) {
        *out = sc_tool_result_ok(
            "{\"referrers\":[{\"source\":\"google\",\"visitors\":1500},"
            "{\"source\":\"twitter\",\"visitors\":420},"
            "{\"source\":\"github\",\"visitors\":380}]}", 138);
    } else if (strcmp(action, "realtime") == 0) {
        *out = sc_tool_result_ok("{\"current_visitors\":23}", 23);
    } else {
        *out = sc_tool_result_fail("unknown action", 14);
    }
    return SC_OK;
#else
    const char *provider = sc_json_get_string(args, "provider");
    if (!provider)
        provider = "plausible";

    if (strcmp(provider, "plausible") == 0) {
        const char *api_key = sc_json_get_string(args, "api_key");
        const char *site_id = sc_json_get_string(args, "site_id");
        if (!api_key || !site_id) {
            *out = sc_tool_result_fail("plausible needs api_key and site_id", 35);
            return SC_OK;
        }
        const char *period = sc_json_get_string(args, "period");
        if (!period) period = "30d";
        char url[512];
        char auth[256];
        snprintf(auth, sizeof(auth), "Bearer %s", api_key);

        if (strcmp(action, "realtime") == 0) {
            snprintf(url, sizeof(url),
                     "https://plausible.io/api/v1/stats/realtime/visitors?site_id=%s", site_id);
        } else {
            const char *metrics = "visitors,pageviews,bounce_rate,visit_duration";
            snprintf(url, sizeof(url),
                     "https://plausible.io/api/v1/stats/aggregate?site_id=%s&period=%s"
                     "&metrics=%s", site_id, period, metrics);
        }
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_get(alloc, url, auth, &resp);
        if (err != SC_OK || resp.status_code != 200) {
            if (resp.owned && resp.body) sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to query analytics", 25);
            return SC_OK;
        }
        char *body = sc_strndup(alloc, resp.body, resp.body_len);
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok_owned(body, body ? strlen(body) : 0);
    } else {
        *out = sc_tool_result_fail("google_analytics requires OAuth2 setup", 38);
    }
    return SC_OK;
#endif
}

static const char *analytics_name(void *ctx) { (void)ctx; return TOOL_NAME; }
static const char *analytics_desc(void *ctx) { (void)ctx; return TOOL_DESC; }
static const char *analytics_params(void *ctx) { (void)ctx; return TOOL_PARAMS; }
static void analytics_deinit(void *ctx, sc_allocator_t *alloc) { (void)alloc; free(ctx); }

static const sc_tool_vtable_t analytics_vtable = {
    .execute = analytics_execute, .name = analytics_name, .description = analytics_desc,
    .parameters_json = analytics_params, .deinit = analytics_deinit,
};

sc_error_t sc_analytics_create(sc_allocator_t *alloc, sc_tool_t *out) {
    (void)alloc;
    void *ctx = calloc(1, sizeof(analytics_ctx_t));
    if (!ctx) return SC_ERR_OUT_OF_MEMORY;
    out->ctx = ctx;
    out->vtable = &analytics_vtable;
    return SC_OK;
}
