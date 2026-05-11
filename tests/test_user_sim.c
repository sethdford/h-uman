#include "human/behavior/user_sim.h"
#include "human/core/allocator.h"
#include "test_framework.h"

#include <string.h>

static void user_sim_scripted_replays_lines(void) {
    static const char *const SCRIPT[] = {"hi", "what do you mean?", "ok thanks"};
    hu_allocator_t alloc = hu_system_allocator();
    hu_user_sim_t sim = hu_user_sim_scripted(SCRIPT, sizeof(SCRIPT) / sizeof(SCRIPT[0]));
    HU_ASSERT_NOT_NULL(sim.vtable);
    HU_ASSERT_NOT_NULL(sim.ctx);

    hu_user_sim_turn_ctx_t t = {0};
    char *l1 = NULL;
    size_t l1len = 0;
    HU_ASSERT_EQ(hu_user_sim_next(&sim, &t, &l1, &l1len), HU_OK);
    HU_ASSERT_NOT_NULL(l1);
    HU_ASSERT_STR_EQ(l1, "hi");
    free(l1);

    char *l2 = NULL;
    size_t l2len = 0;
    t.turn_index = 1;
    HU_ASSERT_EQ(hu_user_sim_next(&sim, &t, &l2, &l2len), HU_OK);
    HU_ASSERT_NOT_NULL(l2);
    HU_ASSERT_STR_EQ(l2, "what do you mean?");
    free(l2);

    char *l3 = NULL;
    size_t l3len = 0;
    HU_ASSERT_EQ(hu_user_sim_next(&sim, &t, &l3, &l3len), HU_OK);
    HU_ASSERT_NOT_NULL(l3);
    HU_ASSERT_STR_EQ(l3, "ok thanks");
    free(l3);

    char *l4 = NULL;
    size_t l4len = 0;
    HU_ASSERT_EQ(hu_user_sim_next(&sim, &t, &l4, &l4len), HU_OK);
    HU_ASSERT_NULL(l4);
    HU_ASSERT_EQ(l4len, 0u);

    hu_user_sim_deinit(&sim, &alloc);
}

void run_user_sim_tests(void);

void run_user_sim_tests(void) {
    HU_TEST_SUITE("user_sim");
    HU_RUN_TEST(user_sim_scripted_replays_lines);
}
