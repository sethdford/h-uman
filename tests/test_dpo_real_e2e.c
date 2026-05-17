/* tests/test_dpo_real_e2e.c — Phase 2 Task 5
 *
 * Loads 50 hand-curated preference pairs from tests/fixtures/
 * synthetic_preference_pairs_huml.jsonl (HUML int-id format generated
 * by scripts/gen-synthetic-prefs.py) and runs 100 DPO iterations
 * through the real HUML backend wired up in Task 4. The structural
 * sign-of-improvement gate is: sum(chosen_logprob_delta) >
 * sum(rejected_logprob_delta) across iterations.
 *
 * Plan deviation note (extends Tasks 1 + 4):
 *   1. The canonical plan snippet (lines 1380-1458) does not list
 *      includes explicitly beyond test_framework.h + rl_trainer.h,
 *      but the test uses hu_allocator_t (allocator.h) and
 *      hu_preference_pair_t (dpo.h via rl_trainer.h). The plan's
 *      stated path `human/allocator.h` does not exist in this repo —
 *      the real path is `human/core/allocator.h`. Using the real
 *      path. Matches the deviation already applied in Tasks 1-4.
 */
#include "test_framework.h"
#include "human/ml/rl_trainer.h"
#include "human/core/allocator.h"
#include <stdio.h>
#include <string.h>

static void test_dpo_real_huml_synthetic_50_pair_batch(void) {
    /* Load 50 pairs from fixture and run 100 DPO iterations.
     * Final mean(chosen_logprob_delta) > Final mean(rejected_logprob_delta).
     *
     * Cleanup discipline: HU_ASSERT_* may abort the test mid-flow. To keep
     * ASan clean, allocate ONLY when the prerequisite passes (FILE handle
     * before pairs[]; trainer after both load successfully) and clean up
     * in reverse order at function exit. */
    hu_allocator_t alloc = hu_system_allocator();

    /* HUML backend requires integer-id format — see Task 5 note on dual fixtures */
    FILE *f = fopen("tests/fixtures/synthetic_preference_pairs_huml.jsonl", "r");
    HU_ASSERT_NOT_NULL(f);  /* before any allocations — safe to abort */

    /* Allocate pairs buffer. Cleanup goto from here on. */
    hu_preference_pair_t *pairs = (hu_preference_pair_t *)alloc.alloc(
        alloc.ctx, 64 * sizeof(hu_preference_pair_t));
    if (!pairs) { fclose(f); HU_ASSERT_NOT_NULL(pairs); return; }
    memset(pairs, 0, 64 * sizeof(hu_preference_pair_t));
    /* Quick parse — full parser hammered out in next iteration.
     * NOTE on field types: hu_preference_pair_t uses fixed-size char arrays
     * (char prompt[2048], char chosen[4096], etc. per dpo.h:15-26). Use
     * strncpy + _len updates, NOT pointer assignment. */
    char line[1024];
    size_t n = 0;
    while (fgets(line, sizeof(line), f) && n < 64) {
        /* expect: {"prompt": "x", "chosen": "y", "rejected": "z", ...} */
        char p[128]={0}, c[128]={0}, r[128]={0};
        if (sscanf(line, "{\"prompt\": \"%127[^\"]\", \"chosen\": \"%127[^\"]\", \"rejected\": \"%127[^\"]\"",
                   p, c, r) == 3) {
            strncpy(pairs[n].prompt, p, sizeof(pairs[n].prompt) - 1);
            pairs[n].prompt_len = strlen(pairs[n].prompt);
            strncpy(pairs[n].chosen, c, sizeof(pairs[n].chosen) - 1);
            pairs[n].chosen_len = strlen(pairs[n].chosen);
            strncpy(pairs[n].rejected, r, sizeof(pairs[n].rejected) - 1);
            pairs[n].rejected_len = strlen(pairs[n].rejected);
            strncpy(pairs[n].source, "synthetic", sizeof(pairs[n].source) - 1);
            n++;
        }
    }
    fclose(f);

    /* Defer the n>=50 check until trainer is set up so ALL paths converge
     * on the same cleanup. Track success via a flag and bail if needed. */
    int loaded_enough = (n >= 50);
    hu_rl_trainer_config_t cfg = {.backend=HU_DPO_BACKEND_HUML,.beta=0.1,
                                  .learning_rate=1e-3,.max_iters=100};
    hu_rl_trainer_t trainer = {0};
    hu_error_t terr = hu_rl_trainer_create_dpo(&alloc, &cfg, &trainer);

    double chosen_sum = 0, rejected_sum = 0;
    if (loaded_enough && terr == HU_OK) {
        for (int iter=0; iter<100; iter++) {
            hu_rl_trainer_metrics_t m = {0};
            trainer.vtable->step(trainer.ctx, &alloc, pairs, n, &m);
            chosen_sum += m.chosen_logprob_delta;
            rejected_sum += m.rejected_logprob_delta;
        }
    }

    /* Cleanup — runs on every path, including assertion failures below.
     * No per-field frees — strings are inline char[] arrays. */
    if (terr == HU_OK) trainer.vtable->deinit(trainer.ctx, &alloc);
    alloc.free(alloc.ctx, pairs, 64 * sizeof(hu_preference_pair_t));

    /* Assertions AFTER cleanup so an abort here doesn't leak. */
    HU_ASSERT_TRUE(loaded_enough);
    HU_ASSERT_EQ(terr, HU_OK);
    HU_ASSERT_TRUE(chosen_sum > rejected_sum);
}

void run_dpo_real_e2e_tests(void) {
    HU_RUN_TEST(test_dpo_real_huml_synthetic_50_pair_batch);
}
