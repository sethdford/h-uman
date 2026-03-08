#ifdef SC_GATEWAY_POSIX

#include "cp_internal.h"
#include "seaclaw/agent.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory.h"
#include "seaclaw/memory/consolidation.h"
#include <stdio.h>
#include <string.h>

static const char *category_to_str(const sc_memory_category_t *cat) {
    if (!cat)
        return "core";
    switch (cat->tag) {
    case SC_MEMORY_CATEGORY_CORE:
        return "core";
    case SC_MEMORY_CATEGORY_DAILY:
        return "daily";
    case SC_MEMORY_CATEGORY_CONVERSATION:
        return "conversation";
    case SC_MEMORY_CATEGORY_INSIGHT:
        return "insight";
    case SC_MEMORY_CATEGORY_CUSTOM:
        if (cat->data.custom.name && cat->data.custom.name_len > 0)
            return cat->data.custom.name;
        return "custom";
    default:
        return "core";
    }
}

static sc_json_value_t *entry_to_json(sc_allocator_t *alloc, const sc_memory_entry_t *e) {
    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return NULL;
    cp_json_set_str(alloc, obj, "key", e->key ? e->key : "");
    cp_json_set_str(alloc, obj, "content", e->content ? e->content : "");
    cp_json_set_str(alloc, obj, "category", category_to_str(&e->category));
    cp_json_set_str(alloc, obj, "timestamp", e->timestamp ? e->timestamp : "");
    cp_json_set_str(alloc, obj, "source", e->source ? e->source : "");
    sc_json_object_set(alloc, obj, "score",
                       sc_json_number_new(alloc, (e->score == e->score) ? e->score : 0.0));
    return obj;
}

sc_error_t cp_memory_status(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                            const sc_control_protocol_t *proto, const sc_json_value_t *root,
                            char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    if (!app || !app->agent || !app->agent->memory)
        return SC_ERR_NOT_SUPPORTED;

    sc_memory_t *memory = app->agent->memory;
    size_t count = 0;
    sc_error_t err = memory->vtable->count(memory->ctx, &count);
    if (err != SC_OK)
        return err;

    const char *backend = memory->vtable->name ? memory->vtable->name(memory->ctx) : "unknown";
    bool healthy = memory->vtable->health_check ? memory->vtable->health_check(memory->ctx) : true;

    /* Per-category counts */
    size_t core_n = 0, daily_n = 0, conv_n = 0, insight_n = 0;
    if (memory->vtable->list) {
        sc_memory_entry_t *all = NULL;
        size_t all_count = 0;
        if (memory->vtable->list(memory->ctx, alloc, NULL, NULL, 0, &all, &all_count) == SC_OK &&
            all) {
            for (size_t i = 0; i < all_count; i++) {
                switch (all[i].category.tag) {
                case SC_MEMORY_CATEGORY_CORE:
                    core_n++;
                    break;
                case SC_MEMORY_CATEGORY_DAILY:
                    daily_n++;
                    break;
                case SC_MEMORY_CATEGORY_CONVERSATION:
                    conv_n++;
                    break;
                case SC_MEMORY_CATEGORY_INSIGHT:
                    insight_n++;
                    break;
                default:
                    daily_n++;
                    break;
                }
                sc_memory_entry_free_fields(alloc, &all[i]);
            }
            alloc->free(alloc->ctx, all, all_count * sizeof(sc_memory_entry_t));
        }
    }

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    cp_json_set_str(alloc, obj, "engine", backend ? backend : "unknown");
    sc_json_object_set(alloc, obj, "total_entries", sc_json_number_new(alloc, (double)count));
    sc_json_object_set(alloc, obj, "healthy", sc_json_bool_new(alloc, healthy));

    sc_json_value_t *cats = sc_json_object_new(alloc);
    if (cats) {
        sc_json_object_set(alloc, cats, "core", sc_json_number_new(alloc, (double)core_n));
        sc_json_object_set(alloc, cats, "daily", sc_json_number_new(alloc, (double)daily_n));
        sc_json_object_set(alloc, cats, "conversation", sc_json_number_new(alloc, (double)conv_n));
        sc_json_object_set(alloc, cats, "insight", sc_json_number_new(alloc, (double)insight_n));
        sc_json_object_set(alloc, obj, "categories", cats);
    }

    err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

sc_error_t cp_memory_list(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                          const sc_control_protocol_t *proto, const sc_json_value_t *root,
                          char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    if (!app || !app->agent || !app->agent->memory)
        return SC_ERR_NOT_SUPPORTED;

    sc_memory_t *memory = app->agent->memory;
    const sc_memory_category_t *cat_ptr = NULL;
    sc_memory_category_t cat = {0};

    if (root) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            const char *category = sc_json_get_string(params, "category");
            if (category && category[0]) {
                cat.tag = SC_MEMORY_CATEGORY_CUSTOM;
                cat.data.custom.name = category;
                cat.data.custom.name_len = strlen(category);
                cat_ptr = &cat;
            }
        }
    }

    sc_memory_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err = memory->vtable->list(memory->ctx, alloc, cat_ptr, NULL, 0, &entries, &count);
    if (err != SC_OK)
        return err;

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) {
        if (entries) {
            for (size_t i = 0; i < count; i++)
                sc_memory_entry_free_fields(alloc, &entries[i]);
            alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
        }
        return SC_ERR_OUT_OF_MEMORY;
    }

    sc_json_value_t *arr = sc_json_array_new(alloc);
    if (arr) {
        for (size_t i = 0; i < count; i++) {
            sc_json_value_t *e_obj = entry_to_json(alloc, &entries[i]);
            if (e_obj)
                sc_json_array_push(alloc, arr, e_obj);
        }
        sc_json_object_set(alloc, obj, "memories", arr);
    }

    if (entries) {
        for (size_t i = 0; i < count; i++)
            sc_memory_entry_free_fields(alloc, &entries[i]);
        alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
    }

    err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

sc_error_t cp_memory_recall(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                            const sc_control_protocol_t *proto, const sc_json_value_t *root,
                            char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    if (!app || !app->agent || !app->agent->memory)
        return SC_ERR_NOT_SUPPORTED;

    const char *query = NULL;
    size_t limit = 10;

    if (root) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            query = sc_json_get_string(params, "query");
            limit = (size_t)sc_json_get_number(params, "limit", 10.0);
            if (limit == 0 || limit > 1000)
                limit = 10;
        }
    }

    if (!query || !query[0]) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        sc_json_value_t *arr = sc_json_array_new(alloc);
        if (arr)
            sc_json_object_set(alloc, obj, "results", arr);
        sc_error_t err = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return err;
    }

    sc_memory_t *memory = app->agent->memory;
    size_t query_len = strlen(query);
    sc_memory_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err = memory->vtable->recall(memory->ctx, alloc, query, query_len, limit, NULL, 0,
                                            &entries, &count);
    if (err != SC_OK)
        return err;

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj) {
        if (entries) {
            for (size_t i = 0; i < count; i++)
                sc_memory_entry_free_fields(alloc, &entries[i]);
            alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
        }
        return SC_ERR_OUT_OF_MEMORY;
    }

    sc_json_value_t *arr = sc_json_array_new(alloc);
    if (arr) {
        for (size_t i = 0; i < count; i++) {
            sc_json_value_t *e_obj = entry_to_json(alloc, &entries[i]);
            if (e_obj)
                sc_json_array_push(alloc, arr, e_obj);
        }
        sc_json_object_set(alloc, obj, "results", arr);
    }

    if (entries) {
        for (size_t i = 0; i < count; i++)
            sc_memory_entry_free_fields(alloc, &entries[i]);
        alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
    }

    err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

sc_error_t cp_memory_store(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                           const sc_control_protocol_t *proto, const sc_json_value_t *root,
                           char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    if (!app || !app->agent || !app->agent->memory)
        return SC_ERR_NOT_SUPPORTED;

    const char *key = NULL;
    const char *content = NULL;
    const char *source = NULL;

    if (root) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            key = sc_json_get_string(params, "key");
            content = sc_json_get_string(params, "content");
            source = sc_json_get_string(params, "source");
        }
    }

    if (!key || !key[0]) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "key is required");
        sc_error_t e = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return e;
    }
    if (!content)
        content = "";

    sc_memory_t *memory = app->agent->memory;
    size_t source_len = (source && source[0]) ? strlen(source) : 0;
    if (source_len > 1024)
        source_len = 1024;

    sc_error_t err = sc_memory_store_with_source(memory, key, strlen(key), content, strlen(content),
                                                 NULL, NULL, 0, source, source_len);
    if (err != SC_OK)
        return err;

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "stored", sc_json_bool_new(alloc, true));
    err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

sc_error_t cp_memory_forget(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                            const sc_control_protocol_t *proto, const sc_json_value_t *root,
                            char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    if (!app || !app->agent || !app->agent->memory)
        return SC_ERR_NOT_SUPPORTED;

    const char *key = NULL;
    if (root) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params)
            key = sc_json_get_string(params, "key");
    }

    if (!key || !key[0]) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "key is required");
        sc_error_t e = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return e;
    }

    sc_memory_t *memory = app->agent->memory;
    bool deleted = false;
    sc_error_t err = memory->vtable->forget(memory->ctx, key, strlen(key), &deleted);
    if (err != SC_OK)
        return err;

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "deleted", sc_json_bool_new(alloc, deleted));
    err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

sc_error_t cp_memory_ingest(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                            const sc_control_protocol_t *proto, const sc_json_value_t *root,
                            char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    if (!app || !app->agent || !app->agent->memory)
        return SC_ERR_NOT_SUPPORTED;

    const char *text = NULL;
    const char *source = NULL;

    if (root) {
        sc_json_value_t *params = sc_json_object_get(root, "params");
        if (params) {
            text = sc_json_get_string(params, "text");
            source = sc_json_get_string(params, "source");
        }
    }

    if (!text || !text[0]) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "text is required");
        sc_error_t e = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return e;
    }
    if (!source || !source[0]) {
        sc_json_value_t *obj = sc_json_object_new(alloc);
        if (!obj)
            return SC_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "source is required");
        sc_error_t e = sc_json_stringify(alloc, obj, out, out_len);
        sc_json_free(alloc, obj);
        return e;
    }

    size_t source_len = strlen(source);
    if (source_len > 1024)
        source_len = 1024;

    char *key = sc_sprintf(alloc, "api-ingest:%.*s", (int)source_len, source);
    if (!key)
        return SC_ERR_OUT_OF_MEMORY;

    sc_memory_t *memory = app->agent->memory;
    sc_memory_category_t cat = {.tag = SC_MEMORY_CATEGORY_DAILY};
    size_t key_len = strlen(key);
    size_t text_len = strlen(text);

    sc_error_t err = sc_memory_store_with_source(memory, key, key_len, text, text_len, &cat, NULL,
                                                 0, source, source_len);
    sc_str_free(alloc, key);
    if (err != SC_OK)
        return err;

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "stored", sc_json_bool_new(alloc, true));
    err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

sc_error_t cp_memory_consolidate(sc_allocator_t *alloc, sc_app_context_t *app, sc_ws_conn_t *conn,
                                 const sc_control_protocol_t *proto, const sc_json_value_t *root,
                                 char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    if (!app || !app->agent || !app->agent->memory)
        return SC_ERR_NOT_SUPPORTED;

    sc_memory_t *memory = app->agent->memory;
    sc_consolidation_config_t config = SC_CONSOLIDATION_DEFAULTS;
    config.provider = &app->agent->provider;
    sc_error_t err = sc_memory_consolidate(alloc, memory, &config);
    if (err != SC_OK)
        return err;

    sc_json_value_t *obj = sc_json_object_new(alloc);
    if (!obj)
        return SC_ERR_OUT_OF_MEMORY;
    sc_json_object_set(alloc, obj, "consolidated", sc_json_bool_new(alloc, true));
    err = sc_json_stringify(alloc, obj, out, out_len);
    sc_json_free(alloc, obj);
    return err;
}

#endif /* SC_GATEWAY_POSIX */
