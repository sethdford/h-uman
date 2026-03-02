/* Browser tool: open URL, read page content via HTTP.
 * CDP (Chrome DevTools Protocol) integration — required for click, type, scroll —
 * needs a WebSocket connection to a Chrome instance and is not yet implemented.
 * Those actions return "CDP browser automation not available".
 * Currently supported: open (system browser), read (HTTP fetch). */
#include "seaclaw/tool.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/process_util.h"
#include "seaclaw/core/string.h"
#include "seaclaw/security.h"
#include "seaclaw/tools/validation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SC_BROWSER_READ_MAX 8192

typedef struct sc_browser_ctx {
    sc_security_policy_t *policy;
} sc_browser_ctx_t;

static sc_error_t browser_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args, sc_tool_result_t *out) {
    sc_browser_ctx_t *bc = (sc_browser_ctx_t *)ctx;
    (void)bc;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *action = sc_json_get_string(args, "action");
    if (!action || action[0] == '\0') {
        *out = sc_tool_result_fail("missing action", 14);
        return SC_OK;
    }
    if (strcmp(action, "open") == 0) {
        const char *url = sc_json_get_string(args, "url");
        if (!url || url[0] == '\0') {
            *out = sc_tool_result_fail("missing url", 11);
            return SC_OK;
        }
        /* Secure by default: HTTPS only, or http://localhost for local dev */
        if (sc_tool_validate_url(url) == SC_OK) {
            /* Valid HTTPS URL */
        } else if (strncasecmp(url, "http://localhost", 16) == 0 &&
                   (url[16] == '\0' || url[16] == '/' || url[16] == ':' || url[16] == '?' || url[16] == '#')) {
            /* Allow http://localhost for local development */
        } else {
            *out = sc_tool_result_fail("invalid url: use https:// or http://localhost only", 48);
            return SC_OK;
        }
#if SC_IS_TEST
        size_t need = 27 + strlen(url);
        char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        int n = snprintf(msg, need + 1, "Opened %s in system browser", url);
        size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
        msg[len] = '\0';
        *out = sc_tool_result_ok_owned(msg, len);
        return SC_OK;
#else
        {
            const char *argv[4];
#ifdef __APPLE__
            argv[0] = "open";
#else
            argv[0] = "xdg-open";
#endif
            argv[1] = url;
            argv[2] = NULL;
            sc_run_result_t run = {0};
            sc_error_t err = sc_process_run_with_policy(alloc, argv, NULL,
                4096, bc ? bc->policy : NULL, &run);
            sc_run_result_free(alloc, &run);
            if (err != SC_OK) {
                *out = sc_tool_result_fail("Failed to open browser", 22);
                return SC_OK;
            }
            if (!run.success) {
                *out = sc_tool_result_fail("Browser open failed", 18);
                return SC_OK;
            }
            char *msg = sc_strndup(alloc, "Opened in system browser", 24);
            if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
            *out = sc_tool_result_ok_owned(msg, 24);
            return SC_OK;
        }
#endif
    }
    if (strcmp(action, "read") == 0) {
        const char *url = sc_json_get_string(args, "url");
        if (!url || url[0] == '\0') {
            *out = sc_tool_result_fail("missing url", 11);
            return SC_OK;
        }
        if (sc_tool_validate_url(url) != SC_OK) {
            *out = sc_tool_result_fail("invalid url: HTTPS only, no private IPs", 37);
            return SC_OK;
        }
#if SC_IS_TEST
        size_t need = 28 + strlen(url);
        char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        int n = snprintf(msg, need + 1, "<html><body>Mock page for %s</body></html>", url);
        size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
        msg[len] = '\0';
        *out = sc_tool_result_ok_owned(msg, len);
        return SC_OK;
#else
        {
            sc_http_response_t resp = {0};
            sc_error_t err = sc_http_get_ex(alloc, url, NULL, &resp);
            if (err != SC_OK) {
                *out = sc_tool_result_fail("Fetch failed", 12);
                return SC_OK;
            }
            if (resp.status_code < 200 || resp.status_code >= 300) {
                sc_http_response_free(alloc, &resp);
                *out = sc_tool_result_fail("HTTP request failed", 18);
                return SC_OK;
            }
            size_t copy_len = resp.body_len;
            if (copy_len > SC_BROWSER_READ_MAX) copy_len = SC_BROWSER_READ_MAX;
            char *body = (char *)alloc->alloc(alloc->ctx, copy_len + 1);
            if (!body) {
                sc_http_response_free(alloc, &resp);
                *out = sc_tool_result_fail("out of memory", 12);
                return SC_ERR_OUT_OF_MEMORY;
            }
            if (resp.body && copy_len > 0) memcpy(body, resp.body, copy_len);
            body[copy_len] = '\0';
            sc_http_response_free(alloc, &resp);
            *out = sc_tool_result_ok_owned(body, copy_len);
            return SC_OK;
        }
#endif
    }
    if (strcmp(action, "screenshot") == 0) {
        *out = sc_tool_result_fail("Use the screenshot tool instead", 31);
        return SC_OK;
    }
    if (strcmp(action, "click") == 0 || strcmp(action, "type") == 0 || strcmp(action, "scroll") == 0) {
        *out = sc_tool_result_fail("CDP browser automation not available", 37);
        return SC_OK;
    }
    *out = sc_tool_result_fail("unknown action", 14);
    return SC_OK;
}
static const char *browser_name(void *ctx) { (void)ctx; return "browser"; }
static const char *browser_desc(void *ctx) { (void)ctx; return "Open a URL in the user's system browser for visual display. NOT for searching or retrieving content — use web_search or web_fetch instead."; }
static const char *browser_params(void *ctx) { (void)ctx; return "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"open\",\"read\",\"screenshot\",\"click\",\"type\",\"scroll\"]},\"url\":{\"type\":\"string\"},\"selector\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"}},\"required\":[\"action\"]}"; }
static void browser_deinit(void *ctx, sc_allocator_t *alloc) { (void)alloc; free(ctx); }

static const sc_tool_vtable_t browser_vtable = {
    .execute = browser_execute, .name = browser_name,
    .description = browser_desc, .parameters_json = browser_params,
    .deinit = browser_deinit,
};

sc_error_t sc_browser_create(sc_allocator_t *alloc, bool enabled,
    sc_security_policy_t *policy, sc_tool_t *out) {
    (void)alloc; (void)enabled;
    sc_browser_ctx_t *c = (sc_browser_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->policy = policy;
    out->ctx = c;
    out->vtable = &browser_vtable;
    return SC_OK;
}
