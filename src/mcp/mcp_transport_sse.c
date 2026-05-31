#include "human/core/http.h"
#include "human/core/string.h"
#include "human/mcp_transport.h"
#include <string.h>

typedef struct sse_ctx {
    char *url;
    char *pending_data;
    size_t pending_len;
} sse_ctx_t;

#ifdef HU_HTTP_CURL
static hu_error_t sse_send(void *ctx, const char *data, size_t len) {
    sse_ctx_t *c = (sse_ctx_t *)ctx;
    if (!c || !c->url || !data)
        return HU_ERR_INVALID_ARGUMENT;
    hu_allocator_t alloc = hu_system_allocator();
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_post_json(&alloc, c->url, NULL, data, len, &resp);
    if (resp.owned && resp.body)
        hu_http_response_free(&alloc, &resp);
    return err;
}

static hu_error_t sse_recv(void *ctx, hu_allocator_t *alloc, char **out, size_t *out_len) {
    sse_ctx_t *c = (sse_ctx_t *)ctx;
    if (!c || !c->url || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    hu_http_response_t resp = {0};
    char accept_hdr[] = "Accept: text/event-stream";
    hu_error_t err = hu_http_get(alloc, c->url, accept_hdr, &resp);
    if (err != HU_OK || !resp.body || resp.body_len == 0) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return err != HU_OK ? err : HU_ERR_IO;
    }

    /* Extract the last "data:" line from the SSE stream */
    const char *last_data = NULL;
    size_t last_data_len = 0;
    const char *p = resp.body;
    const char *end = resp.body + resp.body_len;
    while (p < end) {
        if (end - p >= 5 && memcmp(p, "data:", 5) == 0) {
            const char *val = p + 5;
            while (val < end && *val == ' ')
                val++;
            const char *eol = val;
            while (eol < end && *eol != '\n' && *eol != '\r')
                eol++;
            last_data = val;
            last_data_len = (size_t)(eol - val);
            p = eol;
        }
        while (p < end && *p != '\n')
            p++;
        if (p < end)
            p++;
    }

    if (!last_data || last_data_len == 0) {
        hu_http_response_free(alloc, &resp);
        *out = NULL;
        *out_len = 0;
        return HU_OK;
    }

    char *result = hu_strndup(alloc, last_data, last_data_len);
    hu_http_response_free(alloc, &resp);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;
    *out = result;
    *out_len = last_data_len;
    return HU_OK;
}

static void sse_close(void *ctx, hu_allocator_t *alloc) {
    sse_ctx_t *c = (sse_ctx_t *)ctx;
    if (!c || !alloc)
        return;
    if (c->url) {
        alloc->free(alloc->ctx, c->url, strlen(c->url) + 1);
        c->url = NULL;
    }
    alloc->free(alloc->ctx, c, sizeof(*c));
}
#endif

hu_error_t hu_mcp_transport_sse_create(hu_allocator_t *alloc, const char *url, size_t url_len,
                                       hu_mcp_transport_t *out) {
    if (!alloc || !out || !url || url_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
#ifdef HU_HTTP_CURL
    sse_ctx_t *c = (sse_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    c->url = hu_strndup(alloc, url, url_len);
    if (!c->url) {
        alloc->free(alloc->ctx, c, sizeof(*c));
        return HU_ERR_OUT_OF_MEMORY;
    }
    out->ctx = c;
    out->send = sse_send;
    out->recv = sse_recv;
    out->close = sse_close;
    return HU_OK;
#else
    (void)url;
    (void)url_len;
    return HU_ERR_NOT_SUPPORTED;
#endif
}
