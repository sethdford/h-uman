#include "seaclaw/memory/vector/embeddings_ollama.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OLLAMA_DEFAULT_BASE "http://localhost:11434"
#define OLLAMA_DEFAULT_MODEL "nomic-embed-text"
#define OLLAMA_DEFAULT_DIMS 768

typedef struct ollama_ctx {
    sc_allocator_t *alloc;
    char *base_url;
    char *model;
    size_t dims;
} ollama_ctx_t;

#if SC_IS_TEST
static sc_error_t ollama_embed(void *ctx, sc_allocator_t *alloc,
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
/* Parse Ollama: {"embeddings":[[0.1,0.2,...]]} */
static sc_error_t parse_ollama_response(sc_allocator_t *alloc,
    const char *json_body, size_t json_len,
    sc_embedding_provider_result_t *out) {
    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(alloc, json_body, json_len, &root);
    if (err != SC_OK || !root || root->type != SC_JSON_OBJECT)
        return SC_ERR_JSON_PARSE;

    sc_json_value_t *embs = sc_json_object_get(root, "embeddings");
    if (!embs || embs->type != SC_JSON_ARRAY || embs->data.array.len == 0) {
        sc_json_free(alloc, root);
        return SC_ERR_JSON_PARSE;
    }

    sc_json_value_t *inner = embs->data.array.items[0];
    if (!inner || inner->type != SC_JSON_ARRAY) {
        sc_json_free(alloc, root);
        return SC_ERR_JSON_PARSE;
    }

    size_t n = inner->data.array.len;
    float *arr = (float *)alloc->alloc(alloc->ctx, n * sizeof(float));
    if (!arr) {
        sc_json_free(alloc, root);
        return SC_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < n; i++) {
        sc_json_value_t *item = inner->data.array.items[i];
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

static sc_error_t ollama_embed(void *ctx, sc_allocator_t *alloc,
    const char *text, size_t text_len,
    sc_embedding_provider_result_t *out) {
    ollama_ctx_t *o = (ollama_ctx_t *)ctx;
    if (!alloc || !out || !o) return SC_ERR_INVALID_ARGUMENT;

    if (text_len == 0) {
        out->values = (float *)alloc->alloc(alloc->ctx, 0);
        out->dimensions = 0;
        return SC_OK;
    }

    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != SC_OK) return SC_ERR_OUT_OF_MEMORY;

    if (sc_json_append_key_value(&buf, "model", 5, o->model, strlen(o->model)) != SC_OK) goto fail;
    if (sc_json_buf_append_raw(&buf, ",\"input\":", 9) != SC_OK) goto fail;
    if (sc_json_append_string(&buf, text, text_len) != SC_OK) goto fail;

    char url[384];
    int ulen = snprintf(url, sizeof(url), "%s/api/embeddings", o->base_url);
    if (ulen >= (int)sizeof(url) || ulen < 0) { sc_json_buf_free(&buf); return SC_ERR_INVALID_ARGUMENT; }

    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_post_json(alloc, url, NULL, buf.ptr, buf.len, &resp);
    sc_json_buf_free(&buf);
    if (err != SC_OK) return err;
    if (resp.status_code != 200 || !resp.body) {
        sc_http_response_free(alloc, &resp);
        return SC_ERR_JSON_PARSE;
    }

    sc_error_t parse_err = parse_ollama_response(alloc, resp.body, resp.body_len, out);
    sc_http_response_free(alloc, &resp);
    return parse_err;

fail:
    sc_json_buf_free(&buf);
    return SC_ERR_OUT_OF_MEMORY;
}
#endif

static const char *ollama_name(void *ctx) { (void)ctx; return "ollama"; }
static size_t ollama_dimensions(void *ctx) {
    ollama_ctx_t *o = (ollama_ctx_t *)ctx;
    return o ? o->dims : OLLAMA_DEFAULT_DIMS;
}
static void ollama_deinit(void *ctx, sc_allocator_t *alloc) {
    ollama_ctx_t *o = (ollama_ctx_t *)ctx;
    if (!o || !alloc) return;
    if (o->base_url) alloc->free(alloc->ctx, o->base_url, strlen(o->base_url) + 1);
    if (o->model) alloc->free(alloc->ctx, o->model, strlen(o->model) + 1);
    alloc->free(alloc->ctx, o, sizeof(ollama_ctx_t));
}

static const sc_embedding_provider_vtable_t ollama_vtable = {
    .embed = ollama_embed,
    .name = ollama_name,
    .dimensions = ollama_dimensions,
    .deinit = ollama_deinit,
};

sc_embedding_provider_t sc_embedding_ollama_create(sc_allocator_t *alloc,
    const char *model,
    size_t dims) {
    sc_embedding_provider_t p = { .ctx = NULL, .vtable = &ollama_vtable };
    if (!alloc) return p;

    ollama_ctx_t *o = (ollama_ctx_t *)alloc->alloc(alloc->ctx, sizeof(ollama_ctx_t));
    if (!o) return p;
    memset(o, 0, sizeof(*o));
    o->alloc = alloc;
    o->base_url = sc_strdup(alloc, OLLAMA_DEFAULT_BASE);
    o->model = sc_strdup(alloc, (model && model[0]) ? model : OLLAMA_DEFAULT_MODEL);
    o->dims = (dims > 0) ? dims : OLLAMA_DEFAULT_DIMS;

    if (!o->base_url || !o->model) {
        ollama_deinit(o, alloc);
        return sc_embedding_provider_noop_create(alloc);
    }
    p.ctx = o;
    return p;
}
