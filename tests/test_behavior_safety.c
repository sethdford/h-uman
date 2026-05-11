#include "human/behavior/safety.h"
#include "test_framework.h"

#include <string.h>

static hu_behavior_safety_input_t bs_baseline(void) {
    hu_behavior_safety_input_t in = {0};
    /* Companion: nothing flagged. Vulnerability: NONE. Attachment: low. */
    in.companion.flagged = false;
    in.companion.farewell_unsafe = false;
    in.companion.total_risk = 0.1;
    in.vulnerability.level = HU_VULNERABILITY_NONE;
    in.vulnerability.crisis_keywords = false;
    in.attachment.sessions_per_day_avg = 1;
    in.attachment.late_night_sessions_30d = 0;
    in.attachment.exclusivity_signal_count = 0;
    in.attachment.parasocial_signal_count = 0;
    in.attachment.attachment_estimate = 0.1f;
    return in;
}

static void bs_crisis_keyword_triggers_referral(void) {
    hu_behavior_safety_input_t in = bs_baseline();
    in.vulnerability.level = HU_VULNERABILITY_CRISIS;
    in.vulnerability.crisis_keywords = true;
    hu_behavior_safety_assessment_t out = {0};
    HU_ASSERT_EQ(hu_behavior_safety_assess(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.primary_risk, HU_BRISK_ESCALATION_NEEDED);
    HU_ASSERT_TRUE(out.require_referral);
    HU_ASSERT_TRUE(out.require_boundary);
    HU_ASSERT_TRUE(out.pause_behavior_change);
}

static void bs_high_vulnerability_pauses_behavior_change(void) {
    hu_behavior_safety_input_t in = bs_baseline();
    in.vulnerability.level = HU_VULNERABILITY_HIGH;
    hu_behavior_safety_assessment_t out = {0};
    HU_ASSERT_EQ(hu_behavior_safety_assess(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.primary_risk, HU_BRISK_VULNERABLE_USER);
    HU_ASSERT_TRUE(out.pause_behavior_change);
}

static void bs_farewell_manipulation_resists(void) {
    hu_behavior_safety_input_t in = bs_baseline();
    in.companion.farewell_unsafe = true;
    hu_behavior_safety_assessment_t out = {0};
    HU_ASSERT_EQ(hu_behavior_safety_assess(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.primary_risk, HU_BRISK_GOODBYE_MANIPULATION);
    HU_ASSERT_TRUE(out.require_boundary);
}

static void bs_companion_flagged_high_risk_triggers_boundary(void) {
    hu_behavior_safety_input_t in = bs_baseline();
    in.companion.flagged = true;
    in.companion.total_risk = 0.85;
    hu_behavior_safety_assessment_t out = {0};
    HU_ASSERT_EQ(hu_behavior_safety_assess(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.primary_risk, HU_BRISK_ATTACHMENT_HIGH);
    HU_ASSERT_TRUE(out.require_boundary);
    HU_ASSERT_TRUE(out.encourage_human_relationship);
}

static void bs_exclusivity_repeats_widen_network(void) {
    hu_behavior_safety_input_t in = bs_baseline();
    in.attachment.exclusivity_signal_count = 4;
    hu_behavior_safety_assessment_t out = {0};
    HU_ASSERT_EQ(hu_behavior_safety_assess(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.primary_risk, HU_BRISK_EXCLUSIVITY);
    HU_ASSERT_TRUE(out.encourage_human_relationship);
    HU_ASSERT_TRUE(out.pause_behavior_change);
}

static void bs_dependency_pattern_via_session_volume(void) {
    hu_behavior_safety_input_t in = bs_baseline();
    in.attachment.sessions_per_day_avg = 12;
    hu_behavior_safety_assessment_t out = {0};
    HU_ASSERT_EQ(hu_behavior_safety_assess(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.primary_risk, HU_BRISK_DEPENDENCY_PATTERN);
    HU_ASSERT_TRUE(out.encourage_human_relationship);
}

static void bs_parasocial_signals_warn_displacement(void) {
    hu_behavior_safety_input_t in = bs_baseline();
    in.attachment.parasocial_signal_count = 6;
    hu_behavior_safety_assessment_t out = {0};
    HU_ASSERT_EQ(hu_behavior_safety_assess(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.primary_risk, HU_BRISK_HUMAN_DISPLACEMENT);
    HU_ASSERT_TRUE(out.encourage_human_relationship);
}

static void bs_clean_state_returns_no_risk(void) {
    hu_behavior_safety_input_t in = bs_baseline();
    hu_behavior_safety_assessment_t out = {0};
    HU_ASSERT_EQ(hu_behavior_safety_assess(&in, &out), HU_OK);
    HU_ASSERT_EQ(out.primary_risk, HU_BRISK_NONE);
    HU_ASSERT_FALSE(out.require_referral);
    HU_ASSERT_FALSE(out.require_boundary);
}

static void bs_null_input_returns_invalid(void) {
    hu_behavior_safety_assessment_t out = {0};
    HU_ASSERT_EQ(hu_behavior_safety_assess(NULL, &out), HU_ERR_INVALID_ARGUMENT);
    hu_behavior_safety_input_t in = bs_baseline();
    HU_ASSERT_EQ(hu_behavior_safety_assess(&in, NULL), HU_ERR_INVALID_ARGUMENT);
}

void run_behavior_safety_tests(void);

void run_behavior_safety_tests(void) {
    HU_TEST_SUITE("behavior_safety");
    HU_RUN_TEST(bs_crisis_keyword_triggers_referral);
    HU_RUN_TEST(bs_high_vulnerability_pauses_behavior_change);
    HU_RUN_TEST(bs_farewell_manipulation_resists);
    HU_RUN_TEST(bs_companion_flagged_high_risk_triggers_boundary);
    HU_RUN_TEST(bs_exclusivity_repeats_widen_network);
    HU_RUN_TEST(bs_dependency_pattern_via_session_volume);
    HU_RUN_TEST(bs_parasocial_signals_warn_displacement);
    HU_RUN_TEST(bs_clean_state_returns_no_risk);
    HU_RUN_TEST(bs_null_input_returns_invalid);
}
