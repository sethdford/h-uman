#include "human/eval/eval_judge_external.h"
#include "human/eval/external_judge_fixture.h"

#include <stddef.h>

#if HU_IS_TEST
#define HU_EVAL_JUDGE_FIXTURE_PATH "tests/fixtures/external_judge_canned.json"
#endif

hu_error_t hu_eval_judge_create_gemini_nano(hu_allocator_t *alloc,
                                            hu_eval_judge_external_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    return hu_eval_judge_create_from_external_fixture(alloc, HU_EVAL_JUDGE_FIXTURE_PATH,
                                                      "gemini_nano", "gemini_nano", out);
#else
    (void)alloc;
    (void)out;
    return HU_ERR_NOT_SUPPORTED;
#endif
}
