#ifdef HU_GATEWAY_POSIX

#include "cp_internal.h"
#include "human/agent.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/consolidation.h"
#include "human/memory/graph.h"
#include <stdio.h>
#include <string.h>

static const char *category_to_str(const hu_memory_category_t *cat) {
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

static hu_json_value_t *entry_to_json(hu_allocator_t *alloc, const hu_memory_entry_t *e) {
    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return NULL;
    cp_json_set_str(alloc, obj, "key", e->key ? e->key : "");
    cp_json_set_str(alloc, obj, "content", e->content ? e->content : "");
    cp_json_set_str(alloc, obj, "category", category_to_str(&e->category));
    cp_json_set_str(alloc, obj, "timestamp", e->timestamp ? e->timestamp : "");
    cp_json_set_str(alloc, obj, "source", e->source ? e->source : "");
    hu_json_object_set(alloc, obj, "score",
                       hu_json_number_new(alloc, (e->score == e->score) ? e->score : 0.0));
    return obj;
}

hu_error_t cp_memory_status(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                            const hu_control_protocol_t *proto, const hu_json_value_t *root,
                            char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    if (!app || !app->agent || !app->agent->memory)
        return HU_ERR_NOT_SUPPORTED;

    hu_memory_t *memory = app->agent->memory;
    size_t count = 0;
    hu_error_t err = memory->vtable->count(memory->ctx, &count);
    if (err != HU_OK)
        return err;

    const char *backend = memory->vtable->name ? memory->vtable->name(memory->ctx) : "unknown";
    bool healthy = memory->vtable->health_check ? memory->vtable->health_check(memory->ctx) : true;

    /* Per-category counts */
    size_t core_n = 0, daily_n = 0, conv_n = 0, insight_n = 0;
    if (memory->vtable->list) {
        hu_memory_entry_t *all = NULL;
        size_t all_count = 0;
        if (memory->vtable->list(memory->ctx, alloc, NULL, NULL, 0, &all, &all_count) == HU_OK &&
            all) {
            for (size_t i = 0; i < all_count; i++) {
                switch (all[i].category.tag) {
                case HU_MEMORY_CATEGORY_CORE:
                    core_n++;
                    break;
                case HU_MEMORY_CATEGORY_DAILY:
                    daily_n++;
                    break;
                case HU_MEMORY_CATEGORY_CONVERSATION:
                    conv_n++;
                    break;
                case HU_MEMORY_CATEGORY_INSIGHT:
                    insight_n++;
                    break;
                default:
                    daily_n++;
                    break;
                }
                hu_memory_entry_free_fields(alloc, &all[i]);
            }
            alloc->free(alloc->ctx, all, all_count * sizeof(hu_memory_entry_t));
        }
    }

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    cp_json_set_str(alloc, obj, "engine", backend ? backend : "unknown");
    hu_json_object_set(alloc, obj, "total_entries", hu_json_number_new(alloc, (double)count));
    hu_json_object_set(alloc, obj, "healthy", hu_json_bool_new(alloc, healthy));

    hu_json_value_t *cats = hu_json_object_new(alloc);
    if (cats) {
        hu_json_object_set(alloc, cats, "core", hu_json_number_new(alloc, (double)core_n));
        hu_json_object_set(alloc, cats, "daily", hu_json_number_new(alloc, (double)daily_n));
        hu_json_object_set(alloc, cats, "conversation", hu_json_number_new(alloc, (double)conv_n));
        hu_json_object_set(alloc, cats, "insight", hu_json_number_new(alloc, (double)insight_n));
        hu_json_object_set(alloc, obj, "categories", cats);
    }

    err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

hu_error_t cp_memory_list(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                          const hu_control_protocol_t *proto, const hu_json_value_t *root,
                          char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    if (!app || !app->agent || !app->agent->memory)
        return HU_ERR_NOT_SUPPORTED;

    hu_memory_t *memory = app->agent->memory;
    const hu_memory_category_t *cat_ptr = NULL;
    hu_memory_category_t cat = {0};

    if (root) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            const char *category = hu_json_get_string(params, "category");
            if (category && category[0]) {
                cat.tag = HU_MEMORY_CATEGORY_CUSTOM;
                cat.data.custom.name = category;
                cat.data.custom.name_len = strlen(category);
                cat_ptr = &cat;
            }
        }
    }

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = memory->vtable->list(memory->ctx, alloc, cat_ptr, NULL, 0, &entries, &count);
    if (err != HU_OK)
        return err;

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj) {
        if (entries) {
            for (size_t i = 0; i < count; i++)
                hu_memory_entry_free_fields(alloc, &entries[i]);
            alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
        }
        return HU_ERR_OUT_OF_MEMORY;
    }

    hu_json_value_t *arr = hu_json_array_new(alloc);
    if (arr) {
        for (size_t i = 0; i < count; i++) {
            hu_json_value_t *e_obj = entry_to_json(alloc, &entries[i]);
            if (e_obj)
                hu_json_array_push(alloc, arr, e_obj);
        }
        hu_json_object_set(alloc, obj, "entries", arr);
    }

    if (entries) {
        for (size_t i = 0; i < count; i++)
            hu_memory_entry_free_fields(alloc, &entries[i]);
        alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
    }

    err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

hu_error_t cp_memory_recall(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                            const hu_control_protocol_t *proto, const hu_json_value_t *root,
                            char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    if (!app || !app->agent || !app->agent->memory)
        return HU_ERR_NOT_SUPPORTED;

    const char *query = NULL;
    size_t limit = 10;

    if (root) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            query = hu_json_get_string(params, "query");
            limit = (size_t)hu_json_get_number(params, "limit", 10.0);
            if (limit == 0 || limit > 1000)
                limit = 10;
        }
    }

    if (!query || !query[0]) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj)
            return HU_ERR_OUT_OF_MEMORY;
        hu_json_value_t *arr = hu_json_array_new(alloc);
        if (arr)
            hu_json_object_set(alloc, obj, "entries", arr);
        hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return err;
    }

    hu_memory_t *memory = app->agent->memory;
    size_t query_len = strlen(query);
    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = memory->vtable->recall(memory->ctx, alloc, query, query_len, limit, NULL, 0,
                                            &entries, &count);
    if (err != HU_OK)
        return err;

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj) {
        if (entries) {
            for (size_t i = 0; i < count; i++)
                hu_memory_entry_free_fields(alloc, &entries[i]);
            alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
        }
        return HU_ERR_OUT_OF_MEMORY;
    }

    hu_json_value_t *arr = hu_json_array_new(alloc);
    if (arr) {
        for (size_t i = 0; i < count; i++) {
            hu_json_value_t *e_obj = entry_to_json(alloc, &entries[i]);
            if (e_obj)
                hu_json_array_push(alloc, arr, e_obj);
        }
        hu_json_object_set(alloc, obj, "entries", arr);
    }

    if (entries) {
        for (size_t i = 0; i < count; i++)
            hu_memory_entry_free_fields(alloc, &entries[i]);
        alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
    }

    err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

hu_error_t cp_memory_store(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                           const hu_control_protocol_t *proto, const hu_json_value_t *root,
                           char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    if (!app || !app->agent || !app->agent->memory)
        return HU_ERR_NOT_SUPPORTED;

    const char *key = NULL;
    const char *content = NULL;
    const char *source = NULL;

    if (root) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            key = hu_json_get_string(params, "key");
            content = hu_json_get_string(params, "content");
            source = hu_json_get_string(params, "source");
        }
    }

    if (!key || !key[0]) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj)
            return HU_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "key is required");
        hu_error_t e = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return e;
    }
    if (!content)
        content = "";

    hu_memory_t *memory = app->agent->memory;
    size_t source_len = (source && source[0]) ? strlen(source) : 0;
    if (source_len > 1024)
        source_len = 1024;

    hu_error_t err = hu_memory_store_with_source(memory, key, strlen(key), content, strlen(content),
                                                 NULL, NULL, 0, source, source_len);
    if (err != HU_OK)
        return err;

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "stored", hu_json_bool_new(alloc, true));
    err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

hu_error_t cp_memory_forget(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                            const hu_control_protocol_t *proto, const hu_json_value_t *root,
                            char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    if (!app || !app->agent || !app->agent->memory)
        return HU_ERR_NOT_SUPPORTED;

    const char *key = NULL;
    if (root) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params)
            key = hu_json_get_string(params, "key");
    }

    if (!key || !key[0]) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj)
            return HU_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "key is required");
        hu_error_t e = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return e;
    }

    hu_memory_t *memory = app->agent->memory;
    bool deleted = false;
    hu_error_t err = memory->vtable->forget(memory->ctx, key, strlen(key), &deleted);
    if (err != HU_OK)
        return err;

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "deleted", hu_json_bool_new(alloc, deleted));
    err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

hu_error_t cp_memory_ingest(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                            const hu_control_protocol_t *proto, const hu_json_value_t *root,
                            char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    if (!app || !app->agent || !app->agent->memory)
        return HU_ERR_NOT_SUPPORTED;

    const char *text = NULL;
    const char *source = NULL;

    if (root) {
        hu_json_value_t *params = hu_json_object_get(root, "params");
        if (params) {
            text = hu_json_get_string(params, "text");
            source = hu_json_get_string(params, "source");
        }
    }

    if (!text || !text[0]) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj)
            return HU_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "text is required");
        hu_error_t e = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return e;
    }
    if (!source || !source[0]) {
        hu_json_value_t *obj = hu_json_object_new(alloc);
        if (!obj)
            return HU_ERR_OUT_OF_MEMORY;
        cp_json_set_str(alloc, obj, "error", "source is required");
        hu_error_t e = hu_json_stringify(alloc, obj, out, out_len);
        hu_json_free(alloc, obj);
        return e;
    }

    size_t source_len = strlen(source);
    if (source_len > 1024)
        source_len = 1024;

    char *key = hu_sprintf(alloc, "api-ingest:%.*s", (int)source_len, source);
    if (!key)
        return HU_ERR_OUT_OF_MEMORY;

    hu_memory_t *memory = app->agent->memory;
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_DAILY};
    size_t key_len = strlen(key);
    size_t text_len = strlen(text);

    hu_error_t err = hu_memory_store_with_source(memory, key, key_len, text, text_len, &cat, NULL,
                                                 0, source, source_len);
    hu_str_free(alloc, key);
    if (err != HU_OK)
        return err;

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "stored", hu_json_bool_new(alloc, true));
    err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

hu_error_t cp_memory_graph(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                           const hu_control_protocol_t *proto, const hu_json_value_t *root,
                           char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;

    hu_json_value_t *entities_arr = hu_json_array_new(alloc);
    hu_json_value_t *relations_arr = hu_json_array_new(alloc);

    if (app && app->graph) {
        hu_graph_entity_t *entities = NULL;
        size_t entity_count = 0;
        if (hu_graph_list_entities(app->graph, alloc, 100, &entities, &entity_count) == HU_OK &&
            entities) {
            for (size_t i = 0; i < entity_count; i++) {
                hu_json_value_t *e = hu_json_object_new(alloc);
                if (!e)
                    continue;
                hu_json_object_set(alloc, e, "id",
                                   hu_json_number_new(alloc, (double)entities[i].id));
                cp_json_set_str(alloc, e, "name", entities[i].name ? entities[i].name : "");
                cp_json_set_str(alloc, e, "type", hu_entity_type_to_string(entities[i].type));
                hu_json_object_set(alloc, e, "mention_count",
                                   hu_json_number_new(alloc, (double)entities[i].mention_count));
                if (entities_arr)
                    hu_json_array_push(alloc, entities_arr, e);
            }
            hu_graph_entities_free(alloc, entities, entity_count);
        }

        hu_graph_relation_t *relations = NULL;
        size_t relation_count = 0;
        if (hu_graph_list_relations(app->graph, alloc, 200, &relations, &relation_count) == HU_OK &&
            relations) {
            for (size_t i = 0; i < relation_count; i++) {
                hu_json_value_t *r = hu_json_object_new(alloc);
                if (!r)
                    continue;
                hu_json_object_set(alloc, r, "source",
                                   hu_json_number_new(alloc, (double)relations[i].source_id));
                hu_json_object_set(alloc, r, "target",
                                   hu_json_number_new(alloc, (double)relations[i].target_id));
                cp_json_set_str(alloc, r, "type", hu_relation_type_to_string(relations[i].type));
                hu_json_object_set(alloc, r, "weight",
                                   hu_json_number_new(alloc, (double)relations[i].weight));
                if (relations_arr)
                    hu_json_array_push(alloc, relations_arr, r);
            }
            hu_graph_relations_free(alloc, relations, relation_count);
        }
    }

    if (entities_arr)
        hu_json_object_set(alloc, obj, "entities", entities_arr);
    if (relations_arr)
        hu_json_object_set(alloc, obj, "relations", relations_arr);
    hu_error_t err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

hu_error_t cp_memory_consolidate(hu_allocator_t *alloc, hu_app_context_t *app, hu_ws_conn_t *conn,
                                 const hu_control_protocol_t *proto, const hu_json_value_t *root,
                                 char **out, size_t *out_len) {
    (void)conn;
    (void)proto;
    (void)root;
    if (!app || !app->agent || !app->agent->memory)
        return HU_ERR_NOT_SUPPORTED;

    hu_memory_t *memory = app->agent->memory;
    hu_consolidation_config_t config = HU_CONSOLIDATION_DEFAULTS;
    config.provider = &app->agent->provider;
    config.model = app->agent->model_name;
    config.model_len = app->agent->model_name_len;
    hu_error_t err = hu_memory_consolidate(alloc, memory, &config);
    if (err != HU_OK)
        return err;

    hu_json_value_t *obj = hu_json_object_new(alloc);
    if (!obj)
        return HU_ERR_OUT_OF_MEMORY;
    hu_json_object_set(alloc, obj, "consolidated", hu_json_bool_new(alloc, true));
    err = hu_json_stringify(alloc, obj, out, out_len);
    hu_json_free(alloc, obj);
    return err;
}

#endif /* HU_GATEWAY_POSIX */
