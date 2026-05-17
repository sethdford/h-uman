#include "human/eval/eval_judge_external.h"
#include "human/eval/external_judge_fixture.h"

#include <stddef.h>

#if HU_IS_TEST
#define HU_EVAL_JUDGE_FIXTURE_PATH "tests/fixtures/external_judge_canned.json"
#define HU_EVAL_JUDGE_HAVE_GEMINI_NANO_IMPL 1
#endif

hu_error_t hu_eval_judge_create_gemini_nano(hu_allocator_t *alloc,
                                            hu_eval_judge_external_t *out,
                                            const char **out_reason) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
#if HU_IS_TEST
    if (out_reason)
        *out_reason = NULL;
    return hu_eval_judge_create_from_external_fixture(alloc, HU_EVAL_JUDGE_FIXTURE_PATH,
                                                      "gemini_nano", "gemini_nano", out);
#else
    (void)alloc;
    (void)out;
    if (out_reason)
        *out_reason = "unavailable (not built: HU_EVAL_JUDGE_HAVE_GEMINI_NANO_IMPL=0)";
    return HU_ERR_NOT_SUPPORTED;
#endif
}
