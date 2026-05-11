#include "human/core/log.h"
#include "human/mcp_manager.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/mcp.h"
#include "human/mcp_jsonrpc.h"
#include "human/mcp_transport.h"
#include "human/oauth.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal server slot ─────────────────────────────────────────────── */

typedef struct hu_mcp_mgr_slot {
    char *name;
    hu_mcp_client_t *server;
    hu_mcp_client_config_t config;
    hu_mcp_transport_t transport;  /* For HTTP/SSE transports */
    char *transport_type;          /* "stdio", "http", or "sse" */
    char *url;                     /* For HTTP/SSE transports */
    bool connected;
    bool auto_connect;
    uint32_t timeout_ms;
    size_t tool_count;
    /* OAuth2 PKCE authentication (optional) */
    char *oauth_client_id;
    char *oauth_auth_url;
    char *oauth_token_url;
    char *oauth_scopes;
    char *oauth_redirect_uri;
    hu_oauth_token_t oauth_token;  /* Cached token */
} hu_mcp_mgr_slot_t;

struct hu_mcp_manager {
    hu_allocator_t *alloc;
    hu_mcp_mgr_slot_t slots[HU_MCP_MANAGER_MAX_SERVERS];
    size_t slot_count;
    uint32_t next_rpc_id;  /* For JSON-RPC request IDs - replaces static variable */
};

/* ── Helpers ──────────────────────────────────────────────────────────── */

static char *dup_str(hu_allocator_t *alloc, const char *s) {
    if (!s)
        return NULL;
    size_t len = strlen(s);
    char *d = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (d)
        memcpy(d, s, len + 1);
    return d;
}

static void free_str(hu_allocator_t *alloc, char *s) {
    if (s)
        alloc->free(alloc->ctx, s, strlen(s) + 1);
}

static void slot_destroy(hu_allocator_t *alloc, hu_mcp_mgr_slot_t *slot) {
    if (slot->server)
        hu_mcp_client_destroy(slot->server);
    if (slot->transport.ctx || slot->transport.send || slot->transport.recv || slot->transport.close)
        hu_mcp_transport_destroy(&slot->transport, alloc);
    free_str(alloc, slot->name);
    free_str(alloc, slot->transport_type);
    free_str(alloc, slot->url);
    /* Free OAuth fields */
    free_str(alloc, slot->oauth_client_id);
    free_str(alloc, slot->oauth_auth_url);
    free_str(alloc, slot->oauth_token_url);
    free_str(alloc, slot->oauth_scopes);
    free_str(alloc, slot->oauth_redirect_uri);
    hu_mcp_oauth_token_free(alloc, &slot->oauth_token);
    memset(slot, 0, sizeof(*slot));
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

hu_error_t hu_mcp_manager_create(hu_allocator_t *alloc,
                                 const struct hu_mcp_server_entry *entries, size_t count,
                                 hu_mcp_manager_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;

    hu_mcp_manager_t *mgr =
        (hu_mcp_manager_t *)alloc->alloc(alloc->ctx, sizeof(hu_mcp_manager_t));
    if (!mgr)
        return HU_ERR_OUT_OF_MEMORY;
    memset(mgr, 0, sizeof(*mgr));
    mgr->alloc = alloc;
    mgr->next_rpc_id = 1;  /* Initialize RPC ID counter */

    if (!entries || count == 0) {
        *out = mgr;
        return HU_OK;
    }

    size_t n = count < HU_MCP_MANAGER_MAX_SERVERS ? count : HU_MCP_MANAGER_MAX_SERVERS;
    for (size_t i = 0; i < n; i++) {
        const struct hu_mcp_server_entry *e = &entries[i];
        if (!e->name)
            continue;

        /* Determine transport type and validate required fields */
        const char *transport_type = e->transport_type ? e->transport_type : "stdio";
        bool is_stdio = (strcmp(transport_type, "stdio") == 0);
        bool is_http = (strcmp(transport_type, "http") == 0);
        bool is_sse = (strcmp(transport_type, "sse") == 0);

        if (!is_stdio && !is_http && !is_sse) {
            hu_log_info("mcp-manager", NULL, "mcp_manager: invalid transport_type '%s' for server '%s'",
                    transport_type, e->name);
            continue;
        }

        if (is_stdio && !e->command)
            continue;  /* stdio requires command */
        if ((is_http || is_sse) && !e->url)
            continue;  /* http/sse require url */

        hu_mcp_mgr_slot_t *slot = &mgr->slots[mgr->slot_count];
        slot->name = dup_str(alloc, e->name);
        if (!slot->name)
            goto fail;

        slot->transport_type = dup_str(alloc, transport_type);
        if (!slot->transport_type) {
            free_str(alloc, slot->name);
            goto fail;
        }

        if (is_stdio) {
            hu_mcp_client_config_t cfg = {
                .command = e->command,
                .args = (const char **)e->args,
                .args_count = e->args_count,
            };
            slot->config = cfg;
            slot->server = NULL;
        } else {
            /* For HTTP/SSE, store the URL */
            slot->url = dup_str(alloc, e->url);
            if (!slot->url) {
                free_str(alloc, slot->transport_type);
                free_str(alloc, slot->name);
                goto fail;
            }
            memset(&slot->config, 0, sizeof(slot->config));
            slot->server = NULL;
        }

        memset(&slot->transport, 0, sizeof(slot->transport));
        slot->auto_connect = e->auto_connect;
        slot->timeout_ms = e->timeout_ms > 0 ? e->timeout_ms : HU_MCP_MANAGER_DEFAULT_TIMEOUT;
        slot->connected = false;
        slot->tool_count = 0;

        /* Copy OAuth2 config (if present) */
        slot->oauth_client_id = dup_str(alloc, e->oauth_client_id);
        slot->oauth_auth_url = dup_str(alloc, e->oauth_auth_url);
        slot->oauth_token_url = dup_str(alloc, e->oauth_token_url);
        slot->oauth_scopes = dup_str(alloc, e->oauth_scopes);
        slot->oauth_redirect_uri = dup_str(alloc, e->oauth_redirect_uri);
        memset(&slot->oauth_token, 0, sizeof(slot->oauth_token));

        mgr->slot_count++;
    }

    *out = mgr;
    return HU_OK;

fail:
    for (size_t i = 0; i < mgr->slot_count; i++)
        slot_destroy(alloc, &mgr->slots[i]);
    alloc->free(alloc->ctx, mgr, sizeof(*mgr));
    return HU_ERR_OUT_OF_MEMORY;
}

void hu_mcp_manager_destroy(hu_mcp_manager_t *mgr) {
    if (!mgr)
        return;
    hu_allocator_t *alloc = mgr->alloc;
    for (size_t i = 0; i < mgr->slot_count; i++)
        slot_destroy(alloc, &mgr->slots[i]);
    alloc->free(alloc->ctx, mgr, sizeof(*mgr));
}

/* ── OAuth Token Loading ───────────────────────────────────────────────── */

/**
 * Load cached OAuth token for this slot (if configured).
 * Logs message if token is needed but not found or expired.
 * Returns HU_OK even if token loading fails (graceful degradation).
 */
static void slot_load_oauth_token(hu_allocator_t *alloc, hu_mcp_mgr_slot_t *slot) {
    if (!slot->oauth_client_id || !slot->oauth_token_url)
        return;  /* OAuth not configured for this server */

    /* Try to load cached token from ~/.human/oauth_tokens.json */
    const char *home = getenv("HOME");
    if (!home)
        home = ".";

    /* Build path: ~/.human/oauth_tokens.json */
    char token_path[512];
    int written = snprintf(token_path, sizeof(token_path), "%s/.human/oauth_tokens.json", home);
    if (written < 0 || (size_t)written >= sizeof(token_path)) {
        hu_log_info("mcp-manager", NULL, "mcp_manager: token path too long for server '%s'", slot->name);
        return;
    }

    hu_error_t err = hu_mcp_oauth_token_load(alloc, token_path, slot->name, &slot->oauth_token);
    if (err == HU_OK) {
        if (hu_mcp_oauth_token_is_expired(&slot->oauth_token)) {
            hu_log_info("mcp-manager", NULL, "mcp_manager: OAuth token for '%s' has expired. "
                    "Please re-authenticate.",
                    slot->name);
            hu_mcp_oauth_token_free(alloc, &slot->oauth_token);
        } else {
            hu_log_info("mcp-manager", NULL, "mcp_manager: loaded cached OAuth token for '%s' (valid until %lld)",
                    slot->name, (long long)slot->oauth_token.expires_at);
        }
    } else if (err == HU_ERR_NOT_FOUND) {
        hu_log_info("mcp-manager", NULL, "mcp_manager: no cached OAuth token for '%s'. "
                "Please authenticate via: human oauth %s",
                slot->name, slot->name);
    } else {
        hu_log_error("mcp-manager", NULL, "mcp_manager: failed to load OAuth token for '%s': %d", slot->name,
                (int)err);
    }
}

/* ── Connection ───────────────────────────────────────────────────────── */

static hu_error_t connect_slot(hu_allocator_t *alloc, hu_mcp_mgr_slot_t *slot) {
    if (slot->connected)
        return HU_OK;

    /* Handle stdio transport (traditional MCP server) */
    if (!slot->transport_type || strcmp(slot->transport_type, "stdio") == 0) {
        if (slot->server)
            return HU_OK;

        hu_mcp_client_t *srv = hu_mcp_client_create(alloc, &slot->config);
        if (!srv)
            return HU_ERR_IO;

        hu_error_t err = hu_mcp_client_connect(srv);
        if (err != HU_OK) {
            hu_mcp_client_destroy(srv);
            return err;
        }

        slot->server = srv;
        slot->connected = true;
        return HU_OK;
    }

    /* Handle HTTP transport */
    if (strcmp(slot->transport_type, "http") == 0) {
        if (!slot->url)
            return HU_ERR_INVALID_ARGUMENT;

        /* Load OAuth token if configured */
        slot_load_oauth_token(alloc, slot);

        hu_error_t err =
            hu_mcp_transport_http_create(alloc, slot->url, strlen(slot->url), &slot->transport);
        if (err != HU_OK)
            return err;

        /* Note: OAuth token is passed per-request in mgr_tool_execute() via auth_header.
           Transport layer passes auth via hu_http_post_json(). */

        slot->connected = true;
        return HU_OK;
    }

    /* Handle SSE transport */
    if (strcmp(slot->transport_type, "sse") == 0) {
        if (!slot->url)
            return HU_ERR_INVALID_ARGUMENT;

        /* Load OAuth token if configured */
        slot_load_oauth_token(alloc, slot);

        hu_error_t err =
            hu_mcp_transport_sse_create(alloc, slot->url, strlen(slot->url), &slot->transport);
        if (err != HU_OK)
            return err;

        /* Note: OAuth token is passed per-request in mgr_tool_execute() via auth_header.
           Transport layer passes auth via hu_http_post_json(). */

        slot->connected = true;
        return HU_OK;
    }

    return HU_ERR_INVALID_ARGUMENT;
}

hu_error_t hu_mcp_manager_connect_auto(hu_mcp_manager_t *mgr) {
    if (!mgr)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < mgr->slot_count; i++) {
        if (!mgr->slots[i].auto_connect)
            continue;
        hu_error_t err = connect_slot(mgr->alloc, &mgr->slots[i]);
        if (err != HU_OK)
            hu_log_error("mcp-manager", NULL, "mcp_manager: failed to connect server '%s': %d",
                    mgr->slots[i].name ? mgr->slots[i].name : "?", (int)err);
    }
    return HU_OK;
}

hu_error_t hu_mcp_manager_connect_server(hu_mcp_manager_t *mgr, const char *server_name) {
    if (!mgr || !server_name)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < mgr->slot_count; i++) {
        if (mgr->slots[i].name && strcmp(mgr->slots[i].name, server_name) == 0)
            return connect_slot(mgr->alloc, &mgr->slots[i]);
    }
    return HU_ERR_NOT_FOUND;
}

/* ── Tool wrapper ─────────────────────────────────────────────────────── */

typedef struct hu_mcp_mgr_tool_wrapper {
    hu_allocator_t *alloc;
    hu_mcp_manager_t *mgr;
    size_t slot_index;
    char *original_name;   /* tool name on the server */
    char *prefixed_name;   /* mcp__<server>__<tool> */
    char *desc;
    char *params_json;
} hu_mcp_mgr_tool_wrapper_t;

static hu_error_t mgr_tool_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                   hu_tool_result_t *out) {
    hu_mcp_mgr_tool_wrapper_t *w = (hu_mcp_mgr_tool_wrapper_t *)ctx;
    if (!w || !w->mgr || w->slot_index >= w->mgr->slot_count) {
        *out = hu_tool_result_fail("MCP manager tool wrapper invalid", 33);
        return HU_OK;
    }

    hu_mcp_mgr_slot_t *slot = &w->mgr->slots[w->slot_index];
    if (!slot->connected) {
        *out = hu_tool_result_fail("MCP server not connected", 24);
        return HU_OK;
    }

    /* Prepare args_json for either transport or stdio */
    char *args_json = NULL;
    size_t args_len = 0;
    bool args_allocated = false;
    if (args && args->type == HU_JSON_OBJECT) {
        hu_error_t err = hu_json_stringify(alloc, args, &args_json, &args_len);
        if (err != HU_OK) {
            *out = hu_tool_result_fail("Failed to serialize args", 24);
            return HU_OK;
        }
        args_allocated = true;
    }
    if (!args_json)
        args_json = "{}";

    if (!slot->server) {
        /* Transport-based (HTTP/SSE) tool execution */
#if HU_ENABLE_CURL
        /* Build JSON-RPC request for tools/call */
        char *request = NULL;
        size_t request_len = 0;
        uint32_t rpc_id = w->mgr->next_rpc_id++;
        hu_error_t jerr = hu_mcp_jsonrpc_build_tools_call(alloc, rpc_id,
            w->original_name, args_json, &request, &request_len);
        if (jerr != HU_OK) {
            *out = hu_tool_result_fail("Failed to build JSON-RPC request", 32);
            return HU_OK;
        }

        /* Build auth header if OAuth token available and valid */
        char auth_buf[2048] = {0};
        const char *auth_header = NULL;
        if (slot->oauth_token.access_token && !hu_mcp_oauth_token_is_expired(&slot->oauth_token)) {
            int written = snprintf(auth_buf, sizeof(auth_buf), "Bearer %s",
                                   slot->oauth_token.access_token);
            if (written > 0 && (size_t)written < sizeof(auth_buf)) {
                auth_header = auth_buf;
            }
        }

        /* HTTP POST to server URL */
        hu_http_response_t response = {0};
        hu_error_t http_err = hu_http_post_json(alloc, slot->url, auth_header,
                                                request, request_len, &response);
        alloc->free(alloc->ctx, request, request_len + 1);

        if (http_err != HU_OK) {
            char errbuf[256];
            int n = snprintf(errbuf, sizeof(errbuf), "HTTP request to '%s' failed: error %d",
                           slot->url ? slot->url : "?", (int)http_err);
            if (n < 0) n = 0;
            *out = hu_tool_result_fail(errbuf, (size_t)n);
            return HU_OK;
        }

        if (response.status_code < 200 || response.status_code >= 300) {
            char errbuf[512];
            const char *body = response.body ? response.body : "(empty)";
            int n = snprintf(errbuf, sizeof(errbuf),
                           "MCP HTTP error (status %ld): %.*s",
                           response.status_code,
                           (int)(response.body_len > 200 ? 200 : response.body_len),
                           body);
            if (n < 0) n = 0;
            hu_http_response_free(alloc, &response);
            *out = hu_tool_result_fail(errbuf, (size_t)n);
            return HU_OK;
        }

        /* Parse JSON-RPC response */
        uint32_t resp_id = 0;
        char *result = NULL;
        size_t result_len = 0;
        bool is_error = false;
        hu_error_t parse_err = hu_mcp_jsonrpc_parse_response(alloc, response.body, response.body_len,
                                                            &resp_id, &result, &result_len, &is_error);
        hu_http_response_free(alloc, &response);

        if (parse_err != HU_OK) {
            *out = hu_tool_result_fail("MCP response parse error: invalid JSON-RPC", 41);
            return HU_OK;
        }

        if (is_error) {
            /* Prefix tool error with context */
            char errbuf[512];
            const char *result_cstr = result ? result : "";
            int n = snprintf(errbuf, sizeof(errbuf), "MCP tool error: %.*s",
                           (int)(result_len > 400 ? 400 : result_len), result_cstr);
            if (n < 0) n = 0;
            if (result)
                alloc->free(alloc->ctx, result, result_len + 1);
            char *msg = (char *)alloc->alloc(alloc->ctx, (size_t)n + 1);
            if (msg) {
                memcpy(msg, errbuf, (size_t)n + 1);
                *out = hu_tool_result_fail_owned(msg, (size_t)n);
            } else {
                *out = hu_tool_result_fail(errbuf, (size_t)n);
            }
        } else {
            *out = hu_tool_result_ok_owned(result, result_len);
        }
        if (args_allocated && args_json)
            alloc->free(alloc->ctx, args_json, args_len + 1);
        return HU_OK;
#else
        *out = hu_tool_result_fail("HTTP transport requires HU_ENABLE_CURL", 38);
        if (args_allocated && args_json)
            alloc->free(alloc->ctx, args_json, args_len + 1);
        return HU_OK;
#endif
    }

    char *result = NULL;
    size_t result_len = 0;
    hu_error_t err = hu_mcp_client_call_tool(slot->server, alloc, w->original_name, args_json,
                                             &result, &result_len);
    if (args_allocated && args_json)
        alloc->free(alloc->ctx, args_json, args_len + 1);

    if (err != HU_OK) {
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "MCP tool '%s' on server '%s' failed: error %d",
                         w->original_name, slot->name ? slot->name : "?", (int)err);
        if (n < 0)
            n = 0;
        char *msg = (char *)alloc->alloc(alloc->ctx, (size_t)n + 1);
        if (msg) {
            memcpy(msg, buf, (size_t)n + 1);
            *out = hu_tool_result_fail_owned(msg, (size_t)n);
        } else {
            *out = hu_tool_result_fail("MCP tool call failed", 20);
        }
        return HU_OK;
    }

    /* Enforce max response size */
    if (result_len > HU_MCP_MANAGER_MAX_RESPONSE) {
        alloc->free(alloc->ctx, result, result_len + 1);
        *out = hu_tool_result_fail("MCP response exceeds 100MB limit", 32);
        return HU_OK;
    }

    *out = hu_tool_result_ok_owned(result, result_len);
    return HU_OK;
}

static const char *mgr_tool_name(void *ctx) {
    return ((hu_mcp_mgr_tool_wrapper_t *)ctx)->prefixed_name;
}

static const char *mgr_tool_description(void *ctx) {
    return ((hu_mcp_mgr_tool_wrapper_t *)ctx)->desc;
}

static const char *mgr_tool_parameters_json(void *ctx) {
    return ((hu_mcp_mgr_tool_wrapper_t *)ctx)->params_json;
}

static void mgr_tool_deinit(void *ctx, hu_allocator_t *alloc) {
    hu_mcp_mgr_tool_wrapper_t *w = (hu_mcp_mgr_tool_wrapper_t *)ctx;
    free_str(alloc, w->original_name);
    free_str(alloc, w->prefixed_name);
    free_str(alloc, w->desc);
    free_str(alloc, w->params_json);
    alloc->free(alloc->ctx, w, sizeof(*w));
}

static const hu_tool_vtable_t mgr_tool_vtable = {
    .execute = mgr_tool_execute,
    .name = mgr_tool_name,
    .description = mgr_tool_description,
    .parameters_json = mgr_tool_parameters_json,
    .deinit = mgr_tool_deinit,
};

/* ── Tool Discovery ───────────────────────────────────────────────────── */

hu_error_t hu_mcp_manager_load_tools(hu_mcp_manager_t *mgr, hu_allocator_t *alloc,
                                     hu_tool_t **out_tools, size_t *out_count) {
    if (!mgr || !alloc || !out_tools || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_tools = NULL;
    *out_count = 0;

    hu_tool_t *tools = NULL;
    size_t total = 0;
    size_t tools_cap = 0;

    for (size_t si = 0; si < mgr->slot_count; si++) {
        hu_mcp_mgr_slot_t *slot = &mgr->slots[si];
        if (!slot->connected)
            continue;

        if (!slot->server) {
            hu_log_info("mcp-manager", NULL, "mcp_manager: tool discovery not yet supported for %s transport",
                    slot->transport_type ? slot->transport_type : "unknown");
            continue;
        }

        char **names = NULL, **descs = NULL, **params = NULL;
        size_t n = 0;
        hu_error_t err;

        if (slot->server) {
            /* stdio-based server: use native MCP protocol */
            err = hu_mcp_client_list_tools(slot->server, alloc, &names, &descs, &params, &n);
#if HU_ENABLE_CURL
        } else if (slot->url) {
            /* HTTP/SSE-based server: use JSON-RPC tools/list over HTTP */
            char *request = NULL;
            size_t request_len = 0;
            uint32_t rpc_id = mgr->next_rpc_id++;
            err = hu_mcp_jsonrpc_build_tools_list(alloc, rpc_id, &request, &request_len);
            if (err != HU_OK) {
                fprintf(stderr, "mcp_manager: failed to build tools/list request for %s\n",
                        slot->name ? slot->name : "?");
                continue;
            }

            /* Build auth header if available */
            char auth_buf[2048] = {0};
            const char *auth_header = NULL;
            if (slot->oauth_token.access_token &&
                !hu_mcp_oauth_token_is_expired(&slot->oauth_token)) {
                int aw = snprintf(auth_buf, sizeof(auth_buf), "Bearer %s",
                                  slot->oauth_token.access_token);
                if (aw > 0 && (size_t)aw < sizeof(auth_buf))
                    auth_header = auth_buf;
            }

            hu_http_response_t response = {0};
            hu_error_t http_err = hu_http_post_json(alloc, slot->url, auth_header,
                                                    request, request_len, &response);
            alloc->free(alloc->ctx, request, request_len + 1);

            if (http_err != HU_OK || response.status_code < 200 || response.status_code >= 300) {
                fprintf(stderr, "mcp_manager: HTTP tools/list failed for %s (status %ld)\n",
                        slot->name ? slot->name : "?", response.status_code);
                hu_http_response_free(alloc, &response);
                continue;
            }

            /* Parse JSON-RPC response */
            char *result_json = NULL;
            size_t result_len = 0;
            uint32_t resp_id = 0;
            bool is_error = false;
            err = hu_mcp_jsonrpc_parse_response(alloc, response.body, response.body_len,
                                                &resp_id, &result_json, &result_len, &is_error);
            hu_http_response_free(alloc, &response);

            if (err != HU_OK || is_error || !result_json) {
                if (result_json) alloc->free(alloc->ctx, result_json, result_len + 1);
                fprintf(stderr, "mcp_manager: tools/list RPC error for %s\n",
                        slot->name ? slot->name : "?");
                continue;
            }

            /* Parse result_json as a JSON object containing "tools" array.
             * Each tool: {"name": "...", "description": "...", "inputSchema": {...}} */
            hu_json_value_t *root = NULL;
            err = hu_json_parse(alloc, result_json, result_len, &root);
            alloc->free(alloc->ctx, result_json, result_len + 1);

            if (err != HU_OK || !root) {
                fprintf(stderr, "mcp_manager: failed to parse tools/list result for %s\n",
                        slot->name ? slot->name : "?");
                continue;
            }

            /* Extract tools array */
            hu_json_value_t *tools_arr = hu_json_object_get(root, "tools");
            if (!tools_arr || tools_arr->type != HU_JSON_ARRAY) {
                hu_json_free(alloc, root);
                continue;
            }

            n = tools_arr->data.array.len;
            if (n == 0) {
                hu_json_free(alloc, root);
                continue;
            }

            names = (char **)alloc->alloc(alloc->ctx, n * sizeof(char *));
            descs = (char **)alloc->alloc(alloc->ctx, n * sizeof(char *));
            params = (char **)alloc->alloc(alloc->ctx, n * sizeof(char *));
            if (!names || !descs || !params) {
                if (names) alloc->free(alloc->ctx, names, n * sizeof(char *));
                if (descs) alloc->free(alloc->ctx, descs, n * sizeof(char *));
                if (params) alloc->free(alloc->ctx, params, n * sizeof(char *));
                hu_json_free(alloc, root);
                continue;
            }
            memset(names, 0, n * sizeof(char *));
            memset(descs, 0, n * sizeof(char *));
            memset(params, 0, n * sizeof(char *));

            for (size_t ti = 0; ti < n; ti++) {
                hu_json_value_t *tool_obj = tools_arr->data.array.items[ti];
                if (!tool_obj || tool_obj->type != HU_JSON_OBJECT) continue;

                const char *tname = hu_json_get_string(tool_obj, "name");
                const char *tdesc = hu_json_get_string(tool_obj, "description");
                if (tname) {
                    size_t tname_len = strlen(tname);
                    names[ti] = (char *)alloc->alloc(alloc->ctx, tname_len + 1);
                    if (names[ti]) { memcpy(names[ti], tname, tname_len); names[ti][tname_len] = '\0'; }
                }
                if (tdesc) {
                    size_t tdesc_len = strlen(tdesc);
                    descs[ti] = (char *)alloc->alloc(alloc->ctx, tdesc_len + 1);
                    if (descs[ti]) { memcpy(descs[ti], tdesc, tdesc_len); descs[ti][tdesc_len] = '\0'; }
                }
                /* inputSchema as JSON string */
                hu_json_value_t *schema = hu_json_object_get(tool_obj, "inputSchema");
                if (schema) {
                    params[ti] = (char *)alloc->alloc(alloc->ctx, 3);
                    if (params[ti]) { memcpy(params[ti], "{}", 3); }
                }
            }
            hu_json_free(alloc, root);
            err = HU_OK;
#endif /* HU_ENABLE_CURL */
        } else {
            fprintf(stderr, "mcp_manager: tool discovery not supported for %s (no server or URL)\n",
                    slot->name ? slot->name : "?");
            continue;
        }
        if (err != HU_OK || n == 0)
            continue;

        slot->tool_count = n;

        /* Ensure capacity */
        if (total + n > tools_cap) {
            size_t new_cap = (total + n) * 2;
            if (new_cap < 8)
                new_cap = 8;
            hu_tool_t *nb = (hu_tool_t *)alloc->realloc(
                alloc->ctx, tools, tools_cap * sizeof(hu_tool_t), new_cap * sizeof(hu_tool_t));
            if (!nb) {
                /* Free this batch's string arrays */
                for (size_t j = 0; j < n; j++) {
                    free_str(alloc, names[j]);
                    free_str(alloc, descs[j]);
                    if (params[j])
                        free_str(alloc, params[j]);
                }
                alloc->free(alloc->ctx, names, n * sizeof(char *));
                alloc->free(alloc->ctx, descs, n * sizeof(char *));
                alloc->free(alloc->ctx, params, n * sizeof(char *));
                goto fail;
            }
            tools = nb;
            tools_cap = new_cap;
        }

        for (size_t j = 0; j < n; j++) {
            if (!names[j])
                continue;

            /* Build prefixed name: mcp__<server_name>__<tool_name> */
            const char *srv_name = slot->name ? slot->name : "unknown";
            size_t pref_len = 6 + strlen(srv_name) + 2 + strlen(names[j]); /* mcp__ + srv + __ + tool */
            char *prefixed = (char *)alloc->alloc(alloc->ctx, pref_len + 1);
            if (!prefixed) {
                /* Free remaining strings in this batch */
                for (size_t k = j; k < n; k++) {
                    free_str(alloc, names[k]);
                    free_str(alloc, descs[k]);
                    if (params[k])
                        free_str(alloc, params[k]);
                }
                alloc->free(alloc->ctx, names, n * sizeof(char *));
                alloc->free(alloc->ctx, descs, n * sizeof(char *));
                alloc->free(alloc->ctx, params, n * sizeof(char *));
                goto fail;
            }
            snprintf(prefixed, pref_len + 1, "mcp__%s__%s", srv_name, names[j]);

            hu_mcp_mgr_tool_wrapper_t *w = (hu_mcp_mgr_tool_wrapper_t *)alloc->alloc(
                alloc->ctx, sizeof(hu_mcp_mgr_tool_wrapper_t));
            if (!w) {
                alloc->free(alloc->ctx, prefixed, pref_len + 1);
                for (size_t k = j; k < n; k++) {
                    free_str(alloc, names[k]);
                    free_str(alloc, descs[k]);
                    if (params[k])
                        free_str(alloc, params[k]);
                }
                alloc->free(alloc->ctx, names, n * sizeof(char *));
                alloc->free(alloc->ctx, descs, n * sizeof(char *));
                alloc->free(alloc->ctx, params, n * sizeof(char *));
                goto fail;
            }
            w->alloc = alloc;
            w->mgr = mgr;
            w->slot_index = si;
            w->original_name = names[j];
            w->prefixed_name = prefixed;
            w->desc = descs[j];
            w->params_json = (params && params[j]) ? params[j] : dup_str(alloc, "{}");

            tools[total].ctx = w;
            tools[total].vtable = &mgr_tool_vtable;
            total++;
        }

        /* Free the arrays themselves (strings are now owned by wrappers) */
        alloc->free(alloc->ctx, names, n * sizeof(char *));
        alloc->free(alloc->ctx, descs, n * sizeof(char *));
        if (params)
            alloc->free(alloc->ctx, params, n * sizeof(char *));
    }

    /* Shrink to exact size */
    if (total > 0 && total < tools_cap) {
        hu_tool_t *shrunk = (hu_tool_t *)alloc->realloc(
            alloc->ctx, tools, tools_cap * sizeof(hu_tool_t), total * sizeof(hu_tool_t));
        if (shrunk)
            tools = shrunk;
    }

    *out_tools = tools;
    *out_count = total;
    return HU_OK;

fail:
    if (tools) {
        for (size_t i = 0; i < total; i++) {
            if (tools[i].vtable && tools[i].vtable->deinit)
                tools[i].vtable->deinit(tools[i].ctx, alloc);
        }
        alloc->free(alloc->ctx, tools, tools_cap * sizeof(hu_tool_t));
    }
    return HU_ERR_OUT_OF_MEMORY;
}

void hu_mcp_manager_free_tools(hu_allocator_t *alloc, hu_tool_t *tools, size_t count) {
    if (!alloc || !tools)
        return;
    for (size_t i = 0; i < count; i++) {
        if (tools[i].vtable && tools[i].vtable->deinit)
            tools[i].vtable->deinit(tools[i].ctx, alloc);
    }
    alloc->free(alloc->ctx, tools, count * sizeof(hu_tool_t));
}

/* ── Direct Tool Call ─────────────────────────────────────────────────── */

hu_error_t hu_mcp_manager_call_tool(hu_mcp_manager_t *mgr, hu_allocator_t *alloc,
                                    const char *server_name, const char *tool_name,
                                    const char *args_json, char **out_result,
                                    size_t *out_result_len) {
    if (!mgr || !alloc || !server_name || !tool_name || !out_result || !out_result_len)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < mgr->slot_count; i++) {
        if (!mgr->slots[i].name || strcmp(mgr->slots[i].name, server_name) != 0)
            continue;
        if (!mgr->slots[i].connected)
            return HU_ERR_IO;

        /* stdio-based server */
        if (mgr->slots[i].server) {
            return hu_mcp_client_call_tool(mgr->slots[i].server, alloc, tool_name, args_json,
                                           out_result, out_result_len);
        }

#if HU_ENABLE_CURL
        /* HTTP/SSE-based server: JSON-RPC tools/call over HTTP */
        if (mgr->slots[i].url) {
            hu_mcp_mgr_slot_t *slot = &mgr->slots[i];
            char *request = NULL;
            size_t request_len = 0;
            uint32_t rpc_id = mgr->next_rpc_id++;
            hu_error_t jerr = hu_mcp_jsonrpc_build_tools_call(alloc, rpc_id,
                tool_name, args_json ? args_json : "{}", &request, &request_len);
            if (jerr != HU_OK)
                return jerr;

            char auth_buf[2048] = {0};
            const char *auth_header = NULL;
            if (slot->oauth_token.access_token &&
                !hu_mcp_oauth_token_is_expired(&slot->oauth_token)) {
                int aw = snprintf(auth_buf, sizeof(auth_buf), "Bearer %s",
                                  slot->oauth_token.access_token);
                if (aw > 0 && (size_t)aw < sizeof(auth_buf))
                    auth_header = auth_buf;
            }

            hu_http_response_t response = {0};
            hu_error_t http_err = hu_http_post_json(alloc, slot->url, auth_header,
                                                    request, request_len, &response);
            alloc->free(alloc->ctx, request, request_len + 1);
            if (http_err != HU_OK) {
                hu_http_response_free(alloc, &response);
                return http_err;
            }

            char *result = NULL;
            size_t result_len = 0;
            uint32_t resp_id = 0;
            bool is_error = false;
            hu_error_t perr = hu_mcp_jsonrpc_parse_response(alloc, response.body,
                response.body_len, &resp_id, &result, &result_len, &is_error);
            hu_http_response_free(alloc, &response);

            if (perr != HU_OK)
                return perr;
            if (is_error) {
                if (result) alloc->free(alloc->ctx, result, result_len + 1);
                return HU_ERR_TOOL_EXECUTION;
            }
            *out_result = result;
            *out_result_len = result_len;
            return HU_OK;
        }
#endif
        return HU_ERR_NOT_SUPPORTED;
    }
    return HU_ERR_NOT_FOUND;
}

/* ── Queries ──────────────────────────────────────────────────────────── */

size_t hu_mcp_manager_server_count(const hu_mcp_manager_t *mgr) {
    return mgr ? mgr->slot_count : 0;
}

hu_error_t hu_mcp_manager_server_info(const hu_mcp_manager_t *mgr, size_t index,
                                      hu_mcp_server_info_t *out) {
    if (!mgr || !out || index >= mgr->slot_count)
        return HU_ERR_NOT_FOUND;

    const hu_mcp_mgr_slot_t *slot = &mgr->slots[index];
    out->name = slot->name;
    out->command = slot->config.command;
    out->connected = slot->connected;
    out->tool_count = slot->tool_count;
    out->timeout_ms = slot->timeout_ms;
    return HU_OK;
}

hu_error_t hu_mcp_manager_find_server(const hu_mcp_manager_t *mgr, const char *name,
                                      size_t *out_index) {
    if (!mgr || !name || !out_index)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < mgr->slot_count; i++) {
        if (mgr->slots[i].name && strcmp(mgr->slots[i].name, name) == 0) {
            *out_index = i;
            return HU_OK;
        }
    }
    return HU_ERR_NOT_FOUND;
}
