#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona/relationship.h"
#include "test_framework.h"
#include <string.h>

static void relationship_new_stage(void) {
    hu_relationship_state_t state = {0};
    HU_ASSERT_EQ(state.stage, HU_REL_NEW);
    hu_relationship_new_session(&state);
    HU_ASSERT_EQ(state.stage, HU_REL_NEW);
    HU_ASSERT_EQ(state.session_count, 1u);
}

static void relationship_familiar_after_5(void) {
    hu_relationship_state_t state = {0};
    for (int i = 0; i < 5; i++)
        hu_relationship_new_session(&state);
    HU_ASSERT_EQ(state.stage, HU_REL_FAMILIAR);
    HU_ASSERT_EQ(state.session_count, 5u);
}

static void relationship_trusted_after_20(void) {
    hu_relationship_state_t state = {0};
    for (int i = 0; i < 20; i++)
        hu_relationship_new_session(&state);
    HU_ASSERT_EQ(state.stage, HU_REL_TRUSTED);
    HU_ASSERT_EQ(state.session_count, 20u);
}

static void relationship_deep_after_50(void) {
    hu_relationship_state_t state = {0};
    for (int i = 0; i < 50; i++)
        hu_relationship_new_session(&state);
    HU_ASSERT_EQ(state.stage, HU_REL_DEEP);
    HU_ASSERT_EQ(state.session_count, 50u);
}

static void relationship_update_increments_turns_not_sessions(void) {
    hu_relationship_state_t state = {0};
    hu_relationship_new_session(&state);
    HU_ASSERT_EQ(state.session_count, 1u);
    HU_ASSERT_EQ(state.total_turns, 0u);
    hu_relationship_update(&state, 1);
    HU_ASSERT_EQ(state.session_count, 1u);
    HU_ASSERT_EQ(state.total_turns, 1u);
    hu_relationship_update(&state, 3);
    HU_ASSERT_EQ(state.session_count, 1u);
    HU_ASSERT_EQ(state.total_turns, 4u);
}

static void relationship_build_prompt_contains_stage(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_relationship_state_t state = {.stage = HU_REL_FAMILIAR, .session_count = 10, .total_turns = 25};
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_relationship_build_prompt(&alloc, &state, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Relationship Context") != NULL);
    HU_ASSERT_TRUE(strstr(out, "familiar") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Sessions: 10") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void relationship_new_session_null_safe(void) {
    hu_relationship_new_session(NULL);
}

static void relationship_build_prompt_null_state_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_relationship_build_prompt(&alloc, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(out);
}

void run_relationship_tests(void) {
    HU_TEST_SUITE("relationship");
    HU_RUN_TEST(relationship_new_stage);
    HU_RUN_TEST(relationship_familiar_after_5);
    HU_RUN_TEST(relationship_trusted_after_20);
    HU_RUN_TEST(relationship_deep_after_50);
    HU_RUN_TEST(relationship_update_increments_turns_not_sessions);
    HU_RUN_TEST(relationship_build_prompt_contains_stage);
    HU_RUN_TEST(relationship_new_session_null_safe);
    HU_RUN_TEST(relationship_build_prompt_null_state_fails);
}
