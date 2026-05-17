#ifndef HU_EVAL_STOCK_BASELINE_H
#define HU_EVAL_STOCK_BASELINE_H

/* Phase 5 Task 6 — stock (un-LoRA'd) provider judge. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/eval/eval_judge_external.h"
#include "human/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

hu_error_t hu_stock_baseline_create(hu_allocator_t *alloc, hu_provider_t *provider,
                                    hu_eval_judge_external_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HU_EVAL_STOCK_BASELINE_H */
