#include "human/behavior/trust.h"
#include "human/behavior/trust_prompt.h"
#include "human/core/allocator.h"
#include "test_framework.h"

#include <string.h>

static hu_trust_input_t trust_baseline(void) {
    hu_trust_input_t in = {0};
    in.memory_contradicts_user = false;
    in.tool_output_contradicts_user = false;
    in.source_is_tool_output = false;
    in.source_is_user_assertion = false;
    in.user_reasserted_after_pushback = false;
    in.user_pressure_count = 0;
    in.user_invoked_authority = false;
    in.user_emotional_pressure = false;
    in.trust_score = 0.7f;
    in.answer_is_speculative = false;
    return in;
}

static void trust_tool_disagree_no_pressure_pushes_back(void) {
    hu_trust_input_t in = trust_baseline();
    in.tool_output_contradicts_user = true;
    in.source_is_tool_output = true;
    hu_trust_decision_t out = {0};
    HU_ASSERT_EQ(hu_trust_calibrate(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.action, HU_TRUST_PUSH_BACK);
    HU_ASSERT_TRUE(out.firmness >= 0.5f);
}

static void trust_tool_disagree_with_pressure_refuses_to_agree(void) {
    hu_trust_input_t in = trust_baseline();
    in.tool_output_contradicts_user = true;
    in.user_reasserted_after_pushback = true;
    in.user_pressure_count = 3;
    hu_trust_decision_t out = {0};
    HU_ASSERT_EQ(hu_trust_calibrate(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.action, HU_TRUST_REFUSE_TO_AGREE);
    /* Firmness should be high under pressure. */
    HU_ASSERT_TRUE(out.firmness >= 0.7f);
}

static void trust_memory_disagree_first_time_pushes_back(void) {
    hu_trust_input_t in = trust_baseline();
    in.memory_contradicts_user = true;
    hu_trust_decision_t out = {0};
    HU_ASSERT_EQ(hu_trust_calibrate(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.action, HU_TRUST_PUSH_BACK);
}

static void trust_memory_disagree_with_repeated_pressure_refuses(void) {
    hu_trust_input_t in = trust_baseline();
    in.memory_contradicts_user = true;
    in.user_pressure_count = 3;
    hu_trust_decision_t out = {0};
    HU_ASSERT_EQ(hu_trust_calibrate(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.action, HU_TRUST_REFUSE_TO_AGREE);
}

static void trust_pressure_increases_firmness_monotonically(void) {
    hu_trust_input_t base = trust_baseline();
    base.tool_output_contradicts_user = true;
    base.user_reasserted_after_pushback = true;

    hu_trust_decision_t a = {0};
    base.user_pressure_count = 1;
    HU_ASSERT_EQ(hu_trust_calibrate(&base, &a), HU_OK);

    hu_trust_decision_t b = {0};
    base.user_pressure_count = 5;
    HU_ASSERT_EQ(hu_trust_calibrate(&base, &b), HU_OK);

    HU_ASSERT_TRUE(b.firmness >= a.firmness);
}

static void trust_authority_invocation_surfaces_uncertainty(void) {
    hu_trust_input_t in = trust_baseline();
    in.user_invoked_authority = true;
    hu_trust_decision_t out = {0};
    HU_ASSERT_EQ(hu_trust_calibrate(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.action, HU_TRUST_DISCLOSE_UNCERTAINTY);
}

static void trust_emotional_pressure_does_not_capitulate(void) {
    hu_trust_input_t in = trust_baseline();
    in.user_emotional_pressure = true;
    hu_trust_decision_t out = {0};
    HU_ASSERT_EQ(hu_trust_calibrate(&in, &out), HU_OK);
    /* Should not be ANSWER (sycophancy); should be uncertainty disclosure or
     * stronger. */
    HU_ASSERT_NEQ(out.action, HU_TRUST_ANSWER);
}

static void trust_speculative_answer_discloses_uncertainty(void) {
    hu_trust_input_t in = trust_baseline();
    in.answer_is_speculative = true;
    hu_trust_decision_t out = {0};
    HU_ASSERT_EQ(hu_trust_calibrate(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.action, HU_TRUST_DISCLOSE_UNCERTAINTY);
}

static void trust_high_score_cites_memory(void) {
    hu_trust_input_t in = trust_baseline();
    in.trust_score = 0.9f;
    hu_trust_decision_t out = {0};
    HU_ASSERT_EQ(hu_trust_calibrate(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.action, HU_TRUST_CITE_MEMORY);
}

static void trust_default_answers(void) {
    hu_trust_input_t in = trust_baseline();
    hu_trust_decision_t out = {0};
    HU_ASSERT_EQ(hu_trust_calibrate(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.action, HU_TRUST_ANSWER);
}

static void trust_null_args_return_invalid(void) {
    hu_trust_decision_t out = {0};
    hu_trust_input_t in = trust_baseline();
    HU_ASSERT_EQ(hu_trust_calibrate(NULL, &out), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_trust_calibrate(&in, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void trust_action_name_known(void) {
    HU_ASSERT_STR_EQ(hu_trust_action_name(HU_TRUST_PUSH_BACK), "push_back");
    HU_ASSERT_STR_EQ(hu_trust_action_name(HU_TRUST_REFUSE_TO_AGREE), "refuse_to_agree");
}

static void trust_directive_emits_for_refuse(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_trust_decision_t d = {0};
    d.action = HU_TRUST_REFUSE_TO_AGREE;
    d.firmness = 0.9f;
    d.confidence = 0.95f;
    snprintf(d.rationale, sizeof(d.rationale), "%s", "test rationale");
    HU_ASSERT_TRUE(hu_trust_directive_is_worth_emitting(&d) != 0);
    char *out = NULL;
    size_t olen = 0;
    HU_ASSERT_EQ(hu_trust_build_directive(&alloc, &d, &out, &olen), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(olen > 0);
    HU_ASSERT_NOT_NULL(strstr(out, "[Trust: refuse_to_agree"));
    HU_ASSERT_NOT_NULL(strstr(out, "test rationale"));
    alloc.free(alloc.ctx, out, olen + 1);
}

static void trust_directive_skips_plain_answer(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_trust_decision_t d = {0};
    d.action = HU_TRUST_ANSWER;
    HU_ASSERT_EQ(hu_trust_directive_is_worth_emitting(&d), 0);
    char *out = NULL;
    size_t olen = 0;
    HU_ASSERT_EQ(hu_trust_build_directive(&alloc, &d, &out, &olen), HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(olen, 0u);
}

void run_behavior_trust_tests(void);

void run_behavior_trust_tests(void) {
    HU_TEST_SUITE("behavior_trust");
    HU_RUN_TEST(trust_tool_disagree_no_pressure_pushes_back);
    HU_RUN_TEST(trust_tool_disagree_with_pressure_refuses_to_agree);
    HU_RUN_TEST(trust_memory_disagree_first_time_pushes_back);
    HU_RUN_TEST(trust_memory_disagree_with_repeated_pressure_refuses);
    HU_RUN_TEST(trust_pressure_increases_firmness_monotonically);
    HU_RUN_TEST(trust_authority_invocation_surfaces_uncertainty);
    HU_RUN_TEST(trust_emotional_pressure_does_not_capitulate);
    HU_RUN_TEST(trust_speculative_answer_discloses_uncertainty);
    HU_RUN_TEST(trust_high_score_cites_memory);
    HU_RUN_TEST(trust_default_answers);
    HU_RUN_TEST(trust_null_args_return_invalid);
    HU_RUN_TEST(trust_action_name_known);
    HU_RUN_TEST(trust_directive_emits_for_refuse);
    HU_RUN_TEST(trust_directive_skips_plain_answer);
}
