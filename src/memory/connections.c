#include "human/memory/connections.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HU_CONN_CONTENT_TRUNC 200

/* Strip markdown code fences (```json ... ```) from LLM responses */
static void strip_markdown_json(const char *in, size_t in_len, const char **out, size_t *out_len) {
    *out = in;
    *out_len = in_len;
    if (!in || in_len < 7)
        return;
    const char *p = in;
    const char *end = in + in_len;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    if (end - p < 3 || p[0] != '`' || p[1] != '`' || p[2] != '`')
        return;
    p += 3;
    while (p < end && *p != '\n' && *p != '\r')
        p++;
    while (p < end && (*p == '\n' || *p == '\r'))
        p++;
    const char *close = end;
    for (const char *s = end - 1; s >= p; s--) {
        if (*s == '`' && s >= p + 2 && s[-1] == '`' && s[-2] == '`') {
            close = s - 2;
            break;
        }
    }
    while (close > p &&
           (close[-1] == ' ' || close[-1] == '\t' || close[-1] == '\n' || close[-1] == '\r'))
        close--;
    *out = p;
    *out_len = (size_t)(close - p);
}

hu_error_t hu_connections_build_prompt(hu_allocator_t *alloc, const hu_memory_entry_t *entries,
                                       size_t entry_count, char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (!entries || entry_count == 0)
        return HU_OK;

    char *buf = (char *)alloc->alloc(alloc->ctx, HU_CONN_PROMPT_CAP);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    int w = snprintf(buf, HU_CONN_PROMPT_CAP,
                     "Analyze these memories and find connections between them.\n"
                     "Return JSON: {\"connections\":[{\"a\":0,\"b\":1,\"relationship\":\"...\","
                     "\"strength\":0.8}],\"insights\":[{\"text\":\"...\",\"related\":[0,1]}]}\n\n");
    if (w > 0)
        pos = (size_t)w;

    for (size_t i = 0; i < entry_count && pos + 64 < HU_CONN_PROMPT_CAP; i++) {
        size_t show = entries[i].content_len;
        if (show > HU_CONN_CONTENT_TRUNC)
            show = HU_CONN_CONTENT_TRUNC;
        w = snprintf(buf + pos, HU_CONN_PROMPT_CAP - pos, "Memory %zu: [%.*s] %.*s", i,
                     (int)(entries[i].key_len > 0 ? entries[i].key_len : 0),
                     entries[i].key ? entries[i].key : "", (int)show,
                     entries[i].content ? entries[i].content : "");
        if (w > 0 && pos + (size_t)w < HU_CONN_PROMPT_CAP)
            pos += (size_t)w;
        if (entries[i].timestamp && entries[i].timestamp_len > 0) {
            w = snprintf(buf + pos, HU_CONN_PROMPT_CAP - pos, " (stored: %.*s)",
                         (int)entries[i].timestamp_len, entries[i].timestamp);
            if (w > 0 && pos + (size_t)w < HU_CONN_PROMPT_CAP)
                pos += (size_t)w;
        }
        if (pos + 2 < HU_CONN_PROMPT_CAP) {
            buf[pos++] = '\n';
        }
    }

    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
    return HU_OK;
}

hu_error_t hu_connections_parse(hu_allocator_t *alloc, const char *response, size_t response_len,
                                size_t entry_count, hu_connection_result_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (!response || response_len == 0)
        return HU_OK;

    const char *json_str = NULL;
    size_t json_len = 0;
    strip_markdown_json(response, response_len, &json_str, &json_len);

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, json_str, json_len, &root);
    if (err != HU_OK || !root || root->type != HU_JSON_OBJECT) {
        if (root)
            hu_json_free(alloc, root);
        return err != HU_OK ? err : HU_OK;
    }

    hu_json_value_t *conns = hu_json_object_get(root, "connections");
    if (conns && conns->type == HU_JSON_ARRAY) {
        for (size_t i = 0;
             i < conns->data.array.len && out->connection_count < HU_CONN_MAX_CONNECTIONS; i++) {
            hu_json_value_t *item = conns->data.array.items[i];
            if (!item || item->type != HU_JSON_OBJECT)
                continue;

            hu_json_value_t *va = hu_json_object_get(item, "a");
            hu_json_value_t *vb = hu_json_object_get(item, "b");
            hu_json_value_t *vr = hu_json_object_get(item, "relationship");
            hu_json_value_t *vs = hu_json_object_get(item, "strength");

            if (!va || va->type != HU_JSON_NUMBER || !vb || vb->type != HU_JSON_NUMBER)
                continue;

            size_t a = (size_t)va->data.number;
            size_t b = (size_t)vb->data.number;
            if (a >= entry_count || b >= entry_count)
                continue;

            hu_memory_connection_t *c = &out->connections[out->connection_count];
            c->memory_a_idx = a;
            c->memory_b_idx = b;
            c->strength = (vs && vs->type == HU_JSON_NUMBER) ? vs->data.number : 0.5;
            if (vr && vr->type == HU_JSON_STRING && vr->data.string.len > 0) {
                c->relationship = hu_strndup(alloc, vr->data.string.ptr, vr->data.string.len);
                c->relationship_len = vr->data.string.len;
            } else {
                c->relationship = hu_strndup(alloc, "related", 7);
                c->relationship_len = 7;
            }
            if (c->relationship)
                out->connection_count++;
        }
    }

    hu_json_value_t *insights = hu_json_object_get(root, "insights");
    if (insights && insights->type == HU_JSON_ARRAY) {
        for (size_t i = 0;
             i < insights->data.array.len && out->insight_count < HU_CONN_MAX_INSIGHTS; i++) {
            hu_json_value_t *item = insights->data.array.items[i];
            if (!item || item->type != HU_JSON_OBJECT)
                continue;

            hu_json_value_t *vt = hu_json_object_get(item, "text");
            if (!vt || vt->type != HU_JSON_STRING || vt->data.string.len == 0)
                continue;

            hu_memory_insight_t *ins = &out->insights[out->insight_count];
            ins->text = hu_strndup(alloc, vt->data.string.ptr, vt->data.string.len);
            ins->text_len = vt->data.string.len;
            if (!ins->text)
                continue;

            hu_json_value_t *vrel = hu_json_object_get(item, "related");
            if (vrel && vrel->type == HU_JSON_ARRAY && vrel->data.array.len > 0) {
                size_t alloc_bytes = vrel->data.array.len * sizeof(size_t);
                ins->related_indices = (size_t *)alloc->alloc(alloc->ctx, alloc_bytes);
                if (ins->related_indices) {
                    ins->related_alloc_bytes = alloc_bytes;
                    for (size_t j = 0; j < vrel->data.array.len; j++) {
                        hu_json_value_t *idx = vrel->data.array.items[j];
                        if (idx && idx->type == HU_JSON_NUMBER) {
                            size_t val = (size_t)idx->data.number;
                            if (val < entry_count) {
                                ins->related_indices[ins->related_count++] = val;
                            }
                        }
                    }
                }
            }
            out->insight_count++;
        }
    }

    hu_json_free(alloc, root);
    return HU_OK;
}

hu_error_t hu_connections_store_insights(hu_allocator_t *alloc, hu_memory_t *memory,
                                         const hu_connection_result_t *result,
                                         const hu_memory_entry_t *entries, size_t entry_count) {
    (void)entries;
    (void)entry_count;
    if (!alloc || !memory || !memory->vtable || !result)
        return HU_ERR_INVALID_ARGUMENT;

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_INSIGHT};
    time_t now = time(NULL);

    for (size_t i = 0; i < result->insight_count; i++) {
        if (!result->insights[i].text || result->insights[i].text_len == 0)
            continue;

        char *key = hu_sprintf(alloc, "insight:%ld:%zu", (long)now, i);
        if (!key)
            continue;

        size_t key_len = strlen(key);
        hu_memory_store_with_source(memory, key, key_len, result->insights[i].text,
                                    result->insights[i].text_len, &cat, NULL, 0,
                                    "connection_discovery", 20);
        hu_str_free(alloc, key);
    }
    return HU_OK;
}

void hu_connection_result_deinit(hu_connection_result_t *result, hu_allocator_t *alloc) {
    if (!result || !alloc)
        return;
    for (size_t i = 0; i < result->connection_count; i++) {
        if (result->connections[i].relationship)
            alloc->free(alloc->ctx, result->connections[i].relationship,
                        result->connections[i].relationship_len + 1);
    }
    result->connection_count = 0;
    for (size_t i = 0; i < result->insight_count; i++) {
        if (result->insights[i].text)
            alloc->free(alloc->ctx, result->insights[i].text, result->insights[i].text_len + 1);
        if (result->insights[i].related_indices)
            alloc->free(alloc->ctx, result->insights[i].related_indices,
                        result->insights[i].related_alloc_bytes);
    }
    result->insight_count = 0;
}
