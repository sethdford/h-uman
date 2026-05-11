#include "human/behavior/policy.h"
#include "human/behavior/support_strategy.h"
#include "test_framework.h"

#include <string.h>

static hu_behavior_decision_t supp_decision(hu_relational_act_t act) {
    hu_behavior_decision_t d = {0};
    d.act = act;
    d.confidence = 0.8f;
    return d;
}

static void supp_validate_maps_to_validate(void) {
    hu_behavior_decision_t d = supp_decision(HU_RELACT_VALIDATE);
    HU_ASSERT_EQ(hu_support_strategy_from_decision(&d), HU_SUPP_VALIDATE);
}

static void supp_reflect_maps_to_validate(void) {
    hu_behavior_decision_t d = supp_decision(HU_RELACT_REFLECT);
    HU_ASSERT_EQ(hu_support_strategy_from_decision(&d), HU_SUPP_VALIDATE);
}

static void supp_clarify_maps_to_question(void) {
    hu_behavior_decision_t d = supp_decision(HU_RELACT_ASK_CLARIFY);
    HU_ASSERT_EQ(hu_support_strategy_from_decision(&d), HU_SUPP_QUESTION);
}

static void supp_pushback_maps_to_reframe(void) {
    hu_behavior_decision_t d = supp_decision(HU_RELACT_PUSH_BACK);
    HU_ASSERT_EQ(hu_support_strategy_from_decision(&d), HU_SUPP_REFRAME);
}

static void supp_boundary_maps_to_boundary(void) {
    hu_behavior_decision_t d = supp_decision(HU_RELACT_BOUNDARY);
    HU_ASSERT_EQ(hu_support_strategy_from_decision(&d), HU_SUPP_BOUNDARY);
}

static void supp_refer_maps_to_refer(void) {
    hu_behavior_decision_t d = supp_decision(HU_RELACT_REFER_OUT);
    HU_ASSERT_EQ(hu_support_strategy_from_decision(&d), HU_SUPP_REFER);
}

static void supp_prompt_maps_to_plan(void) {
    hu_behavior_decision_t d = supp_decision(HU_RELACT_PROMPT);
    HU_ASSERT_EQ(hu_support_strategy_from_decision(&d), HU_SUPP_PLAN);
}

static void supp_answer_maps_to_none(void) {
    hu_behavior_decision_t d = supp_decision(HU_RELACT_ANSWER);
    HU_ASSERT_EQ(hu_support_strategy_from_decision(&d), HU_SUPP_NONE);
}

static void supp_null_returns_none(void) {
    HU_ASSERT_EQ(hu_support_strategy_from_decision(NULL), HU_SUPP_NONE);
}

static void supp_name_returns_known(void) {
    HU_ASSERT_STR_EQ(hu_support_strategy_name(HU_SUPP_VALIDATE), "validate");
    HU_ASSERT_STR_EQ(hu_support_strategy_name(HU_SUPP_REFER), "refer");
    HU_ASSERT_STR_EQ(hu_support_strategy_name(HU_SUPP_NONE), "none");
}

void run_behavior_support_strategy_tests(void);

void run_behavior_support_strategy_tests(void) {
    HU_TEST_SUITE("behavior_support_strategy");
    HU_RUN_TEST(supp_validate_maps_to_validate);
    HU_RUN_TEST(supp_reflect_maps_to_validate);
    HU_RUN_TEST(supp_clarify_maps_to_question);
    HU_RUN_TEST(supp_pushback_maps_to_reframe);
    HU_RUN_TEST(supp_boundary_maps_to_boundary);
    HU_RUN_TEST(supp_refer_maps_to_refer);
    HU_RUN_TEST(supp_prompt_maps_to_plan);
    HU_RUN_TEST(supp_answer_maps_to_none);
    HU_RUN_TEST(supp_null_returns_none);
    HU_RUN_TEST(supp_name_returns_known);
}
