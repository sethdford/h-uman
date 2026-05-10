#ifndef HU_EVALUATION_INTERNAL_H
#define HU_EVALUATION_INTERNAL_H

/* Private helpers shared by W16 backend implementations. Not part of the
 * public API; do not include from anywhere outside src/evaluation/. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/evaluation/evaluation.h"

#include <stddef.h>

/* Initialise an empty report on the given allocator. Allocates the metrics
 * array up to HU_EVALUATION_MAX_METRICS so backends can fill it without a
 * second alloc. Caller frees via `hu_evaluation_report_free`. */
hu_error_t hu_evaluation_report_init(hu_allocator_t *alloc, const char *suite_name,
                                     hu_evaluation_run_report_t *out);

/* Append one metric. Score is clamped to [0.0, 1.0]; baseline is set to NaN
 * (regression layer fills it). Returns HU_ERR_OUT_OF_MEMORY on allocation
 * failure or if the report is already at HU_EVALUATION_MAX_METRICS. */
hu_error_t hu_evaluation_report_add_metric(hu_allocator_t *alloc, hu_evaluation_run_report_t *r,
                                           const char *name, double score, size_t sample_count);

/* Set or replace the (nullable) `error_summary` string. Frees any prior
 * value. Returns HU_ERR_OUT_OF_MEMORY if alloc fails. */
hu_error_t hu_evaluation_report_set_error(hu_allocator_t *alloc, hu_evaluation_run_report_t *r,
                                          const char *summary);

/* Set or replace the (nullable) `model_version` string. Frees any prior
 * value. Returns HU_ERR_OUT_OF_MEMORY if alloc fails. */
hu_error_t hu_evaluation_report_set_model(hu_allocator_t *alloc, hu_evaluation_run_report_t *r,
                                          const char *model_version);

#endif /* HU_EVALUATION_INTERNAL_H */
