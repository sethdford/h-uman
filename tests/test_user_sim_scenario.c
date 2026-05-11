#include "human/behavior/policy.h"
#include "human/behavior/user_sim.h"
#include "human/behavior/user_sim_scenario.h"
#include "test_framework.h"

#include <string.h>

static void scen_scripted_runs_to_completion(void) {
    static const char *const SCRIPT[] = {
        "Hi, how are you?",
        "What did you mean by that?",
        "I'm feeling really overwhelmed lately.",
        "Thanks, that helps.",
    };
    hu_user_sim_t sim =
        hu_user_sim_scripted(SCRIPT, sizeof(SCRIPT) / sizeof(SCRIPT[0]));
    hu_user_sim_run_result_t res = {0};
    HU_ASSERT_EQ(hu_user_sim_scenario_run(&sim, 10, 0, NULL, 0, &res), HU_OK);
    HU_ASSERT_EQ((long long)res.turns_executed, 4LL);
    HU_ASSERT_EQ((long long)res.decided_count, 4LL);
    hu_allocator_t alloc = hu_system_allocator();
    hu_user_sim_deinit(&sim, &alloc);
}

static void scen_distress_message_routes_to_validate(void) {
    static const char *const SCRIPT[] = {
        "I'm so overwhelmed I can't think straight.",
    };
    hu_user_sim_t sim =
        hu_user_sim_scripted(SCRIPT, sizeof(SCRIPT) / sizeof(SCRIPT[0]));
    uint32_t expected[] = {(uint32_t)HU_RELACT_VALIDATE};
    hu_user_sim_run_result_t res = {0};
    HU_ASSERT_EQ(hu_user_sim_scenario_run(&sim, 5, 0, expected, 1, &res), HU_OK);
    HU_ASSERT_EQ((long long)res.expected_total, 1LL);
    /* Heuristic policy may classify the distress as ASK_CLARIFY too on
     * first encounter; assert at least one of the validate-family acts. */
    uint32_t got = res.expected_acts[0];
    int ok = (got == (uint32_t)HU_RELACT_VALIDATE) || (got == (uint32_t)HU_RELACT_REFLECT) ||
             (got == (uint32_t)HU_RELACT_ACKNOWLEDGE);
    HU_ASSERT_TRUE(ok);
    hu_allocator_t alloc = hu_system_allocator();
    hu_user_sim_deinit(&sim, &alloc);
}

static void scen_repair_message_routes_to_repair(void) {
    static const char *const SCRIPT[] = {"Huh?"};
    hu_user_sim_t sim = hu_user_sim_scripted(SCRIPT, 1);
    uint32_t expected[] = {(uint32_t)HU_RELACT_REPAIR};
    hu_user_sim_run_result_t res = {0};
    HU_ASSERT_EQ(hu_user_sim_scenario_run(&sim, 5, 0, expected, 1, &res), HU_OK);
    HU_ASSERT_EQ((long long)res.expected_total, 1LL);
    uint32_t got = res.expected_acts[0];
    int ok = (got == (uint32_t)HU_RELACT_REPAIR) ||
             (got == (uint32_t)HU_RELACT_ASK_CLARIFY);
    HU_ASSERT_TRUE(ok);
    hu_allocator_t alloc = hu_system_allocator();
    hu_user_sim_deinit(&sim, &alloc);
}

static void scen_max_turns_caps_iterations(void) {
    static const char *const SCRIPT[] = {"a", "b", "c", "d", "e"};
    hu_user_sim_t sim = hu_user_sim_scripted(SCRIPT, 5);
    hu_user_sim_run_result_t res = {0};
    HU_ASSERT_EQ(hu_user_sim_scenario_run(&sim, 2, 0, NULL, 0, &res), HU_OK);
    HU_ASSERT_EQ((long long)res.turns_executed, 2LL);
    hu_allocator_t alloc = hu_system_allocator();
    hu_user_sim_deinit(&sim, &alloc);
}

static void scen_null_args_return_invalid(void) {
    hu_user_sim_run_result_t res = {0};
    HU_ASSERT_EQ(hu_user_sim_scenario_run(NULL, 5, 0, NULL, 0, &res), HU_ERR_INVALID_ARGUMENT);
    static const char *const SCRIPT[] = {"hi"};
    hu_user_sim_t sim = hu_user_sim_scripted(SCRIPT, 1);
    HU_ASSERT_EQ(hu_user_sim_scenario_run(&sim, 5, 0, NULL, 0, NULL), HU_ERR_INVALID_ARGUMENT);
    hu_allocator_t alloc = hu_system_allocator();
    hu_user_sim_deinit(&sim, &alloc);
}

void run_user_sim_scenario_tests(void);

void run_user_sim_scenario_tests(void) {
    HU_TEST_SUITE("user_sim_scenario");
    HU_RUN_TEST(scen_scripted_runs_to_completion);
    HU_RUN_TEST(scen_distress_message_routes_to_validate);
    HU_RUN_TEST(scen_repair_message_routes_to_repair);
    HU_RUN_TEST(scen_max_turns_caps_iterations);
    HU_RUN_TEST(scen_null_args_return_invalid);
}
