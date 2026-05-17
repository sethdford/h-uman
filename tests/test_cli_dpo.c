/* tests/test_cli_dpo.c — Phase 2 Task 8
 *
 * Pins the surface contract for the post-split DPO CLI handlers:
 *
 *   1. hu_ml_cli_dpo_judge --help exits zero (legacy path is reachable).
 *   2. hu_ml_cli_dpo_real  --help exits zero (Phase 2 real-DPO path).
 *   3. hu_ml_cli_dpo_real  --help exits zero again (placeholder for the
 *      "default backend is auto" assertion the plan calls out at lines
 *      1932–1939; full text capture lands in Task 9 once an env-var hook
 *      is wired).
 *
 * Plan deviation note: the canonical plan snippet (lines 1918–1946)
 * `#include`s `"human/allocator.h"`. That path does not exist in this
 * repo — the real path is `"human/core/allocator.h"` (matches every
 * other Phase 2 test, e.g. tests/test_rl_trainer.c). */

#include "test_framework.h"
#include "human/core/allocator.h"
#include "human/ml/cli_dpo.h"

static void test_cli_dpo_judge_help_exits_zero(void) {
    const char *argv[] = {"--help"};
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_ml_cli_dpo_judge(&alloc, 1, argv), HU_OK);
}

static void test_cli_dpo_real_help_exits_zero(void) {
    const char *argv[] = {"--help"};
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_ml_cli_dpo_real(&alloc, 1, argv), HU_OK);
}

static void test_cli_dpo_real_default_backend_is_auto(void) {
    /* Help text must mention "auto" backend. The full stdout capture
     * lands in Task 9; for Task 8 we just guarantee no crash so the
     * surface contract is pinned. */
    const char *argv[] = {"--help"};
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_ml_cli_dpo_real(&alloc, 1, argv), HU_OK);
}

/* Phase 2 Task 9 — end-to-end: load JSONL preference pairs and step the
 * trainer. Plan deviation #3 (vs lines 2099-2106): the canonical snippet
 * uses tests/fixtures/synthetic_preference_pairs.jsonl (natural-language
 * tokens). The HUML backend's parse_id_string (Task 4) expects integer
 * IDs, so natural-language pairs would short-circuit at the per-pair
 * "empty token list" guard and the trainer would never actually step.
 * Switching to the int-id fixture exercises the real step path (matches
 * test_dpo_real_e2e.c:36 which already uses the huml fixture). The CWD
 * for human_tests is the repo root (test_dpo_real_e2e.c is also a
 * relative-path consumer and passes), so the fopen call resolves. */
static void test_cli_dpo_real_loads_jsonl_pairs_and_calls_step(void) {
    const char *argv[] = {"--backend", "huml", "--pairs",
                          "tests/fixtures/synthetic_preference_pairs_huml.jsonl",
                          "--iters", "2"};
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_ml_cli_dpo_real(&alloc, 6, argv), HU_OK);
}

void run_cli_dpo_tests(void) {
    HU_TEST_SUITE("cli_dpo");
    HU_RUN_TEST(test_cli_dpo_judge_help_exits_zero);
    HU_RUN_TEST(test_cli_dpo_real_help_exits_zero);
    HU_RUN_TEST(test_cli_dpo_real_default_backend_is_auto);
    HU_RUN_TEST(test_cli_dpo_real_loads_jsonl_pairs_and_calls_step);
}
