#include "human/core/allocator.h"
#include "human/eval/leaderboard.h"
#include "test_framework.h"

#include <stdlib.h>

static void test_mt_bench_runner_returns_canned_score_in_test_mode(void) {
    HU_SKIP_IF(getenv("HU_FORCE_REAL_LEADERBOARD") != NULL,
               "HU_FORCE_REAL_LEADERBOARD set");
    hu_allocator_t alloc = hu_system_allocator();
    hu_leaderboard_runner_t runner = {0};
    hu_leaderboard_config_t cfg = {
        .canned_path = "tests/fixtures/leaderboard_canned.json",
        .seed = 42,
    };
    HU_ASSERT_EQ(hu_leaderboard_create_mt_bench(&alloc, &cfg, &runner), HU_OK);
    const char *prompts[3] = {"explain recursion", "what is bm25", "summarize einstein"};
    const char *responses[3] = {"recursion is...", "bm25 is...", "einstein was..."};
    double scores[3] = {0};
    HU_ASSERT_EQ(runner.vtable->run(&runner, &alloc, prompts, responses, 3, scores), HU_OK);
    for (int i = 0; i < 3; i++)
        HU_ASSERT_TRUE(scores[i] >= 0.0 && scores[i] <= 10.0);
    runner.vtable->deinit(&runner, &alloc);
}

static void test_ifeval_runner_returns_canned_score_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_leaderboard_runner_t runner = {0};
    hu_leaderboard_config_t cfg = {.canned_path = "tests/fixtures/leaderboard_canned.json"};
    HU_ASSERT_EQ(hu_leaderboard_create_ifeval(&alloc, &cfg, &runner), HU_OK);
    const char *p[2] = {"write a haiku", "list five primes"};
    const char *r[2] = {"haiku here", "2 3 5 7 11"};
    double s[2] = {0};
    HU_ASSERT_EQ(runner.vtable->run(&runner, &alloc, p, r, 2, s), HU_OK);
    HU_ASSERT_TRUE(s[0] >= 0.0 && s[0] <= 1.0);
    runner.vtable->deinit(&runner, &alloc);
}

static void test_leaderboard_cache_miss_returns_unsupported_in_production_mode(void) {
    HU_SKIP_IF(getenv("HU_FORCE_REAL_LEADERBOARD") == NULL,
               "production-mode unavailable in test mode");
    hu_allocator_t alloc = hu_system_allocator();
    hu_leaderboard_runner_t runner = {0};
    hu_leaderboard_config_t cfg = {.canned_path = NULL};
    HU_ASSERT_EQ(hu_leaderboard_create_mt_bench(&alloc, &cfg, &runner), HU_OK);
    const char *p[1] = {"unknown prompt to force a cache miss"};
    const char *r[1] = {"unknown response"};
    double s[1] = {0};
    HU_ASSERT_EQ(runner.vtable->run(&runner, &alloc, p, r, 1, s), HU_ERR_NOT_SUPPORTED);
    runner.vtable->deinit(&runner, &alloc);
}

void run_leaderboard_tests(void) {
    HU_TEST_SUITE("leaderboard");
    HU_RUN_TEST(test_mt_bench_runner_returns_canned_score_in_test_mode);
    HU_RUN_TEST(test_ifeval_runner_returns_canned_score_in_test_mode);
    HU_RUN_TEST(test_leaderboard_cache_miss_returns_unsupported_in_production_mode);
}
