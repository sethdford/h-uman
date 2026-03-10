#include "human/tools/composio.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMPOSIO_BASE_V2 "https://backend.composio.dev/api/v2"
#define COMPOSIO_BASE_V3 "https://backend.composio.dev/api/v3"

#define COMPOSIO_PARAMS                                                                        \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list\"," \
    "\"execute\",\"connect\"]},\"app\":{\"type\":\"string\"},\"action_name\":{\"type\":"       \
    "\"string\"},\"params\":{\"type\":\"string\"},\"entity_id\":{\"type\":\"string\"}},"       \
    "\"required\":[\"action\"]}"

typedef struct hu_composio_ctx {
    hu_allocator_t *alloc;
    char *api_key;
    char *entity_id;
} hu_composio_ctx_t;

static hu_error_t composio_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                   hu_tool_result_t *out) {
    hu_composio_ctx_t *c = (hu_composio_ctx_t *)ctx;
    if (!args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *action = hu_json_get_string(args, "action");
    if (!action || action[0] == '\0') {
        *out = hu_tool_result_fail("Missing 'action' parameter", 27);
        return HU_OK;
    }
#if HU_IS_TEST
    (void)c;
    if (strcmp(action, "list") == 0) {
        char *msg = hu_strndup(
            alloc, "[{\"name\":\"mock_action\",\"description\":\"Mock composio action\"}]", 52);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(msg, 52);
        return HU_OK;
    }
    if (strcmp(action, "execute") == 0) {
        char *msg = hu_strndup(alloc, "{\"status\":\"success\",\"result\":\"mock execution\"}", 43);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(msg, 43);
        return HU_OK;
    }
    if (strcmp(action, "connect") == 0) {
        char *msg = hu_strndup(alloc, "{\"status\":\"connected\",\"account_id\":\"mock-123\"}", 44);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(msg, 44);
        return HU_OK;
    }
    *out = hu_tool_result_fail("Unknown action", 14);
    return HU_OK;
#else
    if (!c->api_key || c->api_key[0] == '\0') {
        *out = hu_tool_result_fail("Composio API key not configured", 33);
        return HU_OK;
    }
    char x_api_key[320];
    int nk = snprintf(x_api_key, sizeof(x_api_key), "x-api-key: %s", c->api_key);
    if (nk < 0 || (size_t)nk >= sizeof(x_api_key)) {
        *out = hu_tool_result_fail("Composio API key too long", 24);
        return HU_OK;
    }

    if (strcmp(action, "list") == 0) {
        const char *app = hu_json_get_string(args, "app");
        char url[512];
        int nu = app && app[0]
                     ? snprintf(url, sizeof(url),
                                COMPOSIO_BASE_V3 "/tools?toolkits=%s&page=1&page_size=100", app)
                     : snprintf(url, sizeof(url), COMPOSIO_BASE_V3 "/tools?page=1&page_size=100");
        if (nu < 0 || (size_t)nu >= sizeof(url)) {
            *out = hu_tool_result_fail("URL too long", 12);
            return HU_OK;
        }
        hu_http_response_t resp = {0};
        hu_error_t err = hu_http_get_ex(alloc, url, x_api_key, &resp);
        if (err != HU_OK) {
            *out = hu_tool_result_fail("Composio list request failed", 27);
            return HU_OK;
        }
        if (resp.status_code >= 200 && resp.status_code < 300) {
            char *msg = resp.body ? hu_strndup(alloc, resp.body, resp.body_len)
                                  : hu_strndup(alloc, "[]", 2);
            hu_http_response_free(alloc, &resp);
            *out = msg ? hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0)
                       : hu_tool_result_fail("out of memory", 12);
            return HU_OK;
        }
        hu_http_response_free(alloc, &resp);
        /* Fallback to v2 */
        nu = app && app[0]
                 ? snprintf(url, sizeof(url), COMPOSIO_BASE_V2 "/actions?appNames=%s", app)
                 : snprintf(url, sizeof(url), COMPOSIO_BASE_V2 "/actions");
        if (nu < 0 || (size_t)nu >= sizeof(url)) {
            *out = hu_tool_result_fail("URL too long", 12);
            return HU_OK;
        }
        err = hu_http_get_ex(alloc, url, x_api_key, &resp);
        if (err != HU_OK) {
            *out = hu_tool_result_fail("Composio list request failed", 27);
            return HU_OK;
        }
        if (resp.status_code >= 200 && resp.status_code < 300) {
            char *msg = resp.body ? hu_strndup(alloc, resp.body, resp.body_len)
                                  : hu_strndup(alloc, "[]", 2);
            hu_http_response_free(alloc, &resp);
            *out = msg ? hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0)
                       : hu_tool_result_fail("out of memory", 12);
        } else {
            hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("Composio list failed", 20);
        }
        return HU_OK;
    }

    if (strcmp(action, "execute") == 0) {
        const char *action_name = hu_json_get_string(args, "action_name");
        if (!action_name || !action_name[0])
            action_name = hu_json_get_string(args, "tool_slug");
        if (!action_name || !action_name[0]) {
            *out = hu_tool_result_fail("Missing 'action_name' for execute", 32);
            return HU_OK;
        }
        const char *params_str = hu_json_get_string(args, "params");
        const char *arguments = (params_str && params_str[0]) ? params_str : "{}";
        size_t arguments_len = strlen(arguments);
        const char *eid = hu_json_get_string(args, "entity_id");
        if (!eid || !eid[0])
            eid = c->entity_id ? c->entity_id : "default";

        hu_json_buf_t body_buf;
        if (hu_json_buf_init(&body_buf, alloc) != HU_OK) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        if (hu_json_buf_append_raw(&body_buf, "{\"arguments\":", 13) != HU_OK)
            goto exec_fail;
        if (hu_json_buf_append_raw(&body_buf, arguments, arguments_len) != HU_OK)
            goto exec_fail;
        if (hu_json_buf_append_raw(&body_buf, ",\"user_id\":", 11) != HU_OK)
            goto exec_fail;
        if (hu_json_append_string(&body_buf, eid, strlen(eid)) != HU_OK)
            goto exec_fail;
        if (hu_json_buf_append_raw(&body_buf, "}", 1) != HU_OK)
            goto exec_fail;

        char url[512];
        int nu = snprintf(url, sizeof(url), COMPOSIO_BASE_V3 "/tools/%.128s/execute", action_name);
        if (nu < 0 || (size_t)nu >= sizeof(url)) {
            hu_json_buf_free(&body_buf);
            *out = hu_tool_result_fail("URL too long", 12);
            return HU_OK;
        }
        hu_http_response_t resp = {0};
        hu_error_t err =
            hu_http_post_json_ex(alloc, url, NULL, x_api_key, body_buf.ptr, body_buf.len, &resp);
        hu_json_buf_free(&body_buf);
        if (err != HU_OK) {
            *out = hu_tool_result_fail("Composio execute request failed", 30);
            return HU_OK;
        }
        if (resp.status_code >= 200 && resp.status_code < 300) {
            char *msg = resp.body ? hu_strndup(alloc, resp.body, resp.body_len)
                                  : hu_strndup(alloc, "{}", 2);
            hu_http_response_free(alloc, &resp);
            *out = msg ? hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0)
                       : hu_tool_result_fail("out of memory", 12);
        } else {
            hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("Composio execute failed", 23);
        }
        return HU_OK;
    exec_fail:
        hu_json_buf_free(&body_buf);
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }

    if (strcmp(action, "connect") == 0) {
        const char *eid = hu_json_get_string(args, "entity_id");
        if (!eid || !eid[0])
            eid = c->entity_id ? c->entity_id : "default";

        hu_json_buf_t body_buf;
        if (hu_json_buf_init(&body_buf, alloc) != HU_OK) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        if (hu_json_buf_append_raw(&body_buf, "{\"user_id\":", 11) != HU_OK)
            goto conn_fail;
        if (hu_json_append_string(&body_buf, eid, strlen(eid)) != HU_OK)
            goto conn_fail;
        if (hu_json_buf_append_raw(&body_buf, "}", 1) != HU_OK)
            goto conn_fail;

        const char *url = COMPOSIO_BASE_V3 "/connected_accounts/link";
        hu_http_response_t resp = {0};
        hu_error_t err =
            hu_http_post_json_ex(alloc, url, NULL, x_api_key, body_buf.ptr, body_buf.len, &resp);
        hu_json_buf_free(&body_buf);
        if (err != HU_OK) {
            *out = hu_tool_result_fail("Composio connect request failed", 31);
            return HU_OK;
        }
        if (resp.status_code >= 200 && resp.status_code < 300) {
            char *msg = resp.body ? hu_strndup(alloc, resp.body, resp.body_len)
                                  : hu_strndup(alloc, "{}", 2);
            hu_http_response_free(alloc, &resp);
            *out = msg ? hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0)
                       : hu_tool_result_fail("out of memory", 12);
        } else {
            hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("Composio connect failed", 22);
        }
        return HU_OK;
    conn_fail:
        hu_json_buf_free(&body_buf);
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }

    *out = hu_tool_result_fail("Unknown action", 14);
    return HU_OK;
#endif
}

static const char *composio_name(void *ctx) {
    (void)ctx;
    return "composio";
}
static const char *composio_desc(void *ctx) {
    (void)ctx;
    return "Interact with Composio: list tools, execute actions, or connect accounts.";
}
static const char *composio_params(void *ctx) {
    (void)ctx;
    return COMPOSIO_PARAMS;
}
static void composio_deinit(void *ctx, hu_allocator_t *alloc) {
    hu_composio_ctx_t *c = (hu_composio_ctx_t *)ctx;
    if (c && alloc) {
        if (c->api_key) {
            alloc->free(alloc->ctx, c->api_key, strlen(c->api_key) + 1);
        }
        if (c->entity_id) {
            alloc->free(alloc->ctx, c->entity_id, strlen(c->entity_id) + 1);
        }
        alloc->free(alloc->ctx, c, sizeof(*c));
    }
}

static const hu_tool_vtable_t composio_vtable = {
    .execute = composio_execute,
    .name = composio_name,
    .description = composio_desc,
    .parameters_json = composio_params,
    .deinit = composio_deinit,
};

hu_error_t hu_composio_create(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                              const char *entity_id, size_t entity_id_len, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_composio_ctx_t *c = (hu_composio_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    if (api_key && api_key_len > 0) {
        c->api_key = (char *)alloc->alloc(alloc->ctx, api_key_len + 1);
        if (!c->api_key) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->api_key, api_key, api_key_len);
        c->api_key[api_key_len] = '\0';
    }
    if (entity_id && entity_id_len > 0) {
        c->entity_id = (char *)alloc->alloc(alloc->ctx, entity_id_len + 1);
        if (!c->entity_id) {
            if (c->api_key)
                alloc->free(alloc->ctx, c->api_key, api_key_len + 1);
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(c->entity_id, entity_id, entity_id_len);
        c->entity_id[entity_id_len] = '\0';
    }
    out->ctx = c;
    out->vtable = &composio_vtable;
    return HU_OK;
}
