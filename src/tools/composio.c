#include "seaclaw/tool.h"
#include "seaclaw/tools/composio.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define COMPOSIO_BASE_V2 "https://backend.composio.dev/api/v2"
#define COMPOSIO_BASE_V3 "https://backend.composio.dev/api/v3"

#define COMPOSIO_PARAMS "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list\",\"execute\",\"connect\"]},\"app\":{\"type\":\"string\"},\"action_name\":{\"type\":\"string\"},\"params\":{\"type\":\"string\"},\"entity_id\":{\"type\":\"string\"}},\"required\":[\"action\"]}"

typedef struct sc_composio_ctx {
    sc_allocator_t *alloc;
    char *api_key;
    char *entity_id;
} sc_composio_ctx_t;

static sc_error_t composio_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args, sc_tool_result_t *out)
{
    sc_composio_ctx_t *c = (sc_composio_ctx_t *)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *action = sc_json_get_string(args, "action");
    if (!action || action[0] == '\0') {
        *out = sc_tool_result_fail("Missing 'action' parameter", 27);
        return SC_OK;
    }
#if SC_IS_TEST
    (void)c;
    if (strcmp(action, "list") == 0) {
        char *msg = sc_strndup(alloc, "[{\"name\":\"mock_action\",\"description\":\"Mock composio action\"}]", 52);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, 52);
        return SC_OK;
    }
    if (strcmp(action, "execute") == 0) {
        char *msg = sc_strndup(alloc, "{\"status\":\"success\",\"result\":\"mock execution\"}", 43);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, 43);
        return SC_OK;
    }
    if (strcmp(action, "connect") == 0) {
        char *msg = sc_strndup(alloc, "{\"status\":\"connected\",\"account_id\":\"mock-123\"}", 44);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, 44);
        return SC_OK;
    }
    *out = sc_tool_result_fail("Unknown action", 14);
    return SC_OK;
#else
    if (!c->api_key || c->api_key[0] == '\0') {
        *out = sc_tool_result_fail("Composio API key not configured", 33);
        return SC_OK;
    }
    char x_api_key[320];
    int nk = snprintf(x_api_key, sizeof(x_api_key), "x-api-key: %s", c->api_key);
    if (nk < 0 || (size_t)nk >= sizeof(x_api_key)) {
        *out = sc_tool_result_fail("Composio API key too long", 24);
        return SC_OK;
    }

    if (strcmp(action, "list") == 0) {
        const char *app = sc_json_get_string(args, "app");
        char url[512];
        int nu = app && app[0]
            ? snprintf(url, sizeof(url), COMPOSIO_BASE_V3 "/tools?toolkits=%s&page=1&page_size=100", app)
            : snprintf(url, sizeof(url), COMPOSIO_BASE_V3 "/tools?page=1&page_size=100");
        if (nu < 0 || (size_t)nu >= sizeof(url)) {
            *out = sc_tool_result_fail("URL too long", 12);
            return SC_OK;
        }
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_get_ex(alloc, url, x_api_key, &resp);
        if (err != SC_OK) {
            *out = sc_tool_result_fail("Composio list request failed", 27);
            return SC_OK;
        }
        if (resp.status_code >= 200 && resp.status_code < 300) {
            char *msg = resp.body ? sc_strndup(alloc, resp.body, resp.body_len) : sc_strndup(alloc, "[]", 2);
            sc_http_response_free(alloc, &resp);
            *out = msg ? sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0) : sc_tool_result_fail("out of memory", 12);
            return SC_OK;
        }
        sc_http_response_free(alloc, &resp);
        /* Fallback to v2 */
        nu = app && app[0]
            ? snprintf(url, sizeof(url), COMPOSIO_BASE_V2 "/actions?appNames=%s", app)
            : snprintf(url, sizeof(url), COMPOSIO_BASE_V2 "/actions");
        if (nu < 0 || (size_t)nu >= sizeof(url)) {
            *out = sc_tool_result_fail("URL too long", 12);
            return SC_OK;
        }
        err = sc_http_get_ex(alloc, url, x_api_key, &resp);
        if (err != SC_OK) {
            *out = sc_tool_result_fail("Composio list request failed", 27);
            return SC_OK;
        }
        if (resp.status_code >= 200 && resp.status_code < 300) {
            char *msg = resp.body ? sc_strndup(alloc, resp.body, resp.body_len) : sc_strndup(alloc, "[]", 2);
            sc_http_response_free(alloc, &resp);
            *out = msg ? sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0) : sc_tool_result_fail("out of memory", 12);
        } else {
            sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("Composio list failed", 20);
        }
        return SC_OK;
    }

    if (strcmp(action, "execute") == 0) {
        const char *action_name = sc_json_get_string(args, "action_name");
        if (!action_name || !action_name[0]) action_name = sc_json_get_string(args, "tool_slug");
        if (!action_name || !action_name[0]) {
            *out = sc_tool_result_fail("Missing 'action_name' for execute", 32);
            return SC_OK;
        }
        const char *params_str = sc_json_get_string(args, "params");
        const char *arguments = (params_str && params_str[0]) ? params_str : "{}";
        size_t arguments_len = strlen(arguments);
        const char *eid = sc_json_get_string(args, "entity_id");
        if (!eid || !eid[0]) eid = c->entity_id ? c->entity_id : "default";

        sc_json_buf_t body_buf;
        if (sc_json_buf_init(&body_buf, alloc) != SC_OK) {
            *out = sc_tool_result_fail("out of memory", 12);
            return SC_ERR_OUT_OF_MEMORY;
        }
        if (sc_json_buf_append_raw(&body_buf, "{\"arguments\":", 12) != SC_OK) goto exec_fail;
        if (sc_json_buf_append_raw(&body_buf, arguments, arguments_len) != SC_OK) goto exec_fail;
        if (sc_json_buf_append_raw(&body_buf, ",\"user_id\":", 10) != SC_OK) goto exec_fail;
        if (sc_json_append_string(&body_buf, eid, strlen(eid)) != SC_OK) goto exec_fail;
        if (sc_json_buf_append_raw(&body_buf, "}", 1) != SC_OK) goto exec_fail;

        char url[512];
        int nu = snprintf(url, sizeof(url), COMPOSIO_BASE_V3 "/tools/%.128s/execute", action_name);
        if (nu < 0 || (size_t)nu >= sizeof(url)) { sc_json_buf_free(&body_buf); *out = sc_tool_result_fail("URL too long", 12); return SC_OK; }
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_post_json_ex(alloc, url, NULL, x_api_key, body_buf.ptr, body_buf.len, &resp);
        sc_json_buf_free(&body_buf);
        if (err != SC_OK) {
            *out = sc_tool_result_fail("Composio execute request failed", 30);
            return SC_OK;
        }
        if (resp.status_code >= 200 && resp.status_code < 300) {
            char *msg = resp.body ? sc_strndup(alloc, resp.body, resp.body_len) : sc_strndup(alloc, "{}", 2);
            sc_http_response_free(alloc, &resp);
            *out = msg ? sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0) : sc_tool_result_fail("out of memory", 12);
        } else {
            sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("Composio execute failed", 23);
        }
        return SC_OK;
    exec_fail:
        sc_json_buf_free(&body_buf);
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }

    if (strcmp(action, "connect") == 0) {
        const char *eid = sc_json_get_string(args, "entity_id");
        if (!eid || !eid[0]) eid = c->entity_id ? c->entity_id : "default";

        sc_json_buf_t body_buf;
        if (sc_json_buf_init(&body_buf, alloc) != SC_OK) {
            *out = sc_tool_result_fail("out of memory", 12);
            return SC_ERR_OUT_OF_MEMORY;
        }
        if (sc_json_buf_append_raw(&body_buf, "{\"user_id\":", 10) != SC_OK) goto conn_fail;
        if (sc_json_append_string(&body_buf, eid, strlen(eid)) != SC_OK) goto conn_fail;
        if (sc_json_buf_append_raw(&body_buf, "}", 1) != SC_OK) goto conn_fail;

        const char *url = COMPOSIO_BASE_V3 "/connected_accounts/link";
        sc_http_response_t resp = {0};
        sc_error_t err = sc_http_post_json_ex(alloc, url, NULL, x_api_key, body_buf.ptr, body_buf.len, &resp);
        sc_json_buf_free(&body_buf);
        if (err != SC_OK) {
            *out = sc_tool_result_fail("Composio connect request failed", 31);
            return SC_OK;
        }
        if (resp.status_code >= 200 && resp.status_code < 300) {
            char *msg = resp.body ? sc_strndup(alloc, resp.body, resp.body_len) : sc_strndup(alloc, "{}", 2);
            sc_http_response_free(alloc, &resp);
            *out = msg ? sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0) : sc_tool_result_fail("out of memory", 12);
        } else {
            sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_fail("Composio connect failed", 22);
        }
        return SC_OK;
    conn_fail:
        sc_json_buf_free(&body_buf);
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }

    *out = sc_tool_result_fail("Unknown action", 14);
    return SC_OK;
#endif
}

static const char *composio_name(void *ctx) { (void)ctx; return "composio"; }
static const char *composio_desc(void *ctx) {
    (void)ctx;
    return "Interact with Composio: list tools, execute actions, or connect accounts.";
}
static const char *composio_params(void *ctx) { (void)ctx; return COMPOSIO_PARAMS; }
static void composio_deinit(void *ctx, sc_allocator_t *alloc) {
    sc_composio_ctx_t *c = (sc_composio_ctx_t *)ctx;
    if (c) {
        if (c->api_key) { alloc->free(alloc->ctx, c->api_key, strlen(c->api_key) + 1); }
        if (c->entity_id) { alloc->free(alloc->ctx, c->entity_id, strlen(c->entity_id) + 1); }
    }
    free(ctx);
}

static const sc_tool_vtable_t composio_vtable = {
    .execute = composio_execute, .name = composio_name,
    .description = composio_desc, .parameters_json = composio_params,
    .deinit = composio_deinit,
};

sc_error_t sc_composio_create(sc_allocator_t *alloc,
    const char *api_key, size_t api_key_len,
    const char *entity_id, size_t entity_id_len,
    sc_tool_t *out)
{
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    sc_composio_ctx_t *c = (sc_composio_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    if (api_key && api_key_len > 0) {
        c->api_key = (char *)alloc->alloc(alloc->ctx, api_key_len + 1);
        if (!c->api_key) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        memcpy(c->api_key, api_key, api_key_len);
        c->api_key[api_key_len] = '\0';
    }
    if (entity_id && entity_id_len > 0) {
        c->entity_id = (char *)alloc->alloc(alloc->ctx, entity_id_len + 1);
        if (!c->entity_id) {
            if (c->api_key) alloc->free(alloc->ctx, c->api_key, api_key_len + 1);
            free(c);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->entity_id, entity_id, entity_id_len);
        c->entity_id[entity_id_len] = '\0';
    }
    out->ctx = c;
    out->vtable = &composio_vtable;
    return SC_OK;
}
