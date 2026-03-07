#include "seaclaw/tools/homeassistant.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
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

static sc_error_t homeassistant_execute(void *ctx, sc_allocator_t *alloc,
                                        const sc_json_value_t *args, sc_tool_result_t *out) {
    (void)ctx;
    if (!out)
        return SC_ERR_INVALID_ARGUMENT;
    if (!args) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *operation = sc_json_get_string(args, "operation");
    if (!operation) {
        *out = sc_tool_result_fail("missing operation", 16);
        return SC_OK;
    }

#if SC_IS_TEST
    if (strcmp(operation, "get_states") == 0) {
        const char *resp =
            "{\"states\":[{\"entity_id\":\"light.living_room\",\"state\":\"on\",\"attributes\":{"
            "\"brightness\":255}},{\"entity_id\":\"switch.plug\",\"state\":\"off\"}]}";
        *out = sc_tool_result_ok(resp, strlen(resp));
    } else if (strcmp(operation, "get_entity") == 0) {
        const char *entity_id = sc_json_get_string(args, "entity_id");
        char *msg = sc_sprintf(alloc,
                               "{\"entity_id\":\"%s\",\"state\":\"on\",\"attributes\":{"
                               "\"friendly_name\":\"Living Room Light\"}}",
                               entity_id ? entity_id : "light.living_room");
        *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else if (strcmp(operation, "call_service") == 0) {
        *out = sc_tool_result_ok("{\"success\":true}", 16);
    } else if (strcmp(operation, "fire_event") == 0) {
        *out = sc_tool_result_ok("{\"event_fired\":true}", 19);
    } else {
        *out = sc_tool_result_fail("unknown operation", 17);
    }
    return SC_OK;
#else
    const char *url = sc_json_get_string(args, "url");
    const char *token = sc_json_get_string(args, "token");
    if (!url || strlen(url) == 0 || !token || strlen(token) == 0) {
        *out = sc_tool_result_fail(
            "missing url or token — configure Home Assistant URL and long-lived access token", 72);
        return SC_OK;
    }
    if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) {
        *out = sc_tool_result_fail("url must use http:// or https://", 34);
        return SC_OK;
    }
    if (strlen(url) > 400) {
        *out = sc_tool_result_fail("url too long", 12);
        return SC_OK;
    }

    char auth[512];
    snprintf(auth, sizeof(auth), "Bearer %s", token);

    if (strcmp(operation, "get_states") == 0) {
        char api_url[512];
        snprintf(api_url, sizeof(api_url), "%s/api/states", url);
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_get(alloc, api_url, auth, &resp);
        if (err != SC_OK || resp.status_code != 200) {
            if (resp.owned && resp.body)
                sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to get states", 20);
            return SC_OK;
        }
        char *body = sc_strndup(alloc, resp.body, resp.body_len);
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok_owned(body, body ? strlen(body) : 0);
    } else if (strcmp(operation, "get_entity") == 0) {
        const char *entity_id = sc_json_get_string(args, "entity_id");
        if (!entity_id || strlen(entity_id) == 0) {
            *out = sc_tool_result_fail("get_entity requires entity_id", 28);
            return SC_OK;
        }
        if (strlen(entity_id) > 128) {
            *out = sc_tool_result_fail("entity_id too long", 18);
            return SC_OK;
        }
        char api_url[512];
        snprintf(api_url, sizeof(api_url), "%s/api/states/%s", url, entity_id);
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_get(alloc, api_url, auth, &resp);
        if (err != SC_OK || resp.status_code != 200) {
            if (resp.owned && resp.body)
                sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to get entity", 19);
            return SC_OK;
        }
        char *body = sc_strndup(alloc, resp.body, resp.body_len);
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok_owned(body, body ? strlen(body) : 0);
    } else if (strcmp(operation, "call_service") == 0) {
        const char *domain = sc_json_get_string(args, "domain");
        const char *service = sc_json_get_string(args, "service");
        const char *service_data = sc_json_get_string(args, "service_data");
        if (!domain || strlen(domain) == 0 || !service || strlen(service) == 0) {
            *out = sc_tool_result_fail("call_service requires domain and service", 39);
            return SC_OK;
        }
        char api_url[512];
        snprintf(api_url, sizeof(api_url), "%s/api/services/%s/%s", url, domain, service);
        const char *body_str = service_data && strlen(service_data) > 0 ? service_data : "{}";
        size_t body_len = strlen(body_str);
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_post_json(alloc, api_url, auth, body_str, body_len, &resp);
        if (err != SC_OK) {
            if (resp.owned && resp.body)
                sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to call service", 23);
            return SC_OK;
        }
        char *rbody = resp.body && resp.body_len > 0 ? sc_strndup(alloc, resp.body, resp.body_len)
                                                     : sc_strndup(alloc, "[]", 2);
        if (resp.owned && resp.body)
            sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_ok_owned(rbody, rbody ? strlen(rbody) : 0);
    } else if (strcmp(operation, "fire_event") == 0) {
        const char *event_type = sc_json_get_string(args, "event_type");
        const char *event_data = sc_json_get_string(args, "event_data");
        if (!event_type || strlen(event_type) == 0) {
            *out = sc_tool_result_fail("fire_event requires event_type", 30);
            return SC_OK;
        }
        char api_url[512];
        snprintf(api_url, sizeof(api_url), "%s/api/events/%s", url, event_type);
        const char *body_str = event_data && strlen(event_data) > 0 ? event_data : "{}";
        size_t body_len = strlen(body_str);
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_post_json(alloc, api_url, auth, body_str, body_len, &resp);
        if (err != SC_OK) {
            if (resp.owned && resp.body)
                sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("failed to fire event", 20);
            return SC_OK;
        }
        *out = sc_tool_result_ok("{\"event_fired\":true}", 19);
        if (resp.owned && resp.body)
            sc_http_response_free(alloc, &resp);
    } else {
        *out = sc_tool_result_fail("unknown operation", 17);
    }
    return SC_OK;
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
static void homeassistant_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    free(ctx);
}

static const sc_tool_vtable_t homeassistant_vtable = {
    .execute = homeassistant_execute,
    .name = homeassistant_name,
    .description = homeassistant_desc,
    .parameters_json = homeassistant_params,
    .deinit = homeassistant_deinit,
};

sc_error_t sc_homeassistant_create(sc_allocator_t *alloc, sc_tool_t *out) {
    (void)alloc;
    void *ctx = calloc(1, sizeof(homeassistant_ctx_t));
    if (!ctx)
        return SC_ERR_OUT_OF_MEMORY;
    out->ctx = ctx;
    out->vtable = &homeassistant_vtable;
    return SC_OK;
}
