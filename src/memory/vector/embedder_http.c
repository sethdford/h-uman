/* embedder_http.c — real embeddings over HTTP. See the header. */
#include "human/memory/vector/embedder_http.h"

#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/log.h"

#include <stdio.h>
#include <string.h>

#define HU_EMBED_HTTP_MODEL     "nomic-embed-text-v2"
#define HU_EMBED_HTTP_MAX_BATCH 64u

typedef struct http_embedder_ctx {
    hu_allocator_t *alloc;
    char url[512];
    size_t dim; /* learned from the first successful response; 0 = unknown */
} http_embedder_ctx_t;

static void free_embeddings(hu_allocator_t *alloc, hu_embedding_t *v, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (v[i].values)
            alloc->free(alloc->ctx, v[i].values, v[i].dim * sizeof(float));
        v[i].values = NULL;
        v[i].dim = 0;
    }
}

hu_error_t hu_embedder_http_parse_response(hu_allocator_t *alloc, const char *body, size_t body_len,
                                           size_t expect_count, hu_embedding_t *out) {
    if (!alloc || !body || body_len == 0 || expect_count == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < expect_count; i++) {
        out[i].values = NULL;
        out[i].dim = 0;
    }
    hu_json_value_t *root = NULL;
    if (hu_json_parse(alloc, body, body_len, &root) != HU_OK || !root)
        return HU_ERR_PROVIDER_RESPONSE;
    hu_error_t err = HU_ERR_PROVIDER_RESPONSE;
    hu_json_value_t *data = hu_json_object_get(root, "data");
    if (!data || data->type != HU_JSON_ARRAY || data->data.array.len != expect_count)
        goto done;
    size_t dim = 0;
    for (size_t i = 0; i < expect_count; i++) {
        hu_json_value_t *item = data->data.array.items[i];
        hu_json_value_t *emb = item ? hu_json_object_get(item, "embedding") : NULL;
        if (!emb || emb->type != HU_JSON_ARRAY)
            goto fail;
        size_t n = emb->data.array.len;
        if (n == 0 || (dim && n != dim))
            goto fail; /* ragged or empty: a zero-length vector would score as 0 */
        dim = n;
        /* index may be absent; when present it must match position */
        hu_json_value_t *idx = hu_json_object_get(item, "index");
        if (idx && idx->type == HU_JSON_NUMBER && (size_t)idx->data.number != i)
            goto fail;
        float *vals = (float *)alloc->alloc(alloc->ctx, n * sizeof(float));
        if (!vals) {
            err = HU_ERR_OUT_OF_MEMORY;
            goto fail;
        }
        for (size_t k = 0; k < n; k++) {
            hu_json_value_t *x = emb->data.array.items[k];
            if (!x || x->type != HU_JSON_NUMBER) {
                alloc->free(alloc->ctx, vals, n * sizeof(float));
                goto fail;
            }
            vals[k] = (float)x->data.number;
        }
        out[i].values = vals;
        out[i].dim = n;
    }
    err = HU_OK;
    goto done;
fail:
    free_embeddings(alloc, out, expect_count);
done:
    hu_json_free(alloc, root);
    return err;
}

static hu_error_t embed_batch_impl(void *vctx, hu_allocator_t *alloc, const char **texts,
                                   const size_t *text_lens, size_t count, hu_embedding_t *out) {
    http_embedder_ctx_t *ctx = (http_embedder_ctx_t *)vctx;
    if (!ctx || !alloc || !texts || !text_lens || count == 0 || count > HU_EMBED_HTTP_MAX_BATCH ||
        !out)
        return HU_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < count; i++) {
        out[i].values = NULL;
        out[i].dim = 0;
        if (!texts[i] || text_lens[i] == 0)
            return HU_ERR_INVALID_ARGUMENT; /* the server rejects empty input; fail early */
    }
    /* Build {"model":..., "input":[...]} with hu_json so escaping is correct. */
    hu_json_value_t *req = hu_json_object_new(alloc);
    hu_json_value_t *arr = hu_json_array_new(alloc);
    if (!req || !arr) {
        if (req)
            hu_json_free(alloc, req);
        if (arr)
            hu_json_free(alloc, arr);
        return HU_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < count; i++) {
        hu_json_value_t *s = hu_json_string_new(alloc, texts[i], text_lens[i]);
        if (!s || hu_json_array_push(alloc, arr, s) != HU_OK) {
            if (s)
                hu_json_free(alloc, s);
            hu_json_free(alloc, req);
            hu_json_free(alloc, arr);
            return HU_ERR_OUT_OF_MEMORY;
        }
    }
    hu_json_object_set(alloc, req, "model",
                       hu_json_string_new(alloc, HU_EMBED_HTTP_MODEL, strlen(HU_EMBED_HTTP_MODEL)));
    hu_json_object_set(alloc, req, "input", arr);
    char *body = NULL;
    size_t body_len = 0;
    hu_error_t err = hu_json_stringify(alloc, req, &body, &body_len);
    hu_json_free(alloc, req);
    if (err != HU_OK || !body)
        return err != HU_OK ? err : HU_ERR_OUT_OF_MEMORY;

    hu_http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    err = hu_http_post_json_ex(alloc, ctx->url, NULL, "X-HU-Priority: batch\r\n", body, body_len,
                               &resp);
    alloc->free(alloc->ctx, body, body_len + 1);
    if (err != HU_OK) {
        hu_log_warn("embedder.http", NULL, "embeddings POST failed: %s", hu_error_string(err));
        if (resp.body)
            alloc->free(alloc->ctx, resp.body, resp.body_cap);
        return HU_ERR_PROVIDER_UNAVAILABLE;
    }
    if (resp.status_code != 200) {
        hu_log_warn("embedder.http", NULL, "embeddings HTTP %ld: %.*s", resp.status_code,
                    (int)(resp.body_len > 160 ? 160 : resp.body_len), resp.body ? resp.body : "");
        if (resp.body)
            alloc->free(alloc->ctx, resp.body, resp.body_cap);
        return HU_ERR_PROVIDER_RESPONSE;
    }
    err = hu_embedder_http_parse_response(alloc, resp.body, resp.body_len, count, out);
    if (resp.body)
        alloc->free(alloc->ctx, resp.body, resp.body_cap);
    if (err == HU_OK && ctx->dim == 0)
        ctx->dim = out[0].dim;
    return err;
}

static hu_error_t embed_impl(void *vctx, hu_allocator_t *alloc, const char *text, size_t text_len,
                             hu_embedding_t *out) {
    const char *texts[1] = {text};
    size_t lens[1] = {text_len};
    return embed_batch_impl(vctx, alloc, texts, lens, 1, out);
}

static size_t dimensions_impl(void *vctx) {
    http_embedder_ctx_t *ctx = (http_embedder_ctx_t *)vctx;
    return ctx ? ctx->dim : 0;
}

static void deinit_impl(void *vctx, hu_allocator_t *alloc) {
    if (vctx && alloc)
        alloc->free(alloc->ctx, vctx, sizeof(http_embedder_ctx_t));
}

static const hu_embedder_vtable_t http_vtable = {
    .embed = embed_impl,
    .embed_batch = embed_batch_impl,
    .dimensions = dimensions_impl,
    .deinit = deinit_impl,
};

hu_embedder_t hu_embedder_http_create(hu_allocator_t *alloc, const char *base_url) {
    hu_embedder_t e = {.ctx = NULL, .vtable = &http_vtable};
    if (!alloc || !base_url || !base_url[0])
        return e;
    http_embedder_ctx_t *ctx = (http_embedder_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*ctx));
    if (!ctx)
        return e;
    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = alloc;
    size_t bl = strlen(base_url);
    while (bl > 0 && base_url[bl - 1] == '/')
        bl--;
    int n = snprintf(ctx->url, sizeof(ctx->url), "%.*s/v1/embeddings", (int)bl, base_url);
    if (n <= 0 || (size_t)n >= sizeof(ctx->url)) {
        alloc->free(alloc->ctx, ctx, sizeof(*ctx));
        return e;
    }
    e.ctx = ctx;
    return e;
}
