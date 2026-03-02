#include "seaclaw/memory/vector/embeddings_gemini.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GEMINI_DEFAULT_MODEL "text-embedding-004"
#define GEMINI_DEFAULT_DIMS 768
#define GEMINI_BASE_URL "https://generativelanguage.googleapis.com"

typedef struct gemini_ctx {
    sc_allocator_t *alloc;
    char *base_url;
    char *api_key;
    char *model;
    size_t dims;
} gemini_ctx_t;

#if SC_IS_TEST
static sc_error_t gemini_embed(void *ctx, sc_allocator_t *alloc,
    const char *text, size_t text_len,
    sc_embedding_provider_result_t *out) {
    (void)ctx;
    (void)text;
    (void)text_len;
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    out->values = (float *)alloc->alloc(alloc->ctx, 3 * sizeof(float));
    if (!out->values) return SC_ERR_OUT_OF_MEMORY;
    out->values[0] = 0.1f;
    out->values[1] = 0.2f;
    out->values[2] = 0.3f;
    out->dimensions = 3;
    return SC_OK;
}
#else
/* Parse Gemini response: {"embedding":{"values":[0.1,0.2,...]}} */
static sc_error_t parse_gemini_response(sc_allocator_t *alloc,
    const char *json_body, size_t json_len,
    sc_embedding_provider_result_t *out) {
    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(alloc, json_body, json_len, &root);
    if (err != SC_OK || !root || root->type != SC_JSON_OBJECT)
        return SC_ERR_JSON_PARSE;

    sc_json_value_t *emb = sc_json_object_get(root, "embedding");
    if (!emb || emb->type != SC_JSON_OBJECT) {
        sc_json_free(alloc, root);
        return SC_ERR_JSON_PARSE;
    }
    sc_json_value_t *vals = sc_json_object_get(emb, "values");
    if (!vals || vals->type != SC_JSON_ARRAY) {
        sc_json_free(alloc, root);
        return SC_ERR_JSON_PARSE;
    }

    size_t n = vals->data.array.len;
    float *arr = (float *)alloc->alloc(alloc->ctx, n * sizeof(float));
    if (!arr) {
        sc_json_free(alloc, root);
        return SC_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < n; i++) {
        sc_json_value_t *item = vals->data.array.items[i];
        if (item->type == SC_JSON_NUMBER)
            arr[i] = (float)item->data.number;
        else if (item->type == SC_JSON_STRING) {
            /* allow "1" style */
            arr[i] = (float)atof(item->data.string.ptr);
        } else {
            alloc->free(alloc->ctx, arr, n * sizeof(float));
            sc_json_free(alloc, root);
            return SC_ERR_JSON_PARSE;
        }
    }

    sc_json_free(alloc, root);
    out->values = arr;
    out->dimensions = n;
    return SC_OK;
}

static sc_error_t gemini_embed(void *ctx, sc_allocator_t *alloc,
    const char *text, size_t text_len,
    sc_embedding_provider_result_t *out) {
    gemini_ctx_t *g = (gemini_ctx_t *)ctx;
    if (!alloc || !out || !g) return SC_ERR_INVALID_ARGUMENT;

    if (text_len == 0) {
        out->values = (float *)alloc->alloc(alloc->ctx, 0);
        out->dimensions = 0;
        return SC_OK;
    }

    /* Build body: {"model":"models/xxx","content":{"parts":[{"text":"..."}]}} */
    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != SC_OK) return SC_ERR_OUT_OF_MEMORY;

    char model_prefix[256];
    int mlen = snprintf(model_prefix, sizeof(model_prefix), "models/%s", g->model);
    if (mlen >= (int)sizeof(model_prefix) || mlen < 0) goto fail;
    if (sc_json_append_key_value(&buf, "model", 5, model_prefix, (size_t)mlen) != SC_OK) goto fail;
    if (sc_json_buf_append_raw(&buf, ",\"content\":{\"parts\":[{\"text\":", 32) != SC_OK) goto fail;
    if (sc_json_append_string(&buf, text, text_len) != SC_OK) goto fail;
    if (sc_json_buf_append_raw(&buf, "}]}}", 5) != SC_OK) goto fail;

    char *body = buf.ptr;
    size_t body_len = buf.len;

    /* URL: base/v1beta/models/{model}:embedContent?key={api_key} */
    char url[512];
    int ulen = snprintf(url, sizeof(url), "%s/v1beta/models/%s:embedContent?key=%s",
        g->base_url, g->model, g->api_key);
    if (ulen >= (int)sizeof(url) || ulen < 0) {
        sc_json_buf_free(&buf);
        return SC_ERR_INVALID_ARGUMENT;
    }

    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_post_json(alloc, url, NULL, body, body_len, &resp);
    sc_json_buf_free(&buf);
    if (err != SC_OK) return err;
    if (resp.status_code != 200 || !resp.body) {
        sc_http_response_free(alloc, &resp);
        return SC_ERR_JSON_PARSE;
    }

    sc_error_t parse_err = parse_gemini_response(alloc, resp.body, resp.body_len, out);
    sc_http_response_free(alloc, &resp);
    return parse_err;

fail:
    sc_json_buf_free(&buf);
    return SC_ERR_OUT_OF_MEMORY;
}
#endif

static const char *gemini_name(void *ctx) { (void)ctx; return "gemini"; }

static size_t gemini_dimensions(void *ctx) {
    gemini_ctx_t *g = (gemini_ctx_t *)ctx;
    return g ? g->dims : GEMINI_DEFAULT_DIMS;
}

static void gemini_deinit(void *ctx, sc_allocator_t *alloc) {
    gemini_ctx_t *g = (gemini_ctx_t *)ctx;
    if (!g || !alloc) return;
    if (g->base_url) alloc->free(alloc->ctx, g->base_url, strlen(g->base_url) + 1);
    if (g->api_key) alloc->free(alloc->ctx, g->api_key, strlen(g->api_key) + 1);
    if (g->model) alloc->free(alloc->ctx, g->model, strlen(g->model) + 1);
    alloc->free(alloc->ctx, g, sizeof(gemini_ctx_t));
}

static const sc_embedding_provider_vtable_t gemini_vtable = {
    .embed = gemini_embed,
    .name = gemini_name,
    .dimensions = gemini_dimensions,
    .deinit = gemini_deinit,
};

sc_embedding_provider_t sc_embedding_gemini_create(sc_allocator_t *alloc,
    const char *api_key,
    const char *model,
    size_t dims) {
    sc_embedding_provider_t p = { .ctx = NULL, .vtable = &gemini_vtable };
    if (!alloc) return p;

    gemini_ctx_t *g = (gemini_ctx_t *)alloc->alloc(alloc->ctx, sizeof(gemini_ctx_t));
    if (!g) return p;
    memset(g, 0, sizeof(*g));
    g->alloc = alloc;
    g->base_url = sc_strdup(alloc, GEMINI_BASE_URL);
    g->api_key = sc_strdup(alloc, api_key ? api_key : "");
    g->model = sc_strdup(alloc, (model && model[0]) ? model : GEMINI_DEFAULT_MODEL);
    g->dims = (dims > 0) ? dims : GEMINI_DEFAULT_DIMS;

    if (!g->base_url || !g->api_key || !g->model) {
        gemini_deinit(g, alloc);
        return sc_embedding_provider_noop_create(alloc);
    }

    p.ctx = g;
    return p;
}
