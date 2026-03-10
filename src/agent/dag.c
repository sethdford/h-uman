#include "human/agent/dag.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <string.h>

hu_error_t hu_dag_init(hu_dag_t *dag, hu_allocator_t alloc) {
    if (!dag)
        return HU_ERR_INVALID_ARGUMENT;
    memset(dag, 0, sizeof(*dag));
    dag->alloc = alloc;
    return HU_OK;
}

hu_error_t hu_dag_add_node(hu_dag_t *dag, const char *id, const char *tool_name,
                          const char *args_json, const char **deps, size_t dep_count) {
    if (!dag || !id || !tool_name)
        return HU_ERR_INVALID_ARGUMENT;
    if (dag->node_count >= HU_DAG_MAX_NODES)
        return HU_ERR_INVALID_ARGUMENT;
    if (dep_count > HU_DAG_MAX_DEPS)
        return HU_ERR_INVALID_ARGUMENT;

    hu_allocator_t *a = &dag->alloc;
    hu_dag_node_t *n = &dag->nodes[dag->node_count];
    memset(n, 0, sizeof(*n));

    n->id = hu_strdup(a, id);
    if (!n->id)
        return HU_ERR_OUT_OF_MEMORY;
    n->tool_name = hu_strdup(a, tool_name);
    if (!n->tool_name) {
        a->free(a->ctx, n->id, strlen(n->id) + 1);
        n->id = NULL;
        return HU_ERR_OUT_OF_MEMORY;
    }
    n->args_json = args_json ? hu_strdup(a, args_json) : NULL;
    if (args_json && !n->args_json) {
        a->free(a->ctx, n->id, strlen(n->id) + 1);
        a->free(a->ctx, n->tool_name, strlen(n->tool_name) + 1);
        n->id = NULL;
        n->tool_name = NULL;
        return HU_ERR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < dep_count && i < HU_DAG_MAX_DEPS; i++) {
        if (deps[i]) {
            n->deps[n->dep_count] = hu_strdup(a, deps[i]);
            if (n->deps[n->dep_count])
                n->dep_count++;
        }
    }
    n->status = HU_DAG_PENDING;
    dag->node_count++;
    return HU_OK;
}

typedef enum { DAG_VISIT_NONE, DAG_VISIT_VISITING, DAG_VISIT_VISITED } dag_visit_t;

static bool dag_has_cycle_visit(const hu_dag_t *dag, size_t idx, dag_visit_t *visited) {
    if (visited[idx] == DAG_VISIT_VISITING)
        return true;
    if (visited[idx] == DAG_VISIT_VISITED)
        return false;
    visited[idx] = DAG_VISIT_VISITING;
    const hu_dag_node_t *n = &dag->nodes[idx];
    for (size_t i = 0; i < n->dep_count; i++) {
        hu_dag_node_t *dep = hu_dag_find_node((hu_dag_t *)dag, n->deps[i],
                                              n->deps[i] ? strlen(n->deps[i]) : 0);
        if (!dep)
            continue;
        size_t dep_idx = (size_t)(dep - dag->nodes);
        if (dag_has_cycle_visit(dag, dep_idx, visited))
            return true;
    }
    visited[idx] = DAG_VISIT_VISITED;
    return false;
}

hu_error_t hu_dag_validate(const hu_dag_t *dag) {
    if (!dag)
        return HU_ERR_INVALID_ARGUMENT;

    dag_visit_t visited[HU_DAG_MAX_NODES];
    memset(visited, 0, sizeof(visited));

    for (size_t i = 0; i < dag->node_count; i++) {
        const hu_dag_node_t *n = &dag->nodes[i];
        for (size_t j = i + 1; j < dag->node_count; j++) {
            if (n->id && dag->nodes[j].id && strcmp(n->id, dag->nodes[j].id) == 0)
                return HU_ERR_ALREADY_EXISTS;
        }
    }

    for (size_t i = 0; i < dag->node_count; i++) {
        const hu_dag_node_t *n = &dag->nodes[i];
        for (size_t d = 0; d < n->dep_count; d++) {
            if (!n->deps[d])
                continue;
            if (!hu_dag_find_node((hu_dag_t *)dag, n->deps[d], strlen(n->deps[d])))
                return HU_ERR_NOT_FOUND;
        }
    }

    for (size_t i = 0; i < dag->node_count; i++) {
        if (dag_has_cycle_visit(dag, i, visited))
            return HU_ERR_PARSE;
    }
    return HU_OK;
}

hu_error_t hu_dag_parse_json(hu_dag_t *dag, hu_allocator_t *alloc, const char *json,
                             size_t json_len) {
    if (!dag || !alloc || !json)
        return HU_ERR_INVALID_ARGUMENT;

    if (!dag->alloc.alloc)
        dag->alloc = *alloc;

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, json, json_len, &root);
    if (err != HU_OK || !root || root->type != HU_JSON_OBJECT)
        return err != HU_OK ? err : HU_ERR_JSON_PARSE;

    hu_json_value_t *tasks = hu_json_object_get(root, "tasks");
    if (!tasks || tasks->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, root);
        return HU_ERR_JSON_PARSE;
    }

    for (size_t i = 0; i < tasks->data.array.len; i++) {
        hu_json_value_t *t = tasks->data.array.items[i];
        if (!t || t->type != HU_JSON_OBJECT)
            continue;

        const char *id = hu_json_get_string(t, "id");
        const char *tool = hu_json_get_string(t, "tool");
        if (!id || !tool)
            continue;

        const char *args_str = "{}";
        char *args_out = NULL;
        size_t args_len = 0;
        hu_json_value_t *args_val = hu_json_object_get(t, "args");
        if (args_val && hu_json_stringify(alloc, args_val, &args_out, &args_len) == HU_OK &&
            args_out) {
            args_str = args_out;
        }

        const char *deps_arr[HU_DAG_MAX_DEPS];
        size_t deps_count = 0;
        hu_json_value_t *deps_val = hu_json_object_get(t, "deps");
        if (deps_val && deps_val->type == HU_JSON_ARRAY) {
            for (size_t k = 0; k < deps_val->data.array.len && k < HU_DAG_MAX_DEPS; k++) {
                hu_json_value_t *dv = deps_val->data.array.items[k];
                if (dv && dv->type == HU_JSON_STRING && dv->data.string.ptr) {
                    deps_arr[deps_count++] = dv->data.string.ptr;
                }
            }
        }

        err = hu_dag_add_node(dag, id, tool, args_str, deps_count ? deps_arr : NULL, deps_count);
        if (args_out)
            hu_str_free(alloc, args_out);
        if (err != HU_OK) {
            hu_json_free(alloc, root);
            return err;
        }
    }
    hu_json_free(alloc, root);
    return HU_OK;
}

bool hu_dag_is_complete(const hu_dag_t *dag) {
    if (!dag)
        return true;
    for (size_t i = 0; i < dag->node_count; i++) {
        hu_dag_node_status_t s = dag->nodes[i].status;
        if (s != HU_DAG_DONE && s != HU_DAG_FAILED)
            return false;
    }
    return true;
}

hu_dag_node_t *hu_dag_find_node(hu_dag_t *dag, const char *id, size_t id_len) {
    if (!dag || !id)
        return NULL;
    size_t len = id_len > 0 ? id_len : strlen(id);
    for (size_t i = 0; i < dag->node_count; i++) {
        if (dag->nodes[i].id && strlen(dag->nodes[i].id) == len &&
            memcmp(dag->nodes[i].id, id, len) == 0)
            return &dag->nodes[i];
    }
    return NULL;
}

void hu_dag_deinit(hu_dag_t *dag) {
    if (!dag)
        return;
    hu_allocator_t *alloc = &dag->alloc;
    for (size_t i = 0; i < dag->node_count; i++) {
        hu_dag_node_t *n = &dag->nodes[i];
        if (n->id) {
            alloc->free(alloc->ctx, n->id, strlen(n->id) + 1);
            n->id = NULL;
        }
        if (n->tool_name) {
            alloc->free(alloc->ctx, n->tool_name, strlen(n->tool_name) + 1);
            n->tool_name = NULL;
        }
        if (n->args_json) {
            alloc->free(alloc->ctx, n->args_json, strlen(n->args_json) + 1);
            n->args_json = NULL;
        }
        for (size_t d = 0; d < n->dep_count; d++) {
            if (n->deps[d]) {
                alloc->free(alloc->ctx, n->deps[d], strlen(n->deps[d]) + 1);
                n->deps[d] = NULL;
            }
        }
        n->dep_count = 0;
        if (n->result) {
            alloc->free(alloc->ctx, n->result, n->result_len + 1);
            n->result = NULL;
            n->result_len = 0;
        }
    }
    dag->node_count = 0;
}
