#ifndef HU_DAG_EXECUTOR_H
#define HU_DAG_EXECUTOR_H

#include "human/agent/dag.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stddef.h>

#define HU_DAG_MAX_BATCH_SIZE 8

typedef struct hu_dag_batch {
    hu_dag_node_t *nodes[HU_DAG_MAX_BATCH_SIZE];
    size_t count;
} hu_dag_batch_t;

hu_error_t hu_dag_next_batch(hu_dag_t *dag, hu_dag_batch_t *batch);
hu_error_t hu_dag_resolve_vars(hu_allocator_t *alloc, const hu_dag_t *dag, const char *args,
                               size_t args_len, char **resolved, size_t *resolved_len);

#endif
