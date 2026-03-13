#ifndef HU_AGENT_ORCHESTRATOR_H
#define HU_AGENT_ORCHESTRATOR_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Multi-Agent Orchestrator — coordinates task decomposition, negotiation,
 * result merging, and conflict resolution among multiple agents.
 */

#define HU_ORCHESTRATOR_MAX_AGENTS 8
#define HU_ORCHESTRATOR_MAX_TASKS 32

typedef enum hu_task_status {
    HU_TASK_UNASSIGNED = 0,
    HU_TASK_ASSIGNED,
    HU_TASK_IN_PROGRESS,
    HU_TASK_COMPLETED,
    HU_TASK_FAILED,
} hu_task_status_t;

typedef struct hu_orchestrator_task {
    uint32_t id;
    char description[512];
    size_t description_len;
    char assigned_agent[128];
    size_t assigned_agent_len;
    hu_task_status_t status;
    char result[2048];
    size_t result_len;
    double priority;
    uint32_t depends_on; /* 0 = no dependency */
} hu_orchestrator_task_t;

typedef struct hu_agent_capability {
    char agent_id[128];
    size_t agent_id_len;
    char role[64];
    size_t role_len;
    char skills[512];
    size_t skills_len;
    double capacity; /* 0.0–1.0, current availability */
} hu_agent_capability_t;

typedef struct hu_orchestrator {
    hu_allocator_t *alloc;
    hu_orchestrator_task_t tasks[HU_ORCHESTRATOR_MAX_TASKS];
    size_t task_count;
    hu_agent_capability_t agents[HU_ORCHESTRATOR_MAX_AGENTS];
    size_t agent_count;
    uint32_t next_task_id;
} hu_orchestrator_t;

hu_error_t hu_orchestrator_create(hu_allocator_t *alloc, hu_orchestrator_t *out);
void hu_orchestrator_deinit(hu_orchestrator_t *orch);

/* Register an agent with its capabilities. */
hu_error_t hu_orchestrator_register_agent(hu_orchestrator_t *orch,
                                          const char *agent_id, size_t id_len,
                                          const char *role, size_t role_len,
                                          const char *skills, size_t skills_len);

/* Propose a task split: decomposes a goal into tasks matched to agents. */
hu_error_t hu_orchestrator_propose_split(hu_orchestrator_t *orch,
                                         const char *goal, size_t goal_len,
                                         const char **subtasks,
                                         const size_t *subtask_lens,
                                         size_t subtask_count);

/* Assign a task to the best available agent. */
hu_error_t hu_orchestrator_assign_task(hu_orchestrator_t *orch, uint32_t task_id,
                                       const char *agent_id, size_t agent_id_len);

/* Record task completion with result. */
hu_error_t hu_orchestrator_complete_task(hu_orchestrator_t *orch, uint32_t task_id,
                                         const char *result, size_t result_len);

/* Record task failure. */
hu_error_t hu_orchestrator_fail_task(hu_orchestrator_t *orch, uint32_t task_id,
                                     const char *reason, size_t reason_len);

/* Merge results from all completed tasks. Caller must free *out. */
hu_error_t hu_orchestrator_merge_results(hu_orchestrator_t *orch,
                                         hu_allocator_t *alloc,
                                         char **out, size_t *out_len);

/* Check if all tasks are completed. */
bool hu_orchestrator_all_complete(const hu_orchestrator_t *orch);

/* Get count of tasks by status. */
size_t hu_orchestrator_count_by_status(const hu_orchestrator_t *orch,
                                       hu_task_status_t status);

/* Find next unblocked, unassigned task. */
hu_error_t hu_orchestrator_next_task(hu_orchestrator_t *orch,
                                     hu_orchestrator_task_t **out);

const char *hu_task_status_str(hu_task_status_t status);

/* Forward declaration */
typedef struct hu_agent_registry hu_agent_registry_t;

/* Populate orchestrator agents from the registry. Iterates all registered
 * agent configs and calls hu_orchestrator_register_agent for each. */
hu_error_t hu_orchestrator_load_from_registry(hu_orchestrator_t *orch,
                                               const hu_agent_registry_t *registry);

#endif /* HU_AGENT_ORCHESTRATOR_H */
