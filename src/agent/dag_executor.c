#include "human/agent/dag_executor.h"
#include "human/core/string.h"
#include <ctype.h>
#include <string.h>

hu_error_t hu_dag_next_batch(hu_dag_t *dag, hu_dag_batch_t *batch) {
    if (!dag || !batch)
        return HU_ERR_INVALID_ARGUMENT;
    batch->count = 0;

    for (size_t i = 0; i < dag->node_count && batch->count < HU_DAG_MAX_BATCH_SIZE; i++) {
        hu_dag_node_t *n = &dag->nodes[i];
        if (n->status != HU_DAG_PENDING)
            continue;

        bool all_deps_done = true;
        for (size_t d = 0; d < n->dep_count; d++) {
            hu_dag_node_t *dep =
                hu_dag_find_node(dag, n->deps[d], n->deps[d] ? strlen(n->deps[d]) : 0);
            if (!dep || dep->status != HU_DAG_DONE) {
                all_deps_done = false;
                break;
            }
        }
        if (all_deps_done) {
            n->status = HU_DAG_READY;
            batch->nodes[batch->count++] = n;
        }
    }
    return HU_OK;
}

static bool is_alnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

hu_error_t hu_dag_resolve_vars(hu_allocator_t *alloc, const hu_dag_t *dag, const char *args,
                               size_t args_len, char **resolved, size_t *resolved_len) {
    if (!alloc || !dag || !resolved || !resolved_len)
        return HU_ERR_INVALID_ARGUMENT;
    *resolved = NULL;
    *resolved_len = 0;

    if (!args || args_len == 0) {
        *resolved = hu_strndup(alloc, "", 0);
        if (!*resolved)
            return HU_ERR_OUT_OF_MEMORY;
        *resolved_len = 0;
        return HU_OK;
    }

    hu_json_buf_t buf;
    hu_error_t err = hu_json_buf_init(&buf, alloc);
    if (err != HU_OK)
        return err;

    size_t i = 0;
    while (i < args_len) {
        if (args[i] == '$' && i + 1 < args_len && (args[i + 1] == 't' || args[i + 1] == 'T')) {
            size_t start = i + 1;
            size_t end = start + 1;
            while (end < args_len && is_alnum(args[end]))
                end++;
            if (end > start) {
                const char *ref = args + start;
                size_t ref_len = end - start;
                hu_dag_node_t *node =
                    hu_dag_find_node((hu_dag_t *)dag, ref, ref_len);
                if (node && node->status == HU_DAG_DONE && node->result && node->result_len > 0) {
                    err = hu_json_buf_append_raw(&buf, node->result, node->result_len);
                } else {
                    err = hu_json_buf_append_raw(&buf, args + i, end - i);
                }
                if (err != HU_OK) {
                    hu_json_buf_free(&buf);
                    return err;
                }
                i = end;
                continue;
            }
        }
        err = hu_json_buf_append_raw(&buf, args + i, 1);
        if (err != HU_OK) {
            hu_json_buf_free(&buf);
            return err;
        }
        i++;
    }

    {
        char nul = '\0';
        err = hu_json_buf_append_raw(&buf, &nul, 1);
        if (err != HU_OK) {
            hu_json_buf_free(&buf);
            return err;
        }
    }

    *resolved = buf.ptr;
    *resolved_len = buf.len > 0 ? buf.len - 1 : 0;
    buf.ptr = NULL;
    buf.len = 0;
    buf.cap = 0;
    hu_json_buf_free(&buf);
    return HU_OK;
}
