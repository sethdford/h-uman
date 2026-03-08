#include "seaclaw/memory/connections.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SC_CONN_PROMPT_CAP    4096
#define SC_CONN_CONTENT_TRUNC 200

sc_error_t sc_connections_build_prompt(sc_allocator_t *alloc, const sc_memory_entry_t *entries,
                                       size_t entry_count, char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (!entries || entry_count == 0)
        return SC_OK;

    char *buf = (char *)alloc->alloc(alloc->ctx, SC_CONN_PROMPT_CAP);
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    int w = snprintf(buf, SC_CONN_PROMPT_CAP,
                     "Analyze these memories and find connections between them.\n"
                     "Return JSON: {\"connections\":[{\"a\":0,\"b\":1,\"relationship\":\"...\","
                     "\"strength\":0.8}],\"insights\":[{\"text\":\"...\",\"related\":[0,1]}]}\n\n");
    if (w > 0)
        pos = (size_t)w;

    for (size_t i = 0; i < entry_count && pos + 64 < SC_CONN_PROMPT_CAP; i++) {
        size_t show = entries[i].content_len;
        if (show > SC_CONN_CONTENT_TRUNC)
            show = SC_CONN_CONTENT_TRUNC;
        w = snprintf(buf + pos, SC_CONN_PROMPT_CAP - pos, "Memory %zu: [%.*s] %.*s", i,
                     (int)(entries[i].key_len > 0 ? entries[i].key_len : 0),
                     entries[i].key ? entries[i].key : "", (int)show,
                     entries[i].content ? entries[i].content : "");
        if (w > 0 && pos + (size_t)w < SC_CONN_PROMPT_CAP)
            pos += (size_t)w;
        if (entries[i].timestamp && entries[i].timestamp_len > 0) {
            w = snprintf(buf + pos, SC_CONN_PROMPT_CAP - pos, " (stored: %.*s)",
                         (int)entries[i].timestamp_len, entries[i].timestamp);
            if (w > 0 && pos + (size_t)w < SC_CONN_PROMPT_CAP)
                pos += (size_t)w;
        }
        if (pos + 2 < SC_CONN_PROMPT_CAP) {
            buf[pos++] = '\n';
        }
    }

    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
    return SC_OK;
}

sc_error_t sc_connections_parse(sc_allocator_t *alloc, const char *response, size_t response_len,
                                size_t entry_count, sc_connection_result_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (!response || response_len == 0)
        return SC_OK;

    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(alloc, response, response_len, &root);
    if (err != SC_OK || !root || root->type != SC_JSON_OBJECT) {
        if (root)
            sc_json_free(alloc, root);
        return err != SC_OK ? err : SC_OK;
    }

    sc_json_value_t *conns = sc_json_object_get(root, "connections");
    if (conns && conns->type == SC_JSON_ARRAY) {
        for (size_t i = 0;
             i < conns->data.array.len && out->connection_count < SC_CONN_MAX_CONNECTIONS; i++) {
            sc_json_value_t *item = conns->data.array.items[i];
            if (!item || item->type != SC_JSON_OBJECT)
                continue;

            sc_json_value_t *va = sc_json_object_get(item, "a");
            sc_json_value_t *vb = sc_json_object_get(item, "b");
            sc_json_value_t *vr = sc_json_object_get(item, "relationship");
            sc_json_value_t *vs = sc_json_object_get(item, "strength");

            if (!va || va->type != SC_JSON_NUMBER || !vb || vb->type != SC_JSON_NUMBER)
                continue;

            size_t a = (size_t)va->data.number;
            size_t b = (size_t)vb->data.number;
            if (a >= entry_count || b >= entry_count)
                continue;

            sc_memory_connection_t *c = &out->connections[out->connection_count];
            c->memory_a_idx = a;
            c->memory_b_idx = b;
            c->strength = (vs && vs->type == SC_JSON_NUMBER) ? vs->data.number : 0.5;
            if (vr && vr->type == SC_JSON_STRING && vr->data.string.len > 0) {
                c->relationship = sc_strndup(alloc, vr->data.string.ptr, vr->data.string.len);
                c->relationship_len = vr->data.string.len;
            } else {
                c->relationship = sc_strndup(alloc, "related", 7);
                c->relationship_len = 7;
            }
            if (c->relationship)
                out->connection_count++;
        }
    }

    sc_json_value_t *insights = sc_json_object_get(root, "insights");
    if (insights && insights->type == SC_JSON_ARRAY) {
        for (size_t i = 0;
             i < insights->data.array.len && out->insight_count < SC_CONN_MAX_INSIGHTS; i++) {
            sc_json_value_t *item = insights->data.array.items[i];
            if (!item || item->type != SC_JSON_OBJECT)
                continue;

            sc_json_value_t *vt = sc_json_object_get(item, "text");
            if (!vt || vt->type != SC_JSON_STRING || vt->data.string.len == 0)
                continue;

            sc_memory_insight_t *ins = &out->insights[out->insight_count];
            ins->text = sc_strndup(alloc, vt->data.string.ptr, vt->data.string.len);
            ins->text_len = vt->data.string.len;
            if (!ins->text)
                continue;

            sc_json_value_t *vrel = sc_json_object_get(item, "related");
            if (vrel && vrel->type == SC_JSON_ARRAY && vrel->data.array.len > 0) {
                ins->related_indices =
                    (size_t *)alloc->alloc(alloc->ctx, vrel->data.array.len * sizeof(size_t));
                if (ins->related_indices) {
                    for (size_t j = 0; j < vrel->data.array.len; j++) {
                        sc_json_value_t *idx = vrel->data.array.items[j];
                        if (idx && idx->type == SC_JSON_NUMBER) {
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

    sc_json_free(alloc, root);
    return SC_OK;
}

sc_error_t sc_connections_store_insights(sc_allocator_t *alloc, sc_memory_t *memory,
                                         const sc_connection_result_t *result,
                                         const sc_memory_entry_t *entries, size_t entry_count) {
    (void)entries;
    (void)entry_count;
    if (!alloc || !memory || !memory->vtable || !result)
        return SC_ERR_INVALID_ARGUMENT;

    sc_memory_category_t cat = {.tag = SC_MEMORY_CATEGORY_INSIGHT};
    time_t now = time(NULL);

    for (size_t i = 0; i < result->insight_count; i++) {
        if (!result->insights[i].text || result->insights[i].text_len == 0)
            continue;

        char *key = sc_sprintf(alloc, "insight:%ld:%zu", (long)now, i);
        if (!key)
            continue;

        size_t key_len = strlen(key);
        sc_memory_store_with_source(memory, key, key_len, result->insights[i].text,
                                    result->insights[i].text_len, &cat, NULL, 0,
                                    "connection_discovery", 20);
        sc_str_free(alloc, key);
    }
    return SC_OK;
}

void sc_connection_result_deinit(sc_connection_result_t *result, sc_allocator_t *alloc) {
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
                        result->insights[i].related_count * sizeof(size_t));
    }
    result->insight_count = 0;
}
