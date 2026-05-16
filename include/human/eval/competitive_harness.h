#ifndef HU_EVAL_COMPETITIVE_HARNESS_H
#define HU_EVAL_COMPETITIVE_HARNESS_H

/* Phase 5 Task 9 — competitive eval scorecard orchestrator. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/eval/eval_judge_external.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_competitive_harness_config {
    const char *prompt_fixture;
    const char *out_markdown;
    const char *out_json;
    size_t min_available;
} hu_competitive_harness_config_t;

typedef struct hu_competitive_harness_judge_slot {
    const char *column_name;
    hu_eval_judge_external_t judge;
    bool available;
    const char *unavailable_reason;
} hu_competitive_harness_judge_slot_t;

typedef struct hu_competitive_harness_result {
    size_t n_available;
    size_t n_columns;
    char summary[256];
} hu_competitive_harness_result_t;

hu_error_t hu_competitive_harness_run_with_test_judges(
    hu_allocator_t *alloc, const hu_competitive_harness_config_t *cfg,
    const hu_competitive_harness_judge_slot_t *judges, size_t n_judges,
    hu_competitive_harness_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HU_EVAL_COMPETITIVE_HARNESS_H */
