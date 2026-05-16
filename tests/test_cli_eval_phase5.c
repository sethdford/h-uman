#include "test_framework.h"
#include "human/eval/cli_eval.h"
#include "human/core/allocator.h"
#include <string.h>

static void test_human_eval_competitive_help_lists_phase5_subcommands(void) {
    HU_SKIP_IF(!hu_build_has_competitive_eval(),
               "HU_ENABLE_RL_FULL=OFF in this build");
    hu_allocator_t alloc = hu_system_allocator();
    char argv0[] = "human";
    char argv1[] = "competitive";
    char argv2[] = "--help";
    char *argv[] = {argv0, argv1, argv2};
    HU_ASSERT_EQ(hu_eval_cli_competitive(&alloc, 3, argv), HU_OK);
}

void run_cli_eval_phase5_tests(void) {
    HU_TEST_SUITE("cli-eval-phase5");
    HU_RUN_TEST(test_human_eval_competitive_help_lists_phase5_subcommands);
}
