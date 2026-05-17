#ifndef HU_EVAL_EXTERNAL_JUDGE_FIXTURE_H
#define HU_EVAL_EXTERNAL_JUDGE_FIXTURE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/eval/eval_judge_external.h"

#ifdef __cplusplus
extern "C" {
#endif

hu_error_t hu_eval_judge_create_from_external_fixture(
    hu_allocator_t *alloc, const char *fixture_path, const char *judge_key,
    const char *judge_name, hu_eval_judge_external_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HU_EVAL_EXTERNAL_JUDGE_FIXTURE_H */
