#include "human/behavior/change.h"
#include "test_framework.h"

#include <string.h>

static hu_behavior_change_input_t bc_baseline(void) {
    hu_behavior_change_input_t in = {0};
    in.fogg.motivation = 0.7f;
    in.fogg.ability = 0.7f;
    in.fogg.prompt_readiness = 0.7f;
    in.user_invited_help = true;
    in.user_explicit_consent = true;
    in.autonomy_risk = 0.1f;
    in.burden = 0.2f;
    in.distress_escalation = 0.0f;
    in.hour = 14;
    in.late_night_opt_in = false;
    return in;
}

static void bc_distress_escalation_pauses(void) {
    hu_behavior_change_input_t in = bc_baseline();
    in.distress_escalation = 0.9f;
    hu_behavior_change_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_change_select(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.technique, HU_BCT_NONE);
    HU_ASSERT_TRUE(out.defer);
}

static void bc_late_night_without_optin_defers(void) {
    hu_behavior_change_input_t in = bc_baseline();
    in.hour = 2;
    in.late_night_opt_in = false;
    hu_behavior_change_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_change_select(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.technique, HU_BCT_NONE);
    HU_ASSERT_TRUE(out.defer);
}

static void bc_late_night_with_optin_proceeds(void) {
    hu_behavior_change_input_t in = bc_baseline();
    in.hour = 2;
    in.late_night_opt_in = true;
    hu_behavior_change_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_change_select(&in, &out), HU_OK);
    HU_ASSERT_NEQ(out.technique, HU_BCT_NONE);
}

static void bc_no_invitation_high_autonomy_risk_asks_first(void) {
    hu_behavior_change_input_t in = bc_baseline();
    in.user_invited_help = false;
    in.user_explicit_consent = false;
    in.autonomy_risk = 0.7f;
    hu_behavior_change_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_change_select(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.technique, HU_BCT_NONE);
    HU_ASSERT_TRUE(out.ask_permission_first);
}

static void bc_persuasion_blocked_when_autonomy_risk_high(void) {
    hu_behavior_change_input_t in = bc_baseline();
    in.user_explicit_consent = true;
    in.user_invited_help = true;
    in.autonomy_risk = 0.5f;
    in.fogg.motivation = 0.2f;
    hu_behavior_change_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_change_select(&in, &out), HU_OK);
    HU_ASSERT_NEQ(out.technique, HU_BCT_VERBAL_PERSUASION);
}

static void bc_low_motivation_with_consent_uses_reframing_or_persuasion(void) {
    hu_behavior_change_input_t in = bc_baseline();
    in.fogg.motivation = 0.2f;
    in.fogg.ability = 0.6f;
    in.autonomy_risk = 0.1f;
    hu_behavior_change_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_change_select(&in, &out), HU_OK);
    HU_ASSERT_TRUE(out.technique == HU_BCT_REFRAMING ||
                   out.technique == HU_BCT_VERBAL_PERSUASION);
}

static void bc_low_ability_uses_reduce_friction(void) {
    hu_behavior_change_input_t in = bc_baseline();
    in.fogg.motivation = 0.7f;
    in.fogg.ability = 0.2f;
    hu_behavior_change_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_change_select(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.technique, HU_BCT_REDUCE_FRICTION);
}

static void bc_high_burden_defers(void) {
    hu_behavior_change_input_t in = bc_baseline();
    in.burden = 0.85f;
    hu_behavior_change_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_change_select(&in, &out), HU_OK);
    HU_ASSERT_TRUE(out.defer);
}

static void bc_aligned_state_picks_goal_setting(void) {
    hu_behavior_change_input_t in = bc_baseline();
    hu_behavior_change_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_change_select(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.technique, HU_BCT_GOAL_SETTING);
    HU_ASSERT_TRUE(out.act_now);
}

static void bc_null_input_returns_invalid(void) {
    hu_behavior_change_decision_t out = {0};
    HU_ASSERT_EQ(hu_behavior_change_select(NULL, &out), HU_ERR_INVALID_ARGUMENT);
    hu_behavior_change_input_t in = bc_baseline();
    HU_ASSERT_EQ(hu_behavior_change_select(&in, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void bc_name_returns_known(void) {
    HU_ASSERT_STR_EQ(hu_bct_name(HU_BCT_REDUCE_FRICTION), "reduce_friction");
    HU_ASSERT_STR_EQ(hu_bct_name(HU_BCT_NONE), "none");
}

void run_behavior_change_tests(void);

void run_behavior_change_tests(void) {
    HU_TEST_SUITE("behavior_change");
    HU_RUN_TEST(bc_distress_escalation_pauses);
    HU_RUN_TEST(bc_late_night_without_optin_defers);
    HU_RUN_TEST(bc_late_night_with_optin_proceeds);
    HU_RUN_TEST(bc_no_invitation_high_autonomy_risk_asks_first);
    HU_RUN_TEST(bc_persuasion_blocked_when_autonomy_risk_high);
    HU_RUN_TEST(bc_low_motivation_with_consent_uses_reframing_or_persuasion);
    HU_RUN_TEST(bc_low_ability_uses_reduce_friction);
    HU_RUN_TEST(bc_high_burden_defers);
    HU_RUN_TEST(bc_aligned_state_picks_goal_setting);
    HU_RUN_TEST(bc_null_input_returns_invalid);
    HU_RUN_TEST(bc_name_returns_known);
}
