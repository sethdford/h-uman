#include "human/behavior/affect.h"
#include "human/behavior/policy.h"
#include "human/behavior/safety.h"
#include "test_framework.h"

#include <string.h>

static hu_behavior_input_t bp_baseline(void) {
    hu_behavior_input_t in = {0};
    in.user_message = NULL;
    in.user_message_len = 0;
    in.last_user_act = HU_DACT_UNKNOWN;
    in.last_assistant_act = HU_DACT_UNKNOWN;
    in.user_asked_question = false;
    in.user_in_distress = false;
    in.awaiting_user = false;
    hu_affect_init(&in.affect);
    in.memory_has_relevant = false;
    in.memory_contradicts_user = false;
    in.trust_score = 0.7f;
    in.dependency_risk = 0.1f;
    /* safety zeroed = no overrides */
    in.channel_class = 0;
    in.relationship_stage = 0;
    return in;
}

static void bp_safety_referral_overrides_everything(void) {
    hu_behavior_input_t in = bp_baseline();
    in.user_asked_question = true;
    in.memory_has_relevant = true;
    in.safety.require_referral = true;
    in.safety.severity = 1.f;
    hu_behavior_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_decide(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.act, HU_RELACT_REFER_OUT);
    HU_ASSERT_EQ(out.evidence, HU_EVID_SAFETY);
}

static void bp_safety_boundary_overrides_question(void) {
    hu_behavior_input_t in = bp_baseline();
    in.user_asked_question = true;
    in.memory_has_relevant = true;
    in.safety.require_boundary = true;
    hu_behavior_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_decide(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.act, HU_RELACT_BOUNDARY);
}

static void bp_dependency_high_triggers_boundary(void) {
    hu_behavior_input_t in = bp_baseline();
    in.dependency_risk = 0.9f;
    hu_behavior_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_decide(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.act, HU_RELACT_BOUNDARY);
}

static void bp_repair_signal_picks_repair(void) {
    hu_behavior_input_t in = bp_baseline();
    in.user_message = "huh?";
    in.user_message_len = 4;
    hu_behavior_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_decide(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.act, HU_RELACT_REPAIR);
}

static void bp_distress_picks_validate(void) {
    hu_behavior_input_t in = bp_baseline();
    in.user_in_distress = true;
    in.user_asked_question = true;
    in.memory_has_relevant = true;
    hu_behavior_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_decide(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.act, HU_RELACT_VALIDATE);
}

static void bp_distress_via_affect_picks_validate(void) {
    hu_behavior_input_t in = bp_baseline();
    in.affect.valence = -0.7f;
    in.affect.arousal = 0.85f;
    in.affect.uncertainty = 0.1f;
    hu_behavior_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_decide(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.act, HU_RELACT_VALIDATE);
}

static void bp_memory_contradicts_picks_disclose_uncertainty(void) {
    hu_behavior_input_t in = bp_baseline();
    in.user_asked_question = true;
    in.memory_has_relevant = true;
    in.memory_contradicts_user = true;
    hu_behavior_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_decide(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.act, HU_RELACT_DISCLOSE_UNCERTAINTY);
}

static void bp_question_with_memory_picks_answer(void) {
    hu_behavior_input_t in = bp_baseline();
    in.user_asked_question = true;
    in.memory_has_relevant = true;
    hu_behavior_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_decide(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.act, HU_RELACT_ANSWER);
}

static void bp_question_without_memory_picks_clarify(void) {
    hu_behavior_input_t in = bp_baseline();
    in.user_asked_question = true;
    in.memory_has_relevant = false;
    hu_behavior_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_decide(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.act, HU_RELACT_ASK_CLARIFY);
}

static void bp_voice_awaiting_user_picks_backchannel(void) {
    hu_behavior_input_t in = bp_baseline();
    in.awaiting_user = true;
    in.channel_class = 1;
    hu_behavior_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_decide(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.act, HU_RELACT_BACKCHANNEL);
}

static void bp_text_awaiting_user_picks_wait(void) {
    hu_behavior_input_t in = bp_baseline();
    in.awaiting_user = true;
    in.channel_class = 0;
    hu_behavior_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_decide(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.act, HU_RELACT_WAIT);
}

static void bp_default_picks_answer(void) {
    hu_behavior_input_t in = bp_baseline();
    hu_behavior_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_decide(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.act, HU_RELACT_ANSWER);
}

static void bp_default_policy_vtable_is_named(void) {
    hu_behavior_policy_t p = hu_behavior_default_policy();
    HU_ASSERT_NOT_NULL(p.vtable);
    HU_ASSERT_NOT_NULL(p.vtable->name);
    HU_ASSERT_STR_EQ(p.vtable->name, "default-heuristic");
    /* deinit is a no-op on default; calling must not crash. */
    p.vtable->deinit(p.ctx);
}

static void bp_null_input_returns_invalid(void) {
    hu_behavior_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_decide(NULL, &out), HU_ERR_INVALID_ARGUMENT);
    hu_behavior_input_t in = bp_baseline();
    HU_ASSERT_EQ(hu_behavior_decide(&in, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void bp_relact_name_known(void) {
    HU_ASSERT_STR_EQ(hu_relational_act_name(HU_RELACT_VALIDATE), "validate");
    HU_ASSERT_STR_EQ(hu_relational_act_name(HU_RELACT_REPAIR), "repair");
}

static void bp_input_from_user_message_empty(void) {
    hu_behavior_input_t in;
    hu_behavior_input_from_user_message(&in, NULL, 0, 0);
    HU_ASSERT_EQ(in.last_user_act, HU_DACT_UNKNOWN);
    HU_ASSERT_FALSE(in.user_asked_question);
    HU_ASSERT_FALSE(in.user_in_distress);
    HU_ASSERT_EQ(in.channel_class, 0);
}

static void bp_input_from_user_message_question(void) {
    hu_behavior_input_t in;
    const char *msg = "What time is it?";
    hu_behavior_input_from_user_message(&in, msg, strlen(msg), 0);
    HU_ASSERT_TRUE(in.user_asked_question);
    HU_ASSERT_EQ(in.last_user_act, HU_DACT_QUESTION);
}

static void bp_input_from_user_message_distress(void) {
    hu_behavior_input_t in;
    const char *msg = "I am really scared about tomorrow";
    hu_behavior_input_from_user_message(&in, msg, strlen(msg), 0);
    HU_ASSERT_TRUE(in.user_in_distress);
}

void run_behavior_policy_tests(void) {
    HU_TEST_SUITE("behavior_policy");
    HU_RUN_TEST(bp_safety_referral_overrides_everything);
    HU_RUN_TEST(bp_safety_boundary_overrides_question);
    HU_RUN_TEST(bp_dependency_high_triggers_boundary);
    HU_RUN_TEST(bp_repair_signal_picks_repair);
    HU_RUN_TEST(bp_distress_picks_validate);
    HU_RUN_TEST(bp_distress_via_affect_picks_validate);
    HU_RUN_TEST(bp_memory_contradicts_picks_disclose_uncertainty);
    HU_RUN_TEST(bp_question_with_memory_picks_answer);
    HU_RUN_TEST(bp_question_without_memory_picks_clarify);
    HU_RUN_TEST(bp_voice_awaiting_user_picks_backchannel);
    HU_RUN_TEST(bp_text_awaiting_user_picks_wait);
    HU_RUN_TEST(bp_default_picks_answer);
    HU_RUN_TEST(bp_default_policy_vtable_is_named);
    HU_RUN_TEST(bp_null_input_returns_invalid);
    HU_RUN_TEST(bp_relact_name_known);
    HU_RUN_TEST(bp_input_from_user_message_empty);
    HU_RUN_TEST(bp_input_from_user_message_question);
    HU_RUN_TEST(bp_input_from_user_message_distress);
}
