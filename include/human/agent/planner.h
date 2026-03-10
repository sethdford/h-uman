#ifndef HU_AGENT_PLANNER_H
#define HU_AGENT_PLANNER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Agent Planner — lightweight planning from structured JSON
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_plan_step_status {
    HU_PLAN_STEP_PENDING,
    HU_PLAN_STEP_RUNNING,
    HU_PLAN_STEP_DONE,
    HU_PLAN_STEP_FAILED,
} hu_plan_step_status_t;

typedef struct hu_plan_step {
    char *tool_name;   /* owned */
    char *args_json;   /* owned; JSON object string */
    char *description; /* optional, owned */
    hu_plan_step_status_t status;
} hu_plan_step_t;

typedef struct hu_plan {
    hu_plan_step_t *steps; /* owned array */
    size_t steps_count;
    size_t steps_cap;
} hu_plan_t;

/* Create plan from structured JSON. Expected format:
 *   {"steps": [{"tool": "name", "args": {...}, "description": "..."}, ...]}
 * or {"steps": [{"name": "tool_name", "arguments": {...}}, ...]}
 * Caller must call hu_plan_free. */
hu_error_t hu_planner_create_plan(hu_allocator_t *alloc, const char *goal_json,
                                  size_t goal_json_len, hu_plan_t **out);

/* Generate a plan from a natural language goal by asking the LLM to decompose it.
 * Uses the provider to produce structured JSON, then parses it via hu_planner_create_plan.
 * tool_names: array of available tool names (used in the system prompt).
 * Caller must call hu_plan_free on the result. */
hu_error_t hu_planner_generate(hu_allocator_t *alloc, hu_provider_t *provider, const char *model,
                               size_t model_len, const char *goal, size_t goal_len,
                               const char *const *tool_names, size_t tool_count, hu_plan_t **out);

/* Generate a revised plan after a step failure. Takes the original goal,
 * what has succeeded so far, and what failed. Returns a new plan.
 * Under HU_IS_TEST, returns a minimal stub plan. */
hu_error_t hu_planner_replan(hu_allocator_t *alloc, hu_provider_t *provider, const char *model,
                             size_t model_len, const char *original_goal, size_t original_goal_len,
                             const char *progress_summary, size_t progress_summary_len,
                             const char *failure_detail, size_t failure_detail_len,
                             const char *const *tool_names, size_t tool_count, hu_plan_t **out);

/* Get next pending step, or NULL if none. Does not modify step status. */
hu_plan_step_t *hu_planner_next_step(const hu_plan_t *plan);

/* Mark step at index as done or failed. */
void hu_planner_mark_step(hu_plan_t *plan, size_t index, hu_plan_step_status_t status);

/* Check if all steps are done or failed (no pending/running). */
bool hu_planner_is_complete(const hu_plan_t *plan);

/* Free plan and all owned strings. */
void hu_plan_free(hu_allocator_t *alloc, hu_plan_t *plan);

#endif /* HU_AGENT_PLANNER_H */
