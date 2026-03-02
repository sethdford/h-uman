/*
 * HTTP request tool — GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS.
 * Validates URL (HTTPS only, no private IPs). Supports custom headers and body.
 */
#include "seaclaw/tool.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tools/validation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define SC_HTTP_REQUEST_NAME "http_request"
#define SC_HTTP_REQUEST_DESC "Make HTTP requests. Supports GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS. HTTPS only, no private IPs."
#define SC_HTTP_REQUEST_PARAMS "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"},\"method\":{\"type\":\"string\",\"default\":\"GET\"},\"headers\":{\"type\":\"object\"},\"body\":{\"type\":\"string\"}},\"required\":[\"url\"]}"
#define SC_HTTP_MAX_BODY 1048576

typedef struct sc_http_request_ctx {
    bool allow_http;
} sc_http_request_ctx_t;

static int method_valid(const char *method)
{
    if (!method || !method[0]) return 0;
    size_t len = strlen(method);
    if (len > 16) return 0;
    char upper[17];
    for (size_t i = 0; i < len; i++) upper[i] = (char)toupper((unsigned char)method[i]);
    upper[len] = '\0';
    if (strcmp(upper, "GET") == 0) return 1;
    if (strcmp(upper, "POST") == 0) return 1;
    if (strcmp(upper, "PUT") == 0) return 1;
    if (strcmp(upper, "DELETE") == 0) return 1;
    if (strcmp(upper, "PATCH") == 0) return 1;
    if (strcmp(upper, "HEAD") == 0) return 1;
    if (strcmp(upper, "OPTIONS") == 0) return 1;
    return 0;
}

#if !SC_IS_TEST
static bool header_value_safe(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\r' || s[i] == '\n' || s[i] == '\0') return false;
    }
    return true;
}

/* Build extra_headers string from JSON object. Caller frees. */
static sc_error_t parse_headers(sc_allocator_t *alloc, const sc_json_value_t *headers_val,
    char **out, size_t *out_len)
{
    if (!headers_val || headers_val->type != SC_JSON_OBJECT || headers_val->data.object.len == 0) {
        *out = NULL; *out_len = 0;
        return SC_OK;
    }
    size_t cap = 2048;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) return SC_ERR_OUT_OF_MEMORY;
    size_t len = 0;

    for (size_t i = 0; i < headers_val->data.object.len; i++) {
        sc_json_pair_t *pair = &headers_val->data.object.pairs[i];
        if (!pair->key || !pair->value || pair->value->type != SC_JSON_STRING) continue;
        const char *k = pair->key;
        size_t klen = pair->key_len;
        const char *v = pair->value->data.string.ptr;
        size_t vlen = pair->value->data.string.len;
        if (!header_value_safe(k, klen) || !header_value_safe(v, vlen))
            continue;
        if (len + klen + vlen + 4 > cap) {
            size_t nc = cap * 2;
            char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, nc);
            if (!nb) { alloc->free(alloc->ctx, buf, cap); return SC_ERR_OUT_OF_MEMORY; }
            buf = nb; cap = nc;
        }
        memcpy(buf + len, k, klen); len += klen;
        buf[len++] = ':'; buf[len++] = ' ';
        memcpy(buf + len, v, vlen); len += vlen;
        buf[len++] = '\n';
    }
    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    return SC_OK;
}
#endif

static sc_error_t http_request_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args,
    sc_tool_result_t *out)
{
    (void)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *url = sc_json_get_string(args, "url");
    if (!url || strlen(url) == 0) {
        *out = sc_tool_result_fail("Missing 'url' parameter", 22);
        return SC_OK;
    }
    if (sc_tool_validate_url(url) != SC_OK) {
        *out = sc_tool_result_fail("Only HTTPS allowed, no private IPs", 33);
        return SC_OK;
    }

    const char *method = sc_json_get_string(args, "method");
    if (!method || !method[0]) method = "GET";
    if (!method_valid(method)) {
        char err[64];
        int n = snprintf(err, sizeof(err), "Unsupported HTTP method: %s", method);
        char *e = sc_strndup(alloc, err, (size_t)n);
        *out = e ? sc_tool_result_fail_owned(e, (size_t)n) : sc_tool_result_fail("Unsupported method", 18);
        return SC_OK;
    }

#if SC_IS_TEST
    char *msg = sc_strndup(alloc, "(http_request stub in test)", 27);
    if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    *out = sc_tool_result_ok_owned(msg, 27);
    return SC_OK;
#else
    const char *body = sc_json_get_string(args, "body");
    size_t body_len = body ? strlen(body) : 0;
    if (body_len > SC_HTTP_MAX_BODY) {
        *out = sc_tool_result_fail("body too large", 14);
        return SC_OK;
    }

    char *extra_headers = NULL;
    size_t extra_len = 0;
    sc_json_value_t *headers_val = sc_json_object_get(args, "headers");
    if (headers_val) {
        sc_error_t err = parse_headers(alloc, headers_val, &extra_headers, &extra_len);
        if (err != SC_OK) {
            *out = sc_tool_result_fail("failed to parse headers", 23);
            return SC_OK;
        }
    }

    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_request(alloc, url, method, extra_headers, body, body_len, &resp);
    if (extra_headers) alloc->free(alloc->ctx, extra_headers, extra_len + 1);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("HTTP request failed", 19);
        return SC_OK;
    }

    long status = resp.status_code;
    size_t body_sz = resp.body_len;
    char *output = (char *)alloc->alloc(alloc->ctx, 128 + body_sz + 1);
    if (!output) {
        sc_http_response_free(alloc, &resp);
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(output, 128, "Status: %ld\n\nResponse Body:\n", (long)status);
    size_t out_len = (n > 0 ? (size_t)n : 0);
    if (resp.body && body_sz > 0) {
        memcpy(output + out_len, resp.body, body_sz);
        out_len += body_sz;
    }
    output[out_len] = '\0';
    sc_http_response_free(alloc, &resp);

    bool success = (status >= 200 && status < 300);
    *out = sc_tool_result_ok_owned(output, out_len);
    if (!success) {
        char errbuf[32];
        int en = snprintf(errbuf, sizeof(errbuf), "HTTP %ld", (long)status);
        char *em = sc_strndup(alloc, errbuf, (size_t)en);
        if (em) {
            out->error_msg = em;
            out->error_msg_len = (size_t)en;
            out->error_msg_owned = true;
        }
    }
    return SC_OK;
#endif
}

static const char *http_request_name(void *ctx) { (void)ctx; return SC_HTTP_REQUEST_NAME; }
static const char *http_request_desc(void *ctx) { (void)ctx; return SC_HTTP_REQUEST_DESC; }
static const char *http_request_params(void *ctx) { (void)ctx; return SC_HTTP_REQUEST_PARAMS; }
static void http_request_deinit(void *ctx, sc_allocator_t *alloc) { (void)alloc; free(ctx); }

static const sc_tool_vtable_t http_request_vtable = {
    .execute = http_request_execute, .name = http_request_name,
    .description = http_request_desc, .parameters_json = http_request_params,
    .deinit = http_request_deinit,
};

sc_error_t sc_http_request_create(sc_allocator_t *alloc, bool allow_http, sc_tool_t *out)
{
    (void)alloc;
    (void)allow_http;
    sc_http_request_ctx_t *c = (sc_http_request_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->allow_http = allow_http;
    out->ctx = c;
    out->vtable = &http_request_vtable;
    return SC_OK;
}
