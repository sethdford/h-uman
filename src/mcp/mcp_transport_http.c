#include "human/core/allocator.h"
#include "human/core/http.h"
#include "human/core/string.h"
#include "human/mcp_transport.h"
#include <string.h>

typedef struct http_ctx {
    hu_allocator_t *alloc;
    char *url;
    char *last_response;
    size_t last_response_len;
} http_ctx_t;

#ifdef HU_HTTP_CURL
static hu_error_t http_send(void *ctx, const char *data, size_t len) {
    http_ctx_t *c = (http_ctx_t *)ctx;
    if (!c || !c->url || !data)
        return HU_ERR_INVALID_ARGUMENT;
    hu_allocator_t *alloc = c->alloc;
    if (c->last_response) {
        alloc->free(alloc->ctx, c->last_response, c->last_response_len + 1);
        c->last_response = NULL;
        c->last_response_len = 0;
    }
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_post_json(alloc, c->url, NULL, data, len, &resp);
    if (err != HU_OK)
        return err;
    if (resp.status_code < 200 || resp.status_code >= 300) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }
    if (resp.body && resp.body_len > 0) {
        c->last_response = hu_strndup(alloc, resp.body, resp.body_len);
        c->last_response_len = resp.body_len;
    }
    if (resp.owned && resp.body)
        hu_http_response_free(alloc, &resp);
    return HU_OK;
}

static hu_error_t http_recv(void *ctx, hu_allocator_t *alloc, char **out, size_t *out_len) {
    http_ctx_t *c = (http_ctx_t *)ctx;
    if (!c || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!c->last_response) {
        *out = NULL;
        *out_len = 0;
        return HU_OK;
    }
    *out = hu_strndup(alloc, c->last_response, c->last_response_len);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    *out_len = c->last_response_len;
    return HU_OK;
}

static void http_close(void *ctx, hu_allocator_t *alloc) {
    http_ctx_t *c = (http_ctx_t *)ctx;
    if (!c || !alloc)
        return;
    if (c->url) {
        alloc->free(alloc->ctx, c->url, strlen(c->url) + 1);
        c->url = NULL;
    }
    if (c->last_response) {
        alloc->free(alloc->ctx, c->last_response, c->last_response_len + 1);
        c->last_response = NULL;
        c->last_response_len = 0;
    }
    alloc->free(alloc->ctx, c, sizeof(*c));
}
#endif

hu_error_t hu_mcp_transport_http_create(hu_allocator_t *alloc, const char *url, size_t url_len,
                                        hu_mcp_transport_t *out) {
    if (!alloc || !out || !url || url_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
#ifdef HU_HTTP_CURL
    http_ctx_t *c = (http_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->url = hu_strndup(alloc, url, url_len);
    if (!c->url) {
        alloc->free(alloc->ctx, c, sizeof(*c));
        return HU_ERR_OUT_OF_MEMORY;
    }
    out->ctx = c;
    out->send = http_send;
    out->recv = http_recv;
    out->close = http_close;
    return HU_OK;
#else
    (void)url;
    (void)url_len;
    return HU_ERR_NOT_SUPPORTED;
#endif
}
