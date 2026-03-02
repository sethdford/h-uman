#include "seaclaw/memory/vector/embeddings_voyage.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VOYAGE_DEFAULT_BASE "https://api.voyageai.com"
#define VOYAGE_DEFAULT_MODEL "voyage-3-lite"
#define VOYAGE_DEFAULT_DIMS 512

typedef struct voyage_ctx {
    sc_allocator_t *alloc;
    char *base_url;
    char *api_key;
    char *model;
    size_t dims;
} voyage_ctx_t;

#if SC_IS_TEST
static sc_error_t voyage_embed(void *ctx, sc_allocator_t *alloc,
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
/* Parse Voyage/OpenAI: {"data":[{"embedding":[0.1,0.2,...]}]} */
static sc_error_t parse_voyage_response(sc_allocator_t *alloc,
    const char *json_body, size_t json_len,
    sc_embedding_provider_result_t *out) {
    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(alloc, json_body, json_len, &root);
    if (err != SC_OK || !root || root->type != SC_JSON_OBJECT)
        return SC_ERR_JSON_PARSE;

    sc_json_value_t *data = sc_json_object_get(root, "data");
    if (!data || data->type != SC_JSON_ARRAY || data->data.array.len == 0) {
        sc_json_free(alloc, root);
        return SC_ERR_JSON_PARSE;
    }

    sc_json_value_t *first = data->data.array.items[0];
    if (!first || first->type != SC_JSON_OBJECT) {
        sc_json_free(alloc, root);
        return SC_ERR_JSON_PARSE;
    }

    sc_json_value_t *emb = sc_json_object_get(first, "embedding");
    if (!emb || emb->type != SC_JSON_ARRAY) {
        sc_json_free(alloc, root);
        return SC_ERR_JSON_PARSE;
    }

    size_t n = emb->data.array.len;
    float *arr = (float *)alloc->alloc(alloc->ctx, n * sizeof(float));
    if (!arr) {
        sc_json_free(alloc, root);
        return SC_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < n; i++) {
        sc_json_value_t *item = emb->data.array.items[i];
        if (item->type == SC_JSON_NUMBER)
            arr[i] = (float)item->data.number;
        else {
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

static sc_error_t voyage_embed(void *ctx, sc_allocator_t *alloc,
    const char *text, size_t text_len,
    sc_embedding_provider_result_t *out) {
    voyage_ctx_t *v = (voyage_ctx_t *)ctx;
    if (!alloc || !out || !v) return SC_ERR_INVALID_ARGUMENT;

    if (text_len == 0) {
        out->values = (float *)alloc->alloc(alloc->ctx, 0);
        out->dimensions = 0;
        return SC_OK;
    }

    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != SC_OK) return SC_ERR_OUT_OF_MEMORY;
    if (sc_json_append_key_value(&buf, "model", 5, v->model, strlen(v->model)) != SC_OK) goto fail;
    if (sc_json_buf_append_raw(&buf, ",\"input\":[\"", 11) != SC_OK) goto fail;
    if (sc_json_append_string(&buf, text, text_len) != SC_OK) goto fail;
    if (sc_json_buf_append_raw(&buf, "\"],\"input_type\":\"query\"}", 24) != SC_OK) goto fail;

    char url[384];
    int ulen = snprintf(url, sizeof(url), "%s/v1/embeddings", v->base_url);
    if (ulen >= (int)sizeof(url) || ulen < 0) { sc_json_buf_free(&buf); return SC_ERR_INVALID_ARGUMENT; }

    char auth[256];
    int alen = snprintf(auth, sizeof(auth), "Bearer %s", v->api_key);
    if (alen >= (int)sizeof(auth) || alen < 0) { sc_json_buf_free(&buf); return SC_ERR_INVALID_ARGUMENT; }

    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_post_json(alloc, url, auth, buf.ptr, buf.len, &resp);
    sc_json_buf_free(&buf);
    if (err != SC_OK) return err;
    if (resp.status_code != 200 || !resp.body) {
        sc_http_response_free(alloc, &resp);
        return SC_ERR_JSON_PARSE;
    }

    sc_error_t parse_err = parse_voyage_response(alloc, resp.body, resp.body_len, out);
    sc_http_response_free(alloc, &resp);
    return parse_err;

fail:
    sc_json_buf_free(&buf);
    return SC_ERR_OUT_OF_MEMORY;
}
#endif

static const char *voyage_name(void *ctx) { (void)ctx; return "voyage"; }
static size_t voyage_dimensions(void *ctx) {
    voyage_ctx_t *v = (voyage_ctx_t *)ctx;
    return v ? v->dims : VOYAGE_DEFAULT_DIMS;
}
static void voyage_deinit(void *ctx, sc_allocator_t *alloc) {
    voyage_ctx_t *v = (voyage_ctx_t *)ctx;
    if (!v || !alloc) return;
    if (v->base_url) alloc->free(alloc->ctx, v->base_url, strlen(v->base_url) + 1);
    if (v->api_key) alloc->free(alloc->ctx, v->api_key, strlen(v->api_key) + 1);
    if (v->model) alloc->free(alloc->ctx, v->model, strlen(v->model) + 1);
    alloc->free(alloc->ctx, v, sizeof(voyage_ctx_t));
}

static const sc_embedding_provider_vtable_t voyage_vtable = {
    .embed = voyage_embed,
    .name = voyage_name,
    .dimensions = voyage_dimensions,
    .deinit = voyage_deinit,
};

sc_embedding_provider_t sc_embedding_voyage_create(sc_allocator_t *alloc,
    const char *api_key,
    const char *model,
    size_t dims) {
    sc_embedding_provider_t p = { .ctx = NULL, .vtable = &voyage_vtable };
    if (!alloc) return p;

    voyage_ctx_t *v = (voyage_ctx_t *)alloc->alloc(alloc->ctx, sizeof(voyage_ctx_t));
    if (!v) return p;
    memset(v, 0, sizeof(*v));
    v->alloc = alloc;
    v->base_url = sc_strdup(alloc, VOYAGE_DEFAULT_BASE);
    v->api_key = sc_strdup(alloc, api_key ? api_key : "");
    v->model = sc_strdup(alloc, (model && model[0]) ? model : VOYAGE_DEFAULT_MODEL);
    v->dims = (dims > 0) ? dims : VOYAGE_DEFAULT_DIMS;

    if (!v->base_url || !v->api_key || !v->model) {
        voyage_deinit(v, alloc);
        return sc_embedding_provider_noop_create(alloc);
    }
    p.ctx = v;
    return p;
}
