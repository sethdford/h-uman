#ifndef HU_AGENT_SWARM_H
#define HU_AGENT_SWARM_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Swarm — parallel agent execution for decomposed tasks.
 * Under HU_IS_TEST: executes sequentially with mock results (no real threads).
 * Production (POSIX): pthread-based parallel execution up to max_parallel workers.
 * Production (non-POSIX): sequential fallback.
 */

typedef struct hu_swarm_config {
    int max_parallel;        /* default 4 */
    int64_t timeout_ms;      /* per-agent timeout, default 30000 */
    int retry_on_failure;    /* default 1 */
    hu_provider_t *provider; /* LLM provider for sub-agent calls (NULL = echo fallback) */
    const char *model;       /* model name */
    size_t model_len;
    hu_tool_t *tools; /* tool registry (reserved; single-round chat may use later) */
    size_t tools_count;
} hu_swarm_config_t;

typedef struct hu_swarm_task {
    char description[512];
    size_t description_len;
    bool completed;
    bool failed;
    char result[2048];
    size_t result_len;
    int64_t elapsed_ms;
} hu_swarm_task_t;

typedef enum {
    HU_SWARM_AGG_CONCATENATE = 0,
    HU_SWARM_AGG_FIRST_SUCCESS,
    HU_SWARM_AGG_VOTE,
    HU_SWARM_AGG_LLM_SYNTHESIZE
} hu_swarm_aggregation_t;

typedef struct hu_swarm_result {
    hu_swarm_task_t *tasks;
    size_t task_count;
    size_t completed;
    size_t failed;
    int64_t total_elapsed_ms;
    hu_swarm_aggregation_t aggregation;
} hu_swarm_result_t;

hu_swarm_config_t hu_swarm_config_default(void);

hu_error_t hu_swarm_execute(hu_allocator_t *alloc, const hu_swarm_config_t *config,
                            hu_swarm_task_t *tasks, size_t task_count, hu_swarm_result_t *result);

hu_error_t hu_swarm_aggregate(const hu_swarm_result_t *result, hu_swarm_aggregation_t strategy,
                              char *out, size_t out_size, size_t *out_len);

hu_error_t hu_swarm_aggregate_llm(hu_allocator_t *alloc, const hu_swarm_result_t *result,
                                  hu_provider_t *provider, const char *model, size_t model_len,
                                  char *out, size_t out_size, size_t *out_len);

void hu_swarm_result_free(hu_allocator_t *alloc, hu_swarm_result_t *result);

#endif /* HU_AGENT_SWARM_H */
