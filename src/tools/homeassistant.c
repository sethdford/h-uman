#include "human/tools/homeassistant.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOOL_NAME "homeassistant"
#define TOOL_DESC "Control Home Assistant smart home devices"
#define TOOL_PARAMS                                                                           \
    "{\"type\":\"object\",\"properties\":{\"operation\":{\"type\":\"string\",\"enum\":["      \
    "\"get_states\",\"get_entity\",\"call_service\",\"fire_event\"]},\"entity_id\":{"         \
    "\"type\":\"string\",\"description\":\"Entity ID (e.g. light.living_room)\"},\"domain\":" \
    "{\"type\":\"string\",\"description\":\"Domain for call_service (e.g. light, switch)\"}," \
    "\"service\":{\"type\":\"string\",\"description\":\"Service name for call_service\"},"    \
    "\"service_data\":{\"type\":\"string\",\"description\":\"JSON data for call_service\"},"  \
    "\"event_type\":{\"type\":\"string\",\"description\":\"Event type for fire_event\"},"     \
    "\"event_data\":{\"type\":\"string\",\"description\":\"JSON data for fire_event\"},"      \
    "\"url\":{\"type\":\"string\",\"description\":\"Home Assistant URL (e.g. "                \
    "http://localhost:8123)\"},\"token\":{\"type\":\"string\",\"description\":\"Long-lived "  \
    "access token\"}},\"required\":[\"operation\"]}"

typedef struct {
    char _unused;
} homeassistant_ctx_t;

static hu_error_t homeassistant_execute(void *ctx, hu_allocator_t *alloc,
                                        const hu_json_value_t *args, hu_tool_result_t *out) {
    (void)ctx;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!args) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *operation = hu_json_get_string(args, "operation");
    if (!operation) {
        *out = hu_tool_result_fail("missing operation", 16);
        return HU_OK;
    }

#if HU_IS_TEST
    if (strcmp(operation, "get_states") == 0) {
        const char *resp =
            "{\"states\":[{\"entity_id\":\"light.living_room\",\"state\":\"on\",\"attributes\":{"
            "\"brightness\":255}},{\"entity_id\":\"switch.plug\",\"state\":\"off\"}]}";
        *out = hu_tool_result_ok(resp, strlen(resp));
    } else if (strcmp(operation, "get_entity") == 0) {
        const char *entity_id = hu_json_get_string(args, "entity_id");
        char *msg = hu_sprintf(alloc,
                               "{\"entity_id\":\"%s\",\"state\":\"on\",\"attributes\":{"
                               "\"friendly_name\":\"Living Room Light\"}}",
                               entity_id ? entity_id : "light.living_room");
        *out = hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else if (strcmp(operation, "call_service") == 0) {
        *out = hu_tool_result_ok("{\"success\":true}", 16);
    } else if (strcmp(operation, "fire_event") == 0) {
        *out = hu_tool_result_ok("{\"event_fired\":true}", 19);
    } else {
        *out = hu_tool_result_fail("unknown operation", 17);
    }
    return HU_OK;
#else
    const char *url = hu_json_get_string(args, "url");
    const char *token = hu_json_get_string(args, "token");
    if (!url || strlen(url) == 0 || !token || strlen(token) == 0) {
        *out = hu_tool_result_fail(
            "missing url or token — configure Home Assistant URL and long-lived access token", 72);
        return HU_OK;
    }
    if (strncmp(url, "https://", 8) != 0) {
        if (strncmp(url, "http://localhost", 16) == 0 ||
            strncmp(url, "http://127.0.0.1", 16) == 0 || strncmp(url, "http://[::1]", 12) == 0) {
            /* Allow HTTP for local Home Assistant instances only */
        } else {
            *out = hu_tool_result_fail("url must use https:// (http:// only allowed for localhost)",
                                       58);
            return HU_OK;
        }
    }
    if (strlen(url) > 400) {
        *out = hu_tool_result_fail("url too long", 12);
        return HU_OK;
    }

    if (strlen(token) > 500) {
        *out = hu_tool_result_fail("token too long", 14);
        return HU_OK;
    }
    char auth[512];
    int auth_n = snprintf(auth, sizeof(auth), "Bearer %s", token);
    if (auth_n < 0 || (size_t)auth_n >= sizeof(auth)) {
        *out = hu_tool_result_fail("token too long", 14);
        return HU_OK;
    }

    if (strcmp(operation, "get_states") == 0) {
        char api_url[512];
        int url_n = snprintf(api_url, sizeof(api_url), "%s/api/states", url);
        if (url_n < 0 || (size_t)url_n >= sizeof(api_url)) {
            *out = hu_tool_result_fail("url too long", 12);
            return HU_OK;
        }
        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_get(alloc, api_url, auth, &resp);
        if (err != HU_OK || resp.status_code != 200) {
            if (resp.owned && resp.body)
                hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("failed to get states", 20);
            return HU_OK;
        }
        char *body = hu_strndup(alloc, resp.body, resp.body_len);
        hu_http_response_free(alloc, &resp);
        *out = hu_tool_result_ok_owned(body, body ? strlen(body) : 0);
    } else if (strcmp(operation, "get_entity") == 0) {
        const char *entity_id = hu_json_get_string(args, "entity_id");
        if (!entity_id || strlen(entity_id) == 0) {
            *out = hu_tool_result_fail("get_entity requires entity_id", 28);
            return HU_OK;
        }
        if (strlen(entity_id) > 128) {
            *out = hu_tool_result_fail("entity_id too long", 18);
            return HU_OK;
        }
        if (strchr(entity_id, '/') || strstr(entity_id, "..")) {
            *out = hu_tool_result_fail("invalid entity_id", 17);
            return HU_OK;
        }
        char api_url[512];
        int eid_n = snprintf(api_url, sizeof(api_url), "%s/api/states/%s", url, entity_id);
        if (eid_n < 0 || (size_t)eid_n >= sizeof(api_url)) {
            *out = hu_tool_result_fail("url too long", 12);
            return HU_OK;
        }
        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_get(alloc, api_url, auth, &resp);
        if (err != HU_OK || resp.status_code != 200) {
            if (resp.owned && resp.body)
                hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("failed to get entity", 19);
            return HU_OK;
        }
        char *body = hu_strndup(alloc, resp.body, resp.body_len);
        hu_http_response_free(alloc, &resp);
        *out = hu_tool_result_ok_owned(body, body ? strlen(body) : 0);
    } else if (strcmp(operation, "call_service") == 0) {
        const char *domain = hu_json_get_string(args, "domain");
        const char *service = hu_json_get_string(args, "service");
        const char *service_data = hu_json_get_string(args, "service_data");
        if (!domain || strlen(domain) == 0 || !service || strlen(service) == 0) {
            *out = hu_tool_result_fail("call_service requires domain and service", 39);
            return HU_OK;
        }
        char api_url[512];
        snprintf(api_url, sizeof(api_url), "%s/api/services/%s/%s", url, domain, service);
        const char *body_str = service_data && strlen(service_data) > 0 ? service_data : "{}";
        size_t body_len = strlen(body_str);
        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_post_json(alloc, api_url, auth, body_str, body_len, &resp);
        if (err != HU_OK) {
            if (resp.owned && resp.body)
                hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("failed to call service", 23);
            return HU_OK;
        }
        char *rbody = resp.body && resp.body_len > 0 ? hu_strndup(alloc, resp.body, resp.body_len)
                                                     : hu_strndup(alloc, "[]", 2);
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        *out = hu_tool_result_ok_owned(rbody, rbody ? strlen(rbody) : 0);
    } else if (strcmp(operation, "fire_event") == 0) {
        const char *event_type = hu_json_get_string(args, "event_type");
        const char *event_data = hu_json_get_string(args, "event_data");
        if (!event_type || strlen(event_type) == 0) {
            *out = hu_tool_result_fail("fire_event requires event_type", 30);
            return HU_OK;
        }
        char api_url[512];
        snprintf(api_url, sizeof(api_url), "%s/api/events/%s", url, event_type);
        const char *body_str = event_data && strlen(event_data) > 0 ? event_data : "{}";
        size_t body_len = strlen(body_str);
        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_post_json(alloc, api_url, auth, body_str, body_len, &resp);
        if (err != HU_OK) {
            if (resp.owned && resp.body)
                hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("failed to fire event", 20);
            return HU_OK;
        }
        *out = hu_tool_result_ok("{\"event_fired\":true}", 19);
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
    } else {
        *out = hu_tool_result_fail("unknown operation", 17);
    }
    return HU_OK;
#endif
}

static const char *homeassistant_name(void *ctx) {
    (void)ctx;
    return TOOL_NAME;
}
static const char *homeassistant_desc(void *ctx) {
    (void)ctx;
    return TOOL_DESC;
}
static const char *homeassistant_params(void *ctx) {
    (void)ctx;
    return TOOL_PARAMS;
}
static void homeassistant_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, sizeof(homeassistant_ctx_t));
}

static const hu_tool_vtable_t homeassistant_vtable = {
    .execute = homeassistant_execute,
    .name = homeassistant_name,
    .description = homeassistant_desc,
    .parameters_json = homeassistant_params,
    .deinit = homeassistant_deinit,
};

hu_error_t hu_homeassistant_create(hu_allocator_t *alloc, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    void *ctx = alloc->alloc(alloc->ctx, sizeof(homeassistant_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(homeassistant_ctx_t));
    out->ctx = ctx;
    out->vtable = &homeassistant_vtable;
    return HU_OK;
}
