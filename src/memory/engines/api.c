/* HTTP API memory backend — delegates to external REST service.
 * In HU_IS_TEST: in-memory mock. Otherwise uses hu_http_* calls. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/engines.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOCK_MAX_ENTRIES 32

typedef struct mock_entry {
    char *key;
    char *content;
    char *category;
    char *session_id;
} mock_entry_t;

typedef struct hu_api_memory {
    hu_allocator_t *alloc;
    char *base_url;
    char *api_key;
    uint32_t timeout_ms;
#if defined(HU_IS_TEST) && HU_IS_TEST
    mock_entry_t entries[MOCK_MAX_ENTRIES];
    size_t entry_count;
#endif
} hu_api_memory_t;

static const char *category_to_string(const hu_memory_category_t *cat) {
    if (!cat)
        return "core";
    switch (cat->tag) {
    case HU_MEMORY_CATEGORY_CORE:
        return "core";
    case HU_MEMORY_CATEGORY_DAILY:
        return "daily";
    case HU_MEMORY_CATEGORY_CONVERSATION:
        return "conversation";
    case HU_MEMORY_CATEGORY_INSIGHT:
        return "insight";
    case HU_MEMORY_CATEGORY_CUSTOM:
        if (cat->data.custom.name && cat->data.custom.name_len > 0)
            return cat->data.custom.name;
        return "custom";
    default:
        return "core";
    }
}

#if defined(HU_IS_TEST) && HU_IS_TEST
static void mock_free_entry(hu_allocator_t *alloc, mock_entry_t *e) {
    if (!alloc || !e)
        return;
    if (e->key)
        alloc->free(alloc->ctx, e->key, strlen(e->key) + 1);
    if (e->content)
        alloc->free(alloc->ctx, e->content, strlen(e->content) + 1);
    if (e->category)
        alloc->free(alloc->ctx, e->category, strlen(e->category) + 1);
    if (e->session_id)
        alloc->free(alloc->ctx, e->session_id, strlen(e->session_id) + 1);
    e->key = e->content = e->category = e->session_id = NULL;
}

static mock_entry_t *mock_find_by_key(hu_api_memory_t *self, const char *key, size_t key_len) {
    for (size_t i = 0; i < self->entry_count; i++) {
        mock_entry_t *e = &self->entries[i];
        if (e->key && strlen(e->key) == key_len && memcmp(e->key, key, key_len) == 0)
            return e;
    }
    return NULL;
}

static int mock_contains_substring(const char *haystack, size_t hlen, const char *needle,
                                   size_t nlen) {
    if (nlen == 0)
        return 1;
    if (hlen < nlen)
        return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (memcmp(haystack + i, needle, nlen) == 0)
            return 1;
    }
    return 0;
}
#endif /* HU_IS_TEST */

static const char *impl_name(void *ctx) {
    (void)ctx;
    return "api";
}

static hu_error_t impl_store(void *ctx, const char *key, size_t key_len, const char *content,
                             size_t content_len, const hu_memory_category_t *category,
                             const char *session_id, size_t session_id_len) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    hu_api_memory_t *self = (hu_api_memory_t *)ctx;
    hu_allocator_t *alloc = self->alloc;
    mock_entry_t *existing = mock_find_by_key(self, key, key_len);
    const char *cat_str = category_to_string(category);

    if (existing) {
        if (existing->content)
            alloc->free(alloc->ctx, existing->content, strlen(existing->content) + 1);
        if (existing->category)
            alloc->free(alloc->ctx, existing->category, strlen(existing->category) + 1);
        if (existing->session_id)
            alloc->free(alloc->ctx, existing->session_id, strlen(existing->session_id) + 1);
        existing->content = hu_strndup(alloc, content, content_len);
        existing->category = hu_strndup(alloc, cat_str, strlen(cat_str));
        existing->session_id = (session_id && session_id_len > 0)
                                   ? hu_strndup(alloc, session_id, session_id_len)
                                   : NULL;
        return existing->content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
    }

    if (self->entry_count >= MOCK_MAX_ENTRIES)
        return HU_ERR_OUT_OF_MEMORY;
    mock_entry_t *e = &self->entries[self->entry_count];
    e->key = hu_strndup(alloc, key, key_len);
    e->content = hu_strndup(alloc, content, content_len);
    e->category = hu_strndup(alloc, cat_str, strlen(cat_str));
    e->session_id =
        (session_id && session_id_len > 0) ? hu_strndup(alloc, session_id, session_id_len) : NULL;
    if (!e->key || !e->content || !e->category) {
        mock_free_entry(alloc, e);
        return HU_ERR_OUT_OF_MEMORY;
    }
    self->entry_count++;
    return HU_OK;

#else
    hu_api_memory_t *self = (hu_api_memory_t *)ctx;
    const char *cat_str = category_to_string(category);
    const char *sid = (session_id && session_id_len > 0) ? session_id : "";
    size_t sid_len = (session_id && session_id_len > 0) ? session_id_len : 0;

    hu_json_buf_t jbuf;
    if (hu_json_buf_init(&jbuf, self->alloc) != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;
    hu_error_t err = hu_json_append_key_value(&jbuf, "content", 7, content, content_len);
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&jbuf, ",", 1);
    if (err == HU_OK)
        err = hu_json_append_key_value(&jbuf, "category", 8, cat_str, strlen(cat_str));
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&jbuf, ",", 1);
    if (err == HU_OK)
        err = hu_json_append_key_value(&jbuf, "session_id", 10, sid, sid_len);
    if (err != HU_OK) {
        hu_json_buf_free(&jbuf);
        return err;
    }

    char url[1024];
    size_t blen = strlen(self->base_url);
    while (blen > 0 && self->base_url[blen - 1] == '/')
        blen--;
    int ul = snprintf(url, sizeof(url), "%.*s/memories/%.*s", (int)blen, self->base_url,
                      (int)key_len, key);
    if (ul <= 0 || (size_t)ul >= sizeof(url)) {
        hu_json_buf_free(&jbuf);
        return HU_ERR_INVALID_ARGUMENT;
    }
    char headers[320];
    int hl = 0;
    if (self->api_key && self->api_key[0])
        hl = snprintf(headers, sizeof(headers),
                      "Authorization: Bearer %s\nContent-Type: application/json", self->api_key);
    const char *extra = (hl > 0) ? headers : "Content-Type: application/json";

    hu_http_response_t resp = {0};
    err = hu_http_request(self->alloc, url, "PUT", extra, jbuf.ptr, jbuf.len, &resp);
    hu_json_buf_free(&jbuf);
    if (err != HU_OK)
        return err;
    long status = resp.status_code;
    if (resp.owned && resp.body)
        hu_http_response_free(self->alloc, &resp);
    return (status >= 200 && status < 300) ? HU_OK : HU_ERR_MEMORY_STORE;
#endif
}

static hu_error_t impl_recall(void *ctx, hu_allocator_t *alloc, const char *query, size_t query_len,
                              size_t limit, const char *session_id, size_t session_id_len,
                              hu_memory_entry_t **out, size_t *out_count) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    hu_api_memory_t *self = (hu_api_memory_t *)ctx;
    *out = NULL;
    *out_count = 0;
    size_t cap = 0, n = 0;
    hu_memory_entry_t *results = NULL;

    for (size_t i = 0; i < self->entry_count && n < limit; i++) {
        mock_entry_t *e = &self->entries[i];
        if (!e->key)
            continue;
        bool match = mock_contains_substring(e->key, strlen(e->key), query, query_len) ||
                     mock_contains_substring(e->content, strlen(e->content), query, query_len);
        if (session_id && session_id_len > 0 && e->session_id) {
            if (strlen(e->session_id) != session_id_len ||
                memcmp(e->session_id, session_id, session_id_len) != 0)
                match = false;
        } else if (session_id && session_id_len > 0 && !e->session_id)
            match = false;
        if (!match)
            continue;

        if (n >= cap) {
            size_t new_cap = cap ? cap * 2 : 4;
            hu_memory_entry_t *tmp = (hu_memory_entry_t *)alloc->realloc(
                alloc->ctx, results, cap * sizeof(hu_memory_entry_t),
                new_cap * sizeof(hu_memory_entry_t));
            if (!tmp) {
                for (size_t j = 0; j < n; j++)
                    hu_memory_entry_free_fields(alloc, &results[j]);
                if (results)
                    alloc->free(alloc->ctx, results, cap * sizeof(hu_memory_entry_t));
                return HU_ERR_OUT_OF_MEMORY;
            }
            results = tmp;
            cap = new_cap;
        }

        hu_memory_entry_t *r = &results[n];
        memset(r, 0, sizeof(*r));
        r->id = r->key = hu_strndup(alloc, e->key, strlen(e->key));
        r->key_len = strlen(e->key);
        r->id_len = r->key_len;
        r->content = hu_strndup(alloc, e->content, strlen(e->content));
        r->content_len = strlen(e->content);
        r->timestamp = hu_sprintf(alloc, "0");
        r->timestamp_len = r->timestamp ? strlen(r->timestamp) : 0;
        if (e->session_id) {
            r->session_id = hu_strndup(alloc, e->session_id, strlen(e->session_id));
            r->session_id_len = strlen(e->session_id);
        }
        r->category.tag = HU_MEMORY_CATEGORY_CUSTOM;
        r->category.data.custom.name = hu_strndup(alloc, e->category, strlen(e->category));
        r->category.data.custom.name_len = strlen(e->category);
        if (!r->key || !r->content) {
            for (size_t j = 0; j <= n; j++)
                hu_memory_entry_free_fields(alloc, &results[j]);
            alloc->free(alloc->ctx, results, cap * sizeof(hu_memory_entry_t));
            return HU_ERR_OUT_OF_MEMORY;
        }
        n++;
    }
    *out = results;
    *out_count = n;
    return HU_OK;
#else
    hu_api_memory_t *self = (hu_api_memory_t *)ctx;
    *out = NULL;
    *out_count = 0;
    char url[1024];
    size_t blen = strlen(self->base_url);
    while (blen > 0 && self->base_url[blen - 1] == '/')
        blen--;
    (void)snprintf(url, sizeof(url), "%.*s/memories/search", (int)blen, self->base_url);
    hu_json_buf_t jbuf;
    if (hu_json_buf_init(&jbuf, self->alloc) != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;
    hu_error_t err = hu_json_append_key_value(&jbuf, "query", 5, query, query_len);
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&jbuf, ",", 1);
    if (err == HU_OK)
        err = hu_json_append_key_int(&jbuf, "limit", 5, (long long)limit);
    if (err == HU_OK && session_id && session_id_len > 0) {
        err = hu_json_buf_append_raw(&jbuf, ",", 1);
        if (err == HU_OK)
            err = hu_json_append_key_value(&jbuf, "session_id", 10, session_id, session_id_len);
    }
    if (err != HU_OK) {
        hu_json_buf_free(&jbuf);
        return err;
    }
    char auth[256];
    int al = self->api_key && self->api_key[0]
                 ? snprintf(auth, sizeof(auth), "Bearer %s", self->api_key)
                 : 0;
    const char *auth_header = (al > 0) ? auth : NULL;
    hu_http_response_t resp = {0};
    err = hu_http_post_json(self->alloc, url, auth_header, jbuf.ptr, jbuf.len, &resp);
    hu_json_buf_free(&jbuf);
    if (err != HU_OK)
        return err;
    if (resp.status_code < 200 || resp.status_code >= 300 || !resp.body) {
        if (resp.owned && resp.body)
            hu_http_response_free(self->alloc, &resp);
        return HU_ERR_MEMORY_RECALL;
    }
    hu_json_value_t *root = NULL;
    err = hu_json_parse(self->alloc, resp.body, resp.body_len, &root);
    if (resp.owned && resp.body)
        hu_http_response_free(self->alloc, &resp);
    if (err != HU_OK || !root || root->type != HU_JSON_OBJECT) {
        if (root)
            hu_json_free(self->alloc, root);
        return HU_ERR_MEMORY_RECALL;
    }
    hu_json_value_t *arr = hu_json_object_get(root, "memories");
    if (!arr || arr->type != HU_JSON_ARRAY) {
        hu_json_free(self->alloc, root);
        *out = NULL;
        *out_count = 0;
        return HU_OK;
    }
    size_t n = arr->data.array.len < limit ? arr->data.array.len : limit;
    if (n == 0) {
        hu_json_free(self->alloc, root);
        return HU_OK;
    }
    hu_memory_entry_t *entries =
        (hu_memory_entry_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_memory_entry_t));
    if (!entries) {
        hu_json_free(self->alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(entries, 0, n * sizeof(hu_memory_entry_t));
    for (size_t i = 0; i < n; i++) {
        hu_json_value_t *item = arr->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT)
            continue;
        const char *k = hu_json_get_string(item, "key");
        const char *c = hu_json_get_string(item, "content");
        const char *cat = hu_json_get_string(item, "category");
        const char *sid = hu_json_get_string(item, "session_id");
        if (k) {
            entries[i].key = hu_strndup(alloc, k, strlen(k));
            entries[i].key_len = strlen(k);
            entries[i].id = entries[i].key;
            entries[i].id_len = entries[i].key_len;
        }
        if (c) {
            entries[i].content = hu_strndup(alloc, c, strlen(c));
            entries[i].content_len = strlen(c);
        }
        if (cat) {
            entries[i].category.tag = HU_MEMORY_CATEGORY_CUSTOM;
            entries[i].category.data.custom.name = hu_strndup(alloc, cat, strlen(cat));
            entries[i].category.data.custom.name_len = strlen(cat);
        }
        if (sid) {
            entries[i].session_id = hu_strndup(alloc, sid, strlen(sid));
            entries[i].session_id_len = strlen(sid);
        }
        entries[i].timestamp = hu_sprintf(alloc, "0");
        entries[i].timestamp_len = entries[i].timestamp ? strlen(entries[i].timestamp) : 0;
    }
    hu_json_free(self->alloc, root);
    *out = entries;
    *out_count = n;
    return HU_OK;
#endif
}

static hu_error_t impl_get(void *ctx, hu_allocator_t *alloc, const char *key, size_t key_len,
                           hu_memory_entry_t *out, bool *found) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    hu_api_memory_t *self = (hu_api_memory_t *)ctx;
    mock_entry_t *e = mock_find_by_key(self, key, key_len);
    *found = false;
    memset(out, 0, sizeof(*out));
    if (!e)
        return HU_OK;
    *found = true;
    out->id = out->key = hu_strndup(alloc, e->key, strlen(e->key));
    out->key_len = strlen(e->key);
    out->id_len = out->key_len;
    out->content = hu_strndup(alloc, e->content, strlen(e->content));
    out->content_len = strlen(e->content);
    out->timestamp = hu_sprintf(alloc, "0");
    out->timestamp_len = out->timestamp ? strlen(out->timestamp) : 0;
    if (e->session_id) {
        out->session_id = hu_strndup(alloc, e->session_id, strlen(e->session_id));
        out->session_id_len = strlen(e->session_id);
    }
    out->category.tag = HU_MEMORY_CATEGORY_CUSTOM;
    out->category.data.custom.name = hu_strndup(alloc, e->category, strlen(e->category));
    out->category.data.custom.name_len = strlen(e->category);
    return HU_OK;
#else
    hu_api_memory_t *self = (hu_api_memory_t *)ctx;
    *found = false;
    memset(out, 0, sizeof(*out));
    char url[1024];
    size_t blen = strlen(self->base_url);
    while (blen > 0 && self->base_url[blen - 1] == '/')
        blen--;
    (void)snprintf(url, sizeof(url), "%.*s/memories/%.*s", (int)blen, self->base_url, (int)key_len,
                   key);
    char headers[320];
    int hl =
        self->api_key && self->api_key[0]
            ? snprintf(headers, sizeof(headers),
                       "Authorization: Bearer %s\nContent-Type: application/json", self->api_key)
            : 0;
    hu_http_response_t resp = {0};
    hu_error_t err =
        hu_http_request(self->alloc, url, "GET",
                        (hl > 0) ? headers : "Content-Type: application/json", NULL, 0, &resp);
    if (err != HU_OK)
        return err;
    if (resp.status_code == 404 || !resp.body) {
        if (resp.owned && resp.body)
            hu_http_response_free(self->alloc, &resp);
        return HU_OK;
    }
    hu_json_value_t *root = NULL;
    err = hu_json_parse(self->alloc, resp.body, resp.body_len, &root);
    if (resp.owned && resp.body)
        hu_http_response_free(self->alloc, &resp);
    if (err != HU_OK || !root || root->type != HU_JSON_OBJECT) {
        if (root)
            hu_json_free(self->alloc, root);
        return HU_OK;
    }
    const char *k = hu_json_get_string(root, "key");
    const char *c = hu_json_get_string(root, "content");
    const char *cat = hu_json_get_string(root, "category");
    const char *sid = hu_json_get_string(root, "session_id");
    if (!k && !c) {
        hu_json_free(self->alloc, root);
        return HU_OK;
    }
    *found = true;
    out->id = out->key = k ? hu_strndup(alloc, k, strlen(k)) : hu_strndup(alloc, key, key_len);
    out->key_len = out->id_len = k ? strlen(k) : key_len;
    out->content = c ? hu_strndup(alloc, c, strlen(c)) : hu_strndup(alloc, "", 0);
    out->content_len = c ? strlen(c) : 0;
    out->timestamp = hu_sprintf(alloc, "0");
    out->timestamp_len = out->timestamp ? strlen(out->timestamp) : 0;
    if (sid) {
        out->session_id = hu_strndup(alloc, sid, strlen(sid));
        out->session_id_len = strlen(sid);
    }
    out->category.tag = HU_MEMORY_CATEGORY_CUSTOM;
    if (cat) {
        out->category.data.custom.name = hu_strndup(alloc, cat, strlen(cat));
        out->category.data.custom.name_len = strlen(cat);
    }
    hu_json_free(self->alloc, root);
    return HU_OK;
#endif
}

static hu_error_t impl_list(void *ctx, hu_allocator_t *alloc, const hu_memory_category_t *category,
                            const char *session_id, size_t session_id_len, hu_memory_entry_t **out,
                            size_t *out_count) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    hu_api_memory_t *self = (hu_api_memory_t *)ctx;
    const char *cat_filter = category ? category_to_string(category) : NULL;
    *out = NULL;
    *out_count = 0;
    size_t cap = 0, n = 0;
    hu_memory_entry_t *results = NULL;

    for (size_t i = 0; i < self->entry_count; i++) {
        mock_entry_t *e = &self->entries[i];
        if (!e->key)
            continue;
        if (cat_filter && (!e->category || strcmp(e->category, cat_filter) != 0))
            continue;
        if (session_id && session_id_len > 0) {
            if (!e->session_id)
                continue;
            if (strlen(e->session_id) != session_id_len ||
                memcmp(e->session_id, session_id, session_id_len) != 0)
                continue;
        }

        if (n >= cap) {
            size_t new_cap = cap ? cap * 2 : 4;
            hu_memory_entry_t *tmp = (hu_memory_entry_t *)alloc->realloc(
                alloc->ctx, results, cap * sizeof(hu_memory_entry_t),
                new_cap * sizeof(hu_memory_entry_t));
            if (!tmp) {
                for (size_t j = 0; j < n; j++)
                    hu_memory_entry_free_fields(alloc, &results[j]);
                if (results)
                    alloc->free(alloc->ctx, results, cap * sizeof(hu_memory_entry_t));
                return HU_ERR_OUT_OF_MEMORY;
            }
            results = tmp;
            cap = new_cap;
        }

        hu_memory_entry_t *r = &results[n];
        memset(r, 0, sizeof(*r));
        r->id = r->key = hu_strndup(alloc, e->key, strlen(e->key));
        r->key_len = strlen(e->key);
        r->id_len = r->key_len;
        r->content = hu_strndup(alloc, e->content, strlen(e->content));
        r->content_len = strlen(e->content);
        r->timestamp = hu_sprintf(alloc, "0");
        r->timestamp_len = r->timestamp ? strlen(r->timestamp) : 0;
        if (e->session_id) {
            r->session_id = hu_strndup(alloc, e->session_id, strlen(e->session_id));
            r->session_id_len = strlen(e->session_id);
        }
        r->category.tag = HU_MEMORY_CATEGORY_CUSTOM;
        r->category.data.custom.name = hu_strndup(alloc, e->category, strlen(e->category));
        r->category.data.custom.name_len = strlen(e->category);
        n++;
    }
    *out = results;
    *out_count = n;
    return HU_OK;
#else
    hu_api_memory_t *self = (hu_api_memory_t *)ctx;
    const char *cat_filter = category ? category_to_string(category) : NULL;
    *out = NULL;
    *out_count = 0;
    char url[1024];
    size_t blen = strlen(self->base_url);
    while (blen > 0 && self->base_url[blen - 1] == '/')
        blen--;
    int ul = (int)snprintf(url, sizeof(url), "%.*s/memories", (int)blen, self->base_url);
    if (ul <= 0 || (size_t)ul >= sizeof(url))
        return HU_ERR_INVALID_ARGUMENT;
    if (cat_filter || (session_id && session_id_len > 0)) {
        size_t pos = strlen(url);
        if (cat_filter)
            pos += (size_t)snprintf(url + pos, sizeof(url) - pos, "?category=%s", cat_filter);
        if (session_id && session_id_len > 0) {
            char sid_buf[256];
            size_t sid_len =
                session_id_len < sizeof(sid_buf) - 1 ? session_id_len : sizeof(sid_buf) - 1;
            memcpy(sid_buf, session_id, sid_len);
            sid_buf[sid_len] = '\0';
            pos += (size_t)snprintf(url + pos, sizeof(url) - pos, "%csession_id=%s",
                                    (pos > (size_t)ul) ? '&' : '?', sid_buf);
        }
    }
    char headers[320];
    int hl =
        self->api_key && self->api_key[0]
            ? snprintf(headers, sizeof(headers),
                       "Authorization: Bearer %s\nContent-Type: application/json", self->api_key)
            : 0;
    hu_http_response_t resp = {0};
    hu_error_t err =
        hu_http_request(self->alloc, url, "GET",
                        (hl > 0) ? headers : "Content-Type: application/json", NULL, 0, &resp);
    if (err != HU_OK)
        return err;
    if (!resp.body || resp.body_len == 0) {
        if (resp.owned && resp.body)
            hu_http_response_free(self->alloc, &resp);
        return HU_OK;
    }
    hu_json_value_t *root = NULL;
    err = hu_json_parse(self->alloc, resp.body, resp.body_len, &root);
    if (resp.owned && resp.body)
        hu_http_response_free(self->alloc, &resp);
    if (err != HU_OK || !root || root->type != HU_JSON_OBJECT) {
        if (root)
            hu_json_free(self->alloc, root);
        return HU_OK;
    }
    hu_json_value_t *arr = hu_json_object_get(root, "memories");
    if (!arr || arr->type != HU_JSON_ARRAY) {
        hu_json_free(self->alloc, root);
        return HU_OK;
    }
    size_t n = arr->data.array.len;
    if (n == 0) {
        hu_json_free(self->alloc, root);
        return HU_OK;
    }
    hu_memory_entry_t *entries =
        (hu_memory_entry_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_memory_entry_t));
    if (!entries) {
        hu_json_free(self->alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(entries, 0, n * sizeof(hu_memory_entry_t));
    for (size_t i = 0; i < n; i++) {
        hu_json_value_t *item = arr->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT)
            continue;
        const char *k = hu_json_get_string(item, "key");
        const char *c = hu_json_get_string(item, "content");
        const char *cat = hu_json_get_string(item, "category");
        const char *sid = hu_json_get_string(item, "session_id");
        if (k) {
            entries[i].key = hu_strndup(alloc, k, strlen(k));
            entries[i].key_len = strlen(k);
            entries[i].id = entries[i].key;
            entries[i].id_len = entries[i].key_len;
        }
        if (c) {
            entries[i].content = hu_strndup(alloc, c, strlen(c));
            entries[i].content_len = strlen(c);
        }
        if (cat) {
            entries[i].category.tag = HU_MEMORY_CATEGORY_CUSTOM;
            entries[i].category.data.custom.name = hu_strndup(alloc, cat, strlen(cat));
            entries[i].category.data.custom.name_len = strlen(cat);
        }
        if (sid) {
            entries[i].session_id = hu_strndup(alloc, sid, strlen(sid));
            entries[i].session_id_len = strlen(sid);
        }
        entries[i].timestamp = hu_sprintf(alloc, "0");
        entries[i].timestamp_len = entries[i].timestamp ? strlen(entries[i].timestamp) : 0;
    }
    hu_json_free(self->alloc, root);
    *out = entries;
    *out_count = n;
    return HU_OK;
#endif
}

static hu_error_t impl_forget(void *ctx, const char *key, size_t key_len, bool *deleted) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    hu_api_memory_t *self = (hu_api_memory_t *)ctx;
    *deleted = false;
    for (size_t i = 0; i < self->entry_count; i++) {
        mock_entry_t *e = &self->entries[i];
        if (e->key && strlen(e->key) == key_len && memcmp(e->key, key, key_len) == 0) {
            mock_free_entry(self->alloc, e);
            memmove(&self->entries[i], &self->entries[i + 1],
                    (self->entry_count - i - 1) * sizeof(mock_entry_t));
            memset(&self->entries[self->entry_count - 1], 0, sizeof(mock_entry_t));
            self->entry_count--;
            *deleted = true;
            return HU_OK;
        }
    }
    return HU_OK;
#else
    hu_api_memory_t *self = (hu_api_memory_t *)ctx;
    *deleted = false;
    char url[1024];
    size_t blen = strlen(self->base_url);
    while (blen > 0 && self->base_url[blen - 1] == '/')
        blen--;
    (void)snprintf(url, sizeof(url), "%.*s/memories/%.*s", (int)blen, self->base_url, (int)key_len,
                   key);
    char headers[320];
    int hl =
        self->api_key && self->api_key[0]
            ? snprintf(headers, sizeof(headers),
                       "Authorization: Bearer %s\nContent-Type: application/json", self->api_key)
            : 0;
    hu_http_response_t resp = {0};
    hu_error_t err =
        hu_http_request(self->alloc, url, "DELETE",
                        (hl > 0) ? headers : "Content-Type: application/json", NULL, 0, &resp);
    if (err != HU_OK)
        return err;
    *deleted = (resp.status_code >= 200 && resp.status_code < 300);
    if (resp.owned && resp.body)
        hu_http_response_free(self->alloc, &resp);
    return HU_OK;
#endif
}

static hu_error_t impl_count(void *ctx, size_t *out) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    hu_api_memory_t *self = (hu_api_memory_t *)ctx;
    *out = self->entry_count;
    return HU_OK;
#else
    hu_api_memory_t *self = (hu_api_memory_t *)ctx;
    *out = 0;
    char url[1024];
    size_t blen = strlen(self->base_url);
    while (blen > 0 && self->base_url[blen - 1] == '/')
        blen--;
    (void)snprintf(url, sizeof(url), "%.*s/memories/count", (int)blen, self->base_url);
    char headers[320];
    int hl =
        self->api_key && self->api_key[0]
            ? snprintf(headers, sizeof(headers),
                       "Authorization: Bearer %s\nContent-Type: application/json", self->api_key)
            : 0;
    hu_http_response_t resp = {0};
    hu_error_t err =
        hu_http_request(self->alloc, url, "GET",
                        (hl > 0) ? headers : "Content-Type: application/json", NULL, 0, &resp);
    if (err != HU_OK)
        return err;
    if (resp.body && resp.body_len > 0) {
        hu_json_value_t *root = NULL;
        if (hu_json_parse(self->alloc, resp.body, resp.body_len, &root) == HU_OK && root &&
            root->type == HU_JSON_OBJECT) {
            double c = hu_json_get_number(root, "count", 0);
            *out = (size_t)c;
            hu_json_free(self->alloc, root);
        }
    }
    if (resp.owned && resp.body)
        hu_http_response_free(self->alloc, &resp);
    return HU_OK;
#endif
}

static bool impl_health_check(void *ctx) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)ctx;
    return true;
#else
    hu_api_memory_t *self = (hu_api_memory_t *)ctx;
    char url[1024];
    size_t blen = strlen(self->base_url);
    while (blen > 0 && self->base_url[blen - 1] == '/')
        blen--;
    (void)snprintf(url, sizeof(url), "%.*s/health", (int)blen, self->base_url);
    char headers[320];
    int hl =
        self->api_key && self->api_key[0]
            ? snprintf(headers, sizeof(headers),
                       "Authorization: Bearer %s\nContent-Type: application/json", self->api_key)
            : 0;
    hu_http_response_t resp = {0};
    hu_error_t err =
        hu_http_request(self->alloc, url, "GET",
                        (hl > 0) ? headers : "Content-Type: application/json", NULL, 0, &resp);
    if (err != HU_OK)
        return false;
    bool ok = (resp.status_code >= 200 && resp.status_code < 300);
    if (resp.owned && resp.body)
        hu_http_response_free(self->alloc, &resp);
    return ok;
#endif
}

static void impl_deinit(void *ctx) {
    hu_api_memory_t *self = (hu_api_memory_t *)ctx;
    if (!self)
        return;
#if defined(HU_IS_TEST) && HU_IS_TEST
    for (size_t i = 0; i < self->entry_count; i++)
        mock_free_entry(self->alloc, &self->entries[i]);
    self->entry_count = 0;
#endif
    if (self->base_url && self->alloc)
        self->alloc->free(self->alloc->ctx, self->base_url, strlen(self->base_url) + 1);
    if (self->api_key && self->alloc)
        self->alloc->free(self->alloc->ctx, self->api_key, strlen(self->api_key) + 1);
    if (self->alloc)
        self->alloc->free(self->alloc->ctx, self, sizeof(hu_api_memory_t));
}

static const hu_memory_vtable_t api_vtable = {
    .name = impl_name,
    .store = impl_store,
    .recall = impl_recall,
    .get = impl_get,
    .list = impl_list,
    .forget = impl_forget,
    .count = impl_count,
    .health_check = impl_health_check,
    .deinit = impl_deinit,
};

hu_memory_t hu_api_memory_create(hu_allocator_t *alloc, const char *base_url, const char *api_key,
                                 uint32_t timeout_ms) {
    if (!alloc || !base_url)
        return (hu_memory_t){.ctx = NULL, .vtable = NULL};
    hu_api_memory_t *self = (hu_api_memory_t *)alloc->alloc(alloc->ctx, sizeof(hu_api_memory_t));
    if (!self)
        return (hu_memory_t){.ctx = NULL, .vtable = NULL};
    memset(self, 0, sizeof(hu_api_memory_t));
    self->alloc = alloc;
    self->base_url = (char *)alloc->alloc(alloc->ctx, strlen(base_url) + 1);
    if (self->base_url)
        memcpy(self->base_url, base_url, strlen(base_url) + 1);
    self->api_key = api_key ? (char *)alloc->alloc(alloc->ctx, strlen(api_key) + 1) : NULL;
    if (api_key && self->api_key)
        memcpy(self->api_key, api_key, strlen(api_key) + 1);
    self->timeout_ms = timeout_ms;
    return (hu_memory_t){.ctx = self, .vtable = &api_vtable};
}
