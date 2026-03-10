#ifndef HU_DAG_H
#define HU_DAG_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

#define HU_DAG_MAX_NODES 32
#define HU_DAG_MAX_DEPS   8

typedef enum hu_dag_node_status {
    HU_DAG_PENDING,
    HU_DAG_READY,
    HU_DAG_RUNNING,
    HU_DAG_DONE,
    HU_DAG_FAILED,
} hu_dag_node_status_t;

typedef struct hu_dag_node {
    char *id;
    char *tool_name;
    char *args_json;
    char *deps[HU_DAG_MAX_DEPS];
    size_t dep_count;
    hu_dag_node_status_t status;
    char *result;
    size_t result_len;
} hu_dag_node_t;

typedef struct hu_dag {
    hu_allocator_t alloc;
    hu_dag_node_t nodes[HU_DAG_MAX_NODES];
    size_t node_count;
} hu_dag_t;

hu_error_t hu_dag_init(hu_dag_t *dag, hu_allocator_t alloc);
hu_error_t hu_dag_add_node(hu_dag_t *dag, const char *id, const char *tool_name,
                           const char *args_json, const char **deps, size_t dep_count);
hu_error_t hu_dag_validate(const hu_dag_t *dag);
hu_error_t hu_dag_parse_json(hu_dag_t *dag, hu_allocator_t *alloc, const char *json,
                             size_t json_len);
bool hu_dag_is_complete(const hu_dag_t *dag);
hu_dag_node_t *hu_dag_find_node(hu_dag_t *dag, const char *id, size_t id_len);
void hu_dag_deinit(hu_dag_t *dag);

#endif
