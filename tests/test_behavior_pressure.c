#include "human/behavior/pressure.h"
#include "human/behavior/trust.h"
#include "test_framework.h"

#include <stdint.h>
#include <string.h>

static hu_pressure_signals_t pr_detect(const char *msg) {
    hu_pressure_signals_t out;
    HU_ASSERT_EQ(hu_pressure_detect(msg, msg ? strlen(msg) : 0, &out), HU_OK);
    return out;
}

static void pressure_neutral_message_has_no_signals(void) {
    hu_pressure_signals_t s = pr_detect("Hi, how are you?");
    HU_ASSERT_FALSE(s.invoked_authority);
    HU_ASSERT_FALSE(s.emotional_pressure);
    HU_ASSERT_FALSE(s.reasserted_in_message);
    HU_ASSERT_EQ((long long)s.exclamation_count, 0LL);
}

static void pressure_authority_invocation_detected(void) {
    HU_ASSERT_TRUE(pr_detect("Everyone knows that's wrong.").invoked_authority);
    HU_ASSERT_TRUE(pr_detect("As I told you yesterday, the meeting is at 3.").invoked_authority);
    HU_ASSERT_TRUE(pr_detect("You literally said this yesterday.").invoked_authority);
    HU_ASSERT_TRUE(pr_detect("You should know this by now.").invoked_authority);
}

static void pressure_authority_case_insensitive(void) {
    HU_ASSERT_TRUE(pr_detect("EVERYONE KNOWS THAT").invoked_authority);
    HU_ASSERT_TRUE(pr_detect("EvErYoNe KnOwS").invoked_authority);
}

static void pressure_emotional_words_detected(void) {
    HU_ASSERT_TRUE(pr_detect("That's stupid.").emotional_pressure);
    HU_ASSERT_TRUE(pr_detect("Don't be ridiculous.").emotional_pressure);
    HU_ASSERT_TRUE(pr_detect("You're wrong.").emotional_pressure);
}

static void pressure_exclamations_threshold(void) {
    /* 1 exclamation alone does not trigger emotional pressure. */
    hu_pressure_signals_t one = pr_detect("Wait, what!");
    HU_ASSERT_EQ((long long)one.exclamation_count, 1LL);
    HU_ASSERT_FALSE(one.emotional_pressure);
    /* 2+ exclamations do. */
    hu_pressure_signals_t two = pr_detect("Wait, what!!");
    HU_ASSERT_TRUE(two.exclamation_count >= 2);
    HU_ASSERT_TRUE(two.emotional_pressure);
}

static void pressure_caps_shouting_triggers(void) {
    hu_pressure_signals_t s = pr_detect("Stop SHOUTING at me");
    HU_ASSERT_TRUE(s.caps_run_max >= 4);
    HU_ASSERT_TRUE(s.emotional_pressure);
}

static void pressure_hedging_dampens_emotional_read(void) {
    /* Even with two exclamations, hedging language dampens to no pressure. */
    hu_pressure_signals_t s = pr_detect("I think you might be wrong, maybe!!");
    HU_ASSERT_TRUE(s.exclamation_count >= 2);
    HU_ASSERT_TRUE(s.hedging_phrases >= 1);
    HU_ASSERT_FALSE(s.emotional_pressure);
}

static void pressure_reassertion_language_detected(void) {
    HU_ASSERT_TRUE(pr_detect("I told you, the deadline is Friday.").reasserted_in_message);
    HU_ASSERT_TRUE(pr_detect("Again, the deadline is Friday.").reasserted_in_message);
    HU_ASSERT_TRUE(pr_detect("Like I said, that's not what I meant.").reasserted_in_message);
}

static void pressure_apply_to_trust_input_increments_pressure(void) {
    hu_pressure_signals_t s = pr_detect("EVERYONE KNOWS this is wrong!!");
    HU_ASSERT_TRUE(s.invoked_authority);
    HU_ASSERT_TRUE(s.emotional_pressure);
    hu_trust_input_t tin;
    memset(&tin, 0, sizeof(tin));
    hu_pressure_apply_to_trust_input(&s, &tin);
    HU_ASSERT_TRUE(tin.user_invoked_authority);
    HU_ASSERT_TRUE(tin.user_emotional_pressure);
}

static void pressure_apply_reassertion_bumps_count(void) {
    hu_pressure_signals_t s = pr_detect("I told you, the meeting is Friday.");
    HU_ASSERT_TRUE(s.reasserted_in_message);
    hu_trust_input_t tin;
    memset(&tin, 0, sizeof(tin));
    hu_pressure_apply_to_trust_input(&s, &tin);
    HU_ASSERT_EQ((long long)tin.user_pressure_count, 1LL);

    /* Second application bumps further. */
    hu_pressure_apply_to_trust_input(&s, &tin);
    HU_ASSERT_EQ((long long)tin.user_pressure_count, 2LL);
}

static void pressure_apply_does_not_lower_existing_signals(void) {
    hu_pressure_signals_t s = pr_detect("Hi"); /* no signals */
    hu_trust_input_t tin;
    memset(&tin, 0, sizeof(tin));
    tin.user_invoked_authority = true;
    tin.user_emotional_pressure = true;
    tin.user_pressure_count = 5;
    hu_pressure_apply_to_trust_input(&s, &tin);
    HU_ASSERT_TRUE(tin.user_invoked_authority);
    HU_ASSERT_TRUE(tin.user_emotional_pressure);
    HU_ASSERT_EQ((long long)tin.user_pressure_count, 5LL);
}

static void pressure_null_args_safe(void) {
    HU_ASSERT_EQ(hu_pressure_detect(NULL, 0, NULL), HU_ERR_INVALID_ARGUMENT);
    hu_pressure_signals_t out;
    HU_ASSERT_EQ(hu_pressure_detect(NULL, 0, &out), HU_OK);
    HU_ASSERT_FALSE(out.invoked_authority);
    HU_ASSERT_FALSE(out.emotional_pressure);
    /* hu_pressure_apply_to_trust_input with NULL is a no-op. */
    hu_pressure_apply_to_trust_input(NULL, NULL);
    hu_pressure_apply_to_trust_input(&out, NULL);
}

void run_behavior_pressure_tests(void);

void run_behavior_pressure_tests(void) {
    HU_TEST_SUITE("behavior_pressure");
    HU_RUN_TEST(pressure_neutral_message_has_no_signals);
    HU_RUN_TEST(pressure_authority_invocation_detected);
    HU_RUN_TEST(pressure_authority_case_insensitive);
    HU_RUN_TEST(pressure_emotional_words_detected);
    HU_RUN_TEST(pressure_exclamations_threshold);
    HU_RUN_TEST(pressure_caps_shouting_triggers);
    HU_RUN_TEST(pressure_hedging_dampens_emotional_read);
    HU_RUN_TEST(pressure_reassertion_language_detected);
    HU_RUN_TEST(pressure_apply_to_trust_input_increments_pressure);
    HU_RUN_TEST(pressure_apply_reassertion_bumps_count);
    HU_RUN_TEST(pressure_apply_does_not_lower_existing_signals);
    HU_RUN_TEST(pressure_null_args_safe);
}
