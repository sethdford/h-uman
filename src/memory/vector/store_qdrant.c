#include "human/memory/vector/store_qdrant.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct qdrant_ctx {
    hu_allocator_t *alloc;
    char *url;
    char *api_key;
    char *collection_name;
    size_t dimensions;
} qdrant_ctx_t;

#if HU_IS_TEST
static hu_error_t qdrant_upsert(void *ctx, hu_allocator_t *alloc, const char *id, size_t id_len,
                                const float *embedding, size_t dims, const char *metadata,
                                size_t metadata_len) {
    (void)ctx;
    (void)alloc;
    (void)id;
    (void)id_len;
    (void)embedding;
    (void)dims;
    (void)metadata;
    (void)metadata_len;
    return HU_OK;
}

static hu_error_t qdrant_search(void *ctx, hu_allocator_t *alloc, const float *query_embedding,
                                size_t dims, size_t limit, hu_vector_search_result_t **results,
                                size_t *result_count) {
    (void)ctx;
    (void)alloc;
    (void)query_embedding;
    (void)dims;
    (void)limit;
    *results = NULL;
    *result_count = 0;
    return HU_OK;
}

static hu_error_t qdrant_delete(void *ctx, hu_allocator_t *alloc, const char *id, size_t id_len) {
    (void)ctx;
    (void)alloc;
    (void)id;
    (void)id_len;
    return HU_OK;
}

static size_t qdrant_count(void *ctx) {
    (void)ctx;
    return 0;
}
#else
static hu_error_t qdrant_upsert(void *ctx, hu_allocator_t *alloc, const char *id, size_t id_len,
                                const float *embedding, size_t dims, const char *metadata,
                                size_t metadata_len) {
    qdrant_ctx_t *q = (qdrant_ctx_t *)ctx;
    if (!q || !embedding)
        return HU_ERR_INVALID_ARGUMENT;
    (void)metadata;
    (void)metadata_len;

    char url[512];
    snprintf(url, sizeof(url), "%s/collections/%s/points?wait=true", q->url, q->collection_name);

    char body[4096];
    size_t pos =
        snprintf(body, sizeof(body), "{\"points\":[{\"id\":\"%.*s\",\"vector\":[", (int)id_len, id);
    for (size_t i = 0; i < dims && pos < sizeof(body) - 32; i++) {
        if (i > 0)
            body[pos++] = ',';
        pos += snprintf(body + pos, sizeof(body) - pos, "%f", embedding[i]);
    }
    snprintf(body + pos, sizeof(body) - pos, "],\"payload\":{\"key\":\"%.*s\"}}]}", (int)id_len,
             id);

    hu_http_response_t resp = {0};
    const char *auth = q->api_key && q->api_key[0] ? q->api_key : NULL;
    hu_error_t err = hu_http_post_json(alloc, url, auth, body, strlen(body), &resp);
    if (err != HU_OK)
        return err;
    long status = resp.status_code;
    hu_http_response_free(alloc, &resp);
    return status == 200 ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

static hu_error_t qdrant_search(void *ctx, hu_allocator_t *alloc, const float *query_embedding,
                                size_t dims, size_t limit, hu_vector_search_result_t **results,
                                size_t *result_count) {
    qdrant_ctx_t *q = (qdrant_ctx_t *)ctx;
    if (!q || !query_embedding || !results || !result_count)
        return HU_ERR_INVALID_ARGUMENT;

    char url[512];
    snprintf(url, sizeof(url), "%s/collections/%s/points/search", q->url, q->collection_name);

    char body[4096];
    size_t pos = snprintf(body, sizeof(body), "{\"vector\":[");
    for (size_t i = 0; i < dims && pos < sizeof(body) - 32; i++) {
        if (i > 0)
            body[pos++] = ',';
        pos += snprintf(body + pos, sizeof(body) - pos, "%f", query_embedding[i]);
    }
    snprintf(body + pos, sizeof(body) - pos, "],\"limit\":%zu,\"with_payload\":true}", limit);

    hu_http_response_t resp = {0};
    const char *auth = q->api_key && q->api_key[0] ? q->api_key : NULL;
    hu_error_t err = hu_http_post_json(alloc, url, auth, body, strlen(body), &resp);
    if (err != HU_OK) {
        *results = NULL;
        *result_count = 0;
        return err;
    }
    if (resp.status_code != 200 || !resp.body || resp.body_len == 0) {
        hu_http_response_free(alloc, &resp);
        *results = NULL;
        *result_count = 0;
        return HU_ERR_MEMORY_BACKEND;
    }

    hu_json_value_t *parsed = NULL;
    hu_error_t parse_err = hu_json_parse(alloc, resp.body, resp.body_len, &parsed);
    hu_http_response_free(alloc, &resp);
    if (parse_err != HU_OK || !parsed) {
        *results = NULL;
        *result_count = 0;
        return HU_ERR_MEMORY_BACKEND;
    }

    hu_json_value_t *result_arr = hu_json_object_get(parsed, "result");
    if (!result_arr || result_arr->type != HU_JSON_ARRAY || result_arr->data.array.len == 0) {
        hu_json_free(alloc, parsed);
        *results = NULL;
        *result_count = 0;
        return HU_OK;
    }

    size_t n = result_arr->data.array.len;
    hu_vector_search_result_t *arr = (hu_vector_search_result_t *)alloc->alloc(
        alloc->ctx, n * sizeof(hu_vector_search_result_t));
    if (!arr) {
        hu_json_free(alloc, parsed);
        *results = NULL;
        *result_count = 0;
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(arr, 0, n * sizeof(hu_vector_search_result_t));

    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        hu_json_value_t *item = result_arr->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT)
            continue;
        float score = (float)hu_json_get_number(item, "score", 0.0);
        hu_json_value_t *payload = hu_json_object_get(item, "payload");
        const char *key = NULL;
        if (payload && payload->type == HU_JSON_OBJECT)
            key = hu_json_get_string(payload, "key");
        if (!key)
            key = hu_json_get_string(item, "id");
        if (!key)
            key = "";
        arr[out].id = hu_strdup(alloc, key);
        arr[out].score = score;
        out++;
    }
    hu_json_free(alloc, parsed);
    if (out == 0) {
        alloc->free(alloc->ctx, arr, n * sizeof(hu_vector_search_result_t));
        *results = NULL;
        *result_count = 0;
        return HU_OK;
    }
    if (out < n) {
        hu_vector_search_result_t *shrunk = (hu_vector_search_result_t *)alloc->realloc(
            alloc->ctx, arr, n * sizeof(hu_vector_search_result_t),
            out * sizeof(hu_vector_search_result_t));
        if (shrunk)
            arr = shrunk;
    }
    *results = arr;
    *result_count = out;
    return HU_OK;
}

static hu_error_t qdrant_delete(void *ctx, hu_allocator_t *alloc, const char *id, size_t id_len) {
    qdrant_ctx_t *q = (qdrant_ctx_t *)ctx;
    if (!q || !alloc || !id)
        return HU_ERR_INVALID_ARGUMENT;

    char url[512];
    snprintf(url, sizeof(url), "%s/collections/%s/points/delete?wait=true", q->url,
             q->collection_name);

    /* Body: {"filter":{"must":[{"key":"key","match":{"value":"<id>"}}]}} */
    char body[2048];
    char id_esc[512];
    size_t ie = 0;
    for (size_t i = 0; i < id_len && ie + 2 < sizeof(id_esc); i++) {
        char c = id[i];
        if (c == '"' || c == '\\') {
            id_esc[ie++] = '\\';
            id_esc[ie++] = c;
        } else if ((unsigned char)c >= 32)
            id_esc[ie++] = c;
    }
    id_esc[ie] = '\0';
    int bn = snprintf(body, sizeof(body),
                      "{\"filter\":{\"must\":[{\"key\":\"key\",\"match\":{\"value\":\"%s\"}}]}}",
                      id_esc);
    if (bn <= 0 || (size_t)bn >= sizeof(body))
        return HU_ERR_INVALID_ARGUMENT;

    hu_http_response_t resp = {0};
    const char *auth = q->api_key && q->api_key[0] ? q->api_key : NULL;
    hu_error_t err = hu_http_post_json(alloc, url, auth, body, (size_t)bn, &resp);
    if (err != HU_OK)
        return err;
    long status = resp.status_code;
    hu_http_response_free(alloc, &resp);
    return status == 200 ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

static size_t qdrant_count(void *ctx) {
    qdrant_ctx_t *q = (qdrant_ctx_t *)ctx;
    if (!q)
        return 0;

    char url[512];
    snprintf(url, sizeof(url), "%s/collections/%s/points/count", q->url, q->collection_name);

    const char *body = "{\"exact\":true}";
    hu_http_response_t resp = {0};
    const char *auth = q->api_key && q->api_key[0] ? q->api_key : NULL;
    hu_error_t err = hu_http_post_json(q->alloc, url, auth, body, 15, &resp);
    if (err != HU_OK || resp.status_code != 200 || !resp.body) {
        hu_http_response_free(q->alloc, &resp);
        return 0;
    }

    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(q->alloc, resp.body, resp.body_len, &parsed);
    hu_http_response_free(q->alloc, &resp);
    if (err != HU_OK || !parsed)
        return 0;

    hu_json_value_t *result_obj = hu_json_object_get(parsed, "result");
    size_t n = 0;
    if (result_obj && result_obj->type == HU_JSON_OBJECT)
        n = (size_t)hu_json_get_number(result_obj, "count", 0);
    hu_json_free(q->alloc, parsed);
    return n;
}
#endif

static void qdrant_deinit(void *ctx, hu_allocator_t *alloc) {
    qdrant_ctx_t *q = (qdrant_ctx_t *)ctx;
    if (!q || !alloc)
        return;
    if (q->url)
        alloc->free(alloc->ctx, q->url, strlen(q->url) + 1);
    if (q->api_key)
        alloc->free(alloc->ctx, q->api_key, strlen(q->api_key) + 1);
    if (q->collection_name)
        alloc->free(alloc->ctx, q->collection_name, strlen(q->collection_name) + 1);
    alloc->free(alloc->ctx, q, sizeof(qdrant_ctx_t));
}

static const hu_vector_store_vtable_t qdrant_vtable = {
    .upsert = qdrant_upsert,
    .search = qdrant_search,
    .delete = qdrant_delete,
    .count = qdrant_count,
    .deinit = qdrant_deinit,
};

hu_vector_store_t hu_vector_store_qdrant_create(hu_allocator_t *alloc,
                                                const hu_qdrant_config_t *config) {
    hu_vector_store_t s = {.ctx = NULL, .vtable = &qdrant_vtable};
    if (!alloc || !config)
        return s;

    qdrant_ctx_t *q = (qdrant_ctx_t *)alloc->alloc(alloc->ctx, sizeof(qdrant_ctx_t));
    if (!q)
        return s;
    memset(q, 0, sizeof(*q));
    q->alloc = alloc;
    q->url = config->url ? hu_strdup(alloc, config->url) : NULL;
    q->api_key = config->api_key ? hu_strdup(alloc, config->api_key) : NULL;
    q->collection_name = config->collection_name ? hu_strdup(alloc, config->collection_name) : NULL;
    q->dimensions = config->dimensions;

    if (!q->url || !q->collection_name) {
        qdrant_deinit(q, alloc);
        return s;
    }
    s.ctx = q;
    return s;
}
