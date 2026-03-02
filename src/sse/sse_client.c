#include "seaclaw/sse/sse_client.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <string.h>
#include <stdlib.h>

#define SC_SSE_MAX_EVENT_SIZE (256 * 1024)
#define SC_SSE_MAX_BUFFER_SIZE 8192

#if SC_IS_TEST
static sc_error_t sc_sse_connect_impl(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    const char *extra_headers,
    sc_sse_callback_t callback,
    void *callback_ctx)
{
    (void)url;
    (void)auth_header;
    (void)extra_headers;

    if (!alloc || !callback) return SC_ERR_INVALID_ARGUMENT;

    /* Emit one mock event */
    sc_sse_event_t ev = {0};
    const char *et = "message";
    const char *data = "{\"mock\":\"sse_connect\"}";
    size_t et_len = strlen(et);
    size_t data_len = strlen(data);

    ev.event_type = (char *)alloc->alloc(alloc->ctx, et_len + 1);
    if (!ev.event_type) return SC_ERR_OUT_OF_MEMORY;
    memcpy(ev.event_type, et, et_len + 1);
    ev.event_type_len = et_len;

    ev.data = (char *)alloc->alloc(alloc->ctx, data_len + 1);
    if (!ev.data) {
        alloc->free(alloc->ctx, ev.event_type, et_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memcpy(ev.data, data, data_len + 1);
    ev.data_len = data_len;

    callback(callback_ctx, &ev);

    alloc->free(alloc->ctx, ev.event_type, et_len + 1);
    alloc->free(alloc->ctx, ev.data, data_len + 1);
    return SC_OK;
}
#else
#if defined(SC_HTTP_CURL)
#include <curl/curl.h>

typedef struct sse_ctx {
    sc_allocator_t *alloc;
    char *buf;
    size_t len;
    size_t cap;
    sc_sse_callback_t callback;
    void *callback_ctx;
    sc_error_t last_error;
} sse_ctx_t;

static void parse_field(const char *line, size_t line_len,
    const char **field_out, size_t *field_len,
    const char **value_out, size_t *value_len)
{
    const char *colon = memchr(line, ':', line_len);
    if (colon) {
        *field_out = line;
        *field_len = (size_t)(colon - line);
        const char *val = colon + 1;
        size_t val_len = line_len - (size_t)(colon - line) - 1;
        if (val_len > 0 && val[0] == ' ') {
            val++;
            val_len--;
        }
        *value_out = val;
        *value_len = val_len;
    } else {
        *field_out = line;
        *field_len = line_len;
        *value_out = line + line_len;
        *value_len = 0;
    }
}

static int field_eq(const char *a, size_t alen, const char *b) {
    size_t blen = strlen(b);
    return alen == blen && memcmp(a, b, alen) == 0;
}

static void flush_event(sse_ctx_t *ctx,
    char *event_type, size_t event_type_len,
    char *data, size_t data_len)
{
    sc_sse_event_t ev = {0};
    ev.event_type = event_type;
    ev.event_type_len = event_type_len;
    ev.data = data;
    ev.data_len = data_len;
    ctx->callback(ctx->callback_ctx, &ev);
    if (event_type) ctx->alloc->free(ctx->alloc->ctx, event_type, event_type_len + 1);
    if (data) ctx->alloc->free(ctx->alloc->ctx, data, data_len + 1);
}

static sc_error_t process_buffer(sse_ctx_t *ctx) {
    char *buf = ctx->buf;
    size_t len = ctx->len;
    if (len == 0) return SC_OK;

    char *event_type = NULL;
    size_t event_type_len = 0;
    char *data = NULL;
    size_t data_len = 0;
    size_t data_cap = 0;
    int has_data = 0;
    size_t total_event_size = 0;

    const char *p = buf;
    const char *end = buf + len;
    const char *line_start = p;

    while (p < end) {
        if (*p == '\n' || (*p == '\r' && p + 1 < end && p[1] == '\n')) {
            size_t line_len = (size_t)(p - line_start);
            /* trim trailing CR */
            if (line_len > 0 && line_start[line_len - 1] == '\r') line_len--;

            if (line_len == 0) {
                if (has_data) {
                    char *data_copy = NULL;
                    size_t data_copy_len = 0;
                    if (data) {
                        data_copy_len = data_len;
                        data_copy = (char *)ctx->alloc->alloc(ctx->alloc->ctx, data_copy_len + 1);
                        if (!data_copy) {
                            if (event_type) ctx->alloc->free(ctx->alloc->ctx, event_type, event_type_len + 1);
                            if (data) ctx->alloc->free(ctx->alloc->ctx, data, data_cap);
                            return SC_ERR_OUT_OF_MEMORY;
                        }
                        memcpy(data_copy, data, data_len);
                        data_copy[data_len] = '\0';
                        ctx->alloc->free(ctx->alloc->ctx, data, data_cap);
                        data = NULL;
                    }
                    char *et_copy = NULL;
                    size_t et_copy_len = event_type_len;
                    if (event_type) {
                        et_copy = (char *)ctx->alloc->alloc(ctx->alloc->ctx, et_copy_len + 1);
                        if (!et_copy) {
                            ctx->alloc->free(ctx->alloc->ctx, data_copy, data_copy_len + 1);
                            return SC_ERR_OUT_OF_MEMORY;
                        }
                        memcpy(et_copy, event_type, et_copy_len + 1);
                        ctx->alloc->free(ctx->alloc->ctx, event_type, event_type_len + 1);
                        event_type = NULL;
                    } else {
                        et_copy = (char *)ctx->alloc->alloc(ctx->alloc->ctx, 8);
                        if (!et_copy) {
                            ctx->alloc->free(ctx->alloc->ctx, data_copy, data_copy_len + 1);
                            return SC_ERR_OUT_OF_MEMORY;
                        }
                        memcpy(et_copy, "message", 8);
                        et_copy_len = 7;
                    }
                    flush_event(ctx, et_copy, et_copy_len, data_copy, data_copy_len);
                    event_type = NULL;
                    event_type_len = 0;
                    data = NULL;
                    data_len = 0;
                    data_cap = 0;
                    has_data = 0;
                    total_event_size = 0;
                }
            } else if (line_start[0] != ':') {
                const char *field, *value;
                size_t field_len, value_len;
                parse_field(line_start, line_len, &field, &field_len, &value, &value_len);

                if (field_eq(field, field_len, "data")) {
                    size_t new_size = total_event_size + value_len + (has_data ? 1 : 0);
                    if (new_size > SC_SSE_MAX_EVENT_SIZE) {
                        if (event_type) {
                            ctx->alloc->free(ctx->alloc->ctx, event_type, event_type_len + 1);
                            event_type = NULL;
                        }
                        if (data) {
                            ctx->alloc->free(ctx->alloc->ctx, data, data_cap);
                            data = NULL;
                        }
                        has_data = 0;
                        total_event_size = 0;
                    } else {
                        size_t need = data_len + value_len + (has_data ? 1 : 0) + 1;
                        if (need > data_cap) {
                            size_t new_cap = data_cap ? data_cap * 2 : 256;
                            while (new_cap < need) new_cap *= 2;
                            char *nd = (char *)ctx->alloc->realloc(ctx->alloc->ctx, data,
                                data_cap ? data_cap : 0, new_cap);
                            if (!nd) return SC_ERR_OUT_OF_MEMORY;
                            data = nd;
                            data_cap = new_cap;
                        }
                        if (has_data) data[data_len++] = '\n';
                        memcpy(data + data_len, value, value_len);
                        data_len += value_len;
                        data[data_len] = '\0';
                        total_event_size = new_size;
                        has_data = 1;
                    }
                } else if (field_eq(field, field_len, "event")) {
                    if (event_type) ctx->alloc->free(ctx->alloc->ctx, event_type, event_type_len + 1);
                    event_type_len = value_len;
                    event_type = (char *)ctx->alloc->alloc(ctx->alloc->ctx, value_len + 1);
                    if (!event_type) return SC_ERR_OUT_OF_MEMORY;
                    memcpy(event_type, value, value_len);
                    event_type[value_len] = '\0';
                }
            }

            p++;
            if (p < end && line_start < end && *(p - 1) == '\r' && *p == '\n') p++;
            line_start = p;
        } else {
            p++;
        }
    }

    /* Move unprocessed data to front */
    size_t keep = (size_t)(line_start - buf);
    if (keep < len && keep > 0) {
        memmove(buf, line_start, len - keep);
        ctx->len = len - keep;
    } else if (keep == 0 && len > 0) {
        ctx->len = 0;
    }

    if (event_type && !has_data) {
        ctx->alloc->free(ctx->alloc->ctx, event_type, event_type_len + 1);
    }
    if (data && !has_data) {
        ctx->alloc->free(ctx->alloc->ctx, data, data_cap);
    }

    return SC_OK;
}

static size_t sse_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    sse_ctx_t *ctx = (sse_ctx_t *)userdata;
    size_t n = size * nmemb;
    if (n == 0) return 0;

    while (ctx->len + n + 1 > ctx->cap) {
        size_t new_cap = ctx->cap ? ctx->cap * 2 : 4096;
        while (new_cap < ctx->len + n + 1) new_cap *= 2;
        char *nbuf = (char *)ctx->alloc->realloc(ctx->alloc->ctx, ctx->buf,
            ctx->cap ? ctx->cap : 0, new_cap);
        if (!nbuf) return 0;
        ctx->buf = nbuf;
        ctx->cap = new_cap;
    }
    memcpy(ctx->buf + ctx->len, ptr, n);
    ctx->len += n;
    ctx->buf[ctx->len] = '\0';

    sc_error_t err = process_buffer(ctx);
    if (err != SC_OK) {
        ctx->last_error = err;
        return 0;  /* abort curl */
    }
    return n;
}

static void add_header(struct curl_slist **list, const char *header) {
    if (header && header[0]) *list = curl_slist_append(*list, header);
}

static sc_error_t sc_sse_connect_impl(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    const char *extra_headers,
    sc_sse_callback_t callback,
    void *callback_ctx)
{
    if (!alloc || !url || !callback) return SC_ERR_INVALID_ARGUMENT;

    CURL *curl = curl_easy_init();
    if (!curl) return SC_ERR_NOT_SUPPORTED;

    sse_ctx_t ctx = {0};
    ctx.alloc = alloc;
    ctx.callback = callback;
    ctx.callback_ctx = callback_ctx;
    ctx.buf = (char *)alloc->alloc(alloc->ctx, 4096);
    if (!ctx.buf) {
        curl_easy_cleanup(curl);
        return SC_ERR_OUT_OF_MEMORY;
    }
    ctx.cap = 4096;

    struct curl_slist *headers = NULL;
    add_header(&headers, "Accept: text/event-stream");
    char auth_buf[512];
    if (auth_header && auth_header[0]) {
        int n = snprintf(auth_buf, sizeof(auth_buf), "Authorization: %s", auth_header);
        if (n > 0 && (size_t)n < sizeof(auth_buf))
            add_header(&headers, auth_buf);
    }
    if (extra_headers && extra_headers[0]) {
        const char *p = extra_headers;
        while (*p) {
            const char *eol = strchr(p, '\n');
            size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
            if (linelen > 0 && linelen < 512) {
                char line[512];
                memcpy(line, p, linelen);
                line[linelen] = '\0';
                if (linelen > 0 && line[linelen - 1] == '\r') line[--linelen] = '\0';
                add_header(&headers, line);
            }
            if (!eol) break;
            p = eol + 1;
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);  /* no timeout for streaming */

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    alloc->free(alloc->ctx, ctx.buf, ctx.cap);

    if (ctx.last_error != SC_OK) return ctx.last_error;
    if (res != CURLE_OK) {
        if (res == CURLE_OPERATION_TIMEDOUT) return SC_ERR_TIMEOUT;
        return SC_ERR_IO;
    }
    return SC_OK;
}
#else
static sc_error_t sc_sse_connect_impl(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    const char *extra_headers,
    sc_sse_callback_t callback,
    void *callback_ctx)
{
    (void)alloc;
    (void)url;
    (void)auth_header;
    (void)extra_headers;
    (void)callback;
    (void)callback_ctx;
    return SC_ERR_NOT_SUPPORTED;
}
#endif
#endif

sc_error_t sc_sse_connect(sc_allocator_t *alloc,
    const char *url,
    const char *auth_header,
    const char *extra_headers,
    sc_sse_callback_t callback,
    void *callback_ctx)
{
    return sc_sse_connect_impl(alloc, url, auth_header, extra_headers, callback, callback_ctx);
}
