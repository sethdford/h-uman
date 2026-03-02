#include "seaclaw/tool.h"
#include "seaclaw/tools/browser_open.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/process_util.h"
#include "seaclaw/core/string.h"
#include "seaclaw/security.h"
#include "seaclaw/tools/validation.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define BROWSER_OPEN_PARAMS "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}"

typedef struct sc_browser_open_ctx {
    sc_allocator_t *alloc;
    char **allowed_domains;
    size_t allowed_count;
    sc_security_policy_t *policy;
} sc_browser_open_ctx_t;

static bool is_local_or_private(const char *host, size_t len) {
    if (len >= 9 && strncmp(host, "localhost", 9) == 0) return true;
    if (len >= 10 && strncmp(host + len - 10, ".localhost", 10) == 0) return true;
    if (len >= 6 && strncmp(host + len - 6, ".local", 6) == 0) return true;
    if (len == 3 && strncmp(host, "::1", 3) == 0) return true;
    if (len >= 2 && strncmp(host, "10.", 3) == 0) return true;
    if (len >= 4 && strncmp(host, "127.", 4) == 0) return true;
    if (len >= 8 && strncmp(host, "192.168.", 8) == 0) return true;
    if (len >= 8 && strncmp(host, "169.254.", 8) == 0) return true;
    return false;
}

static sc_error_t browser_open_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args, sc_tool_result_t *out)
{
    sc_browser_open_ctx_t *c = (sc_browser_open_ctx_t *)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *url = sc_json_get_string(args, "url");
    if (!url || url[0] == '\0') {
        *out = sc_tool_result_fail("Missing 'url' parameter", 24);
        return SC_OK;
    }
    size_t url_len = strlen(url);
    if (url_len < 9 || strncmp(url, "https://", 8) != 0) {
        *out = sc_tool_result_fail("Only https:// URLs are allowed", 31);
        return SC_OK;
    }
    const char *rest = url + 8;
    size_t rest_len = url_len - 8;
    size_t host_end = rest_len;
    for (size_t i = 0; i < rest_len; i++) {
        if (rest[i] == '/' || rest[i] == '?' || rest[i] == '#') {
            host_end = i;
            break;
        }
    }
    if (host_end == 0) {
        *out = sc_tool_result_fail("URL must include a host", 23);
        return SC_OK;
    }
    const char *host = rest;
    size_t host_len = host_end;
    for (size_t i = 0; i < host_len; i++) {
        if (host[i] == ':') {
            host_len = i;
            break;
        }
    }
    if (is_local_or_private(host, host_len)) {
        *out = sc_tool_result_fail("Blocked local/private host", 28);
        return SC_OK;
    }
    if (!c->allowed_domains || c->allowed_count == 0) {
        *out = sc_tool_result_fail("No allowed_domains configured for browser_open", 46);
        return SC_OK;
    }
    bool allowed = false;
    for (size_t i = 0; i < c->allowed_count; i++) {
        const char *d = c->allowed_domains[i];
        size_t dlen = strlen(d);
        if (host_len == dlen && strncmp(host, d, dlen) == 0) {
            allowed = true;
            break;
        }
        if (host_len > dlen && host[host_len - dlen - 1] == '.' &&
            strncmp(host + host_len - dlen, d, dlen) == 0) {
            allowed = true;
            break;
        }
    }
    if (!allowed) {
        *out = sc_tool_result_fail("Host is not in browser allowed_domains", 39);
        return SC_OK;
    }
#if SC_IS_TEST
    {
        size_t need = 28 + url_len;
        char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        int n = snprintf(msg, need + 1, "Opened %s in default browser", url);
        size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
        msg[len] = '\0';
        *out = sc_tool_result_ok_owned(msg, len);
    }
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
            4096, c->policy, &run);
        sc_run_result_free(alloc, &run);
        if (err != SC_OK) {
            *out = sc_tool_result_fail("Failed to open browser", 22);
            return SC_OK;
        }
        if (!run.success) {
            *out = sc_tool_result_fail("Browser open failed", 18);
            return SC_OK;
        }
        size_t need = 28 + url_len;
        char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        int n = snprintf(msg, need + 1, "Opened %s in default browser", url);
        size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
        msg[len] = '\0';
        *out = sc_tool_result_ok_owned(msg, len);
    }
    return SC_OK;
#endif
}

static const char *browser_open_name(void *ctx) { (void)ctx; return "browser_open"; }
static const char *browser_open_desc(void *ctx) {
    (void)ctx;
    return "Open an approved HTTPS URL in the default browser. Only allowlisted domains are permitted.";
}
static const char *browser_open_params(void *ctx) { (void)ctx; return BROWSER_OPEN_PARAMS; }
static void browser_open_deinit(void *ctx, sc_allocator_t *alloc) {
    sc_browser_open_ctx_t *c = (sc_browser_open_ctx_t *)ctx;
    if (c && c->allowed_domains) {
        for (size_t i = 0; i < c->allowed_count; i++)
            if (c->allowed_domains[i]) alloc->free(alloc->ctx, c->allowed_domains[i], strlen(c->allowed_domains[i]) + 1);
        alloc->free(alloc->ctx, c->allowed_domains, c->allowed_count * sizeof(char *));
    }
    free(ctx);
}

static const sc_tool_vtable_t browser_open_vtable = {
    .execute = browser_open_execute, .name = browser_open_name,
    .description = browser_open_desc, .parameters_json = browser_open_params,
    .deinit = browser_open_deinit,
};

sc_error_t sc_browser_open_create(sc_allocator_t *alloc,
    const char *const *allowed_domains, size_t allowed_count,
    sc_security_policy_t *policy, sc_tool_t *out)
{
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    sc_browser_open_ctx_t *c = (sc_browser_open_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    c->policy = policy;
    c->allowed_count = allowed_count;
    if (allowed_domains && allowed_count > 0) {
        c->allowed_domains = (char **)alloc->alloc(alloc->ctx, allowed_count * sizeof(char *));
        if (!c->allowed_domains) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        memset(c->allowed_domains, 0, allowed_count * sizeof(char *));
        for (size_t i = 0; i < allowed_count; i++) {
            size_t len = strlen(allowed_domains[i]);
            c->allowed_domains[i] = (char *)alloc->alloc(alloc->ctx, len + 1);
            if (!c->allowed_domains[i]) {
                for (size_t j = 0; j < i; j++)
                    alloc->free(alloc->ctx, c->allowed_domains[j], strlen(c->allowed_domains[j]) + 1);
                alloc->free(alloc->ctx, c->allowed_domains, allowed_count * sizeof(char *));
                free(c);
                return SC_ERR_OUT_OF_MEMORY;
            }
            memcpy(c->allowed_domains[i], allowed_domains[i], len + 1);
        }
    }
    out->ctx = c;
    out->vtable = &browser_open_vtable;
    return SC_OK;
}
