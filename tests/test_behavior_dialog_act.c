#include "human/behavior/dialog_act.h"
#include "test_framework.h"

#include <string.h>

static void dact_classify_question_marks_as_question(void) {
    const char *q = "What time should we meet?";
    HU_ASSERT_EQ(hu_dialog_act_classify(q, strlen(q)), HU_DACT_QUESTION);
}

static void dact_classify_clarify_question_when_phrase_present(void) {
    const char *q = "Wait, what do you mean by that?";
    HU_ASSERT_EQ(hu_dialog_act_classify(q, strlen(q)), HU_DACT_REPAIR_INITIATE);
}

static void dact_classify_short_huh_as_repair(void) {
    const char *q = "huh?";
    HU_ASSERT_EQ(hu_dialog_act_classify(q, strlen(q)), HU_DACT_REPAIR_INITIATE);
}

static void dact_classify_clarify_returns_clarify_question(void) {
    /* "Can you clarify the deadline?" is a substantive clarify-question
     * (asking about content), distinct from other-initiated repair like
     * "what do you mean?". */
    const char *q = "Can you clarify the deadline for me?";
    HU_ASSERT_EQ(hu_dialog_act_classify(q, strlen(q)), HU_DACT_CLARIFY_QUESTION);
}

static void dact_classify_yeah_as_backchannel(void) {
    const char *t = "yeah";
    HU_ASSERT_EQ(hu_dialog_act_classify(t, strlen(t)), HU_DACT_BACKCHANNEL);
}

static void dact_classify_disagreement_with_actually(void) {
    const char *t = "Actually, I think the deadline is next week.";
    HU_ASSERT_EQ(hu_dialog_act_classify(t, strlen(t)), HU_DACT_DISAGREEMENT);
}

static void dact_classify_boundary_phrases(void) {
    const char *t = "I'd rather not talk about that today.";
    HU_ASSERT_EQ(hu_dialog_act_classify(t, strlen(t)), HU_DACT_BOUNDARY);
}

static void dact_classify_abstention_phrases(void) {
    const char *t = "I don't know yet.";
    HU_ASSERT_EQ(hu_dialog_act_classify(t, strlen(t)), HU_DACT_ABSTENTION);
}

static void dact_classify_greeting_short(void) {
    const char *t = "hey there";
    HU_ASSERT_EQ(hu_dialog_act_classify(t, strlen(t)), HU_DACT_GREETING);
}

static void dact_classify_farewell_short(void) {
    const char *t = "bye";
    HU_ASSERT_EQ(hu_dialog_act_classify(t, strlen(t)), HU_DACT_FAREWELL);
}

static void dact_repair_filter_rejects_long_narrative_with_what(void) {
    const char *t = "I told her what happened on Tuesday and she just listened.";
    HU_ASSERT_FALSE(hu_dialog_act_is_repair_initiation(t, strlen(t)));
}

static void dact_repair_detects_didnt_catch(void) {
    const char *t = "Sorry, I didn't catch that?";
    HU_ASSERT_TRUE(hu_dialog_act_is_repair_initiation(t, strlen(t)));
}

static void dact_repair_returns_false_for_null_or_empty(void) {
    HU_ASSERT_FALSE(hu_dialog_act_is_repair_initiation(NULL, 0));
    HU_ASSERT_FALSE(hu_dialog_act_is_repair_initiation("", 0));
}

static void dact_classify_returns_unknown_for_empty(void) {
    HU_ASSERT_EQ(hu_dialog_act_classify(NULL, 0), HU_DACT_UNKNOWN);
    HU_ASSERT_EQ(hu_dialog_act_classify("", 0), HU_DACT_UNKNOWN);
}

static void dact_name_returns_known_strings(void) {
    HU_ASSERT_STR_EQ(hu_dialog_act_name(HU_DACT_QUESTION), "question");
    HU_ASSERT_STR_EQ(hu_dialog_act_name(HU_DACT_REPAIR_INITIATE), "repair_initiate");
}

void run_behavior_dialog_act_tests(void);

void run_behavior_dialog_act_tests(void) {
    HU_TEST_SUITE("behavior_dialog_act");
    HU_RUN_TEST(dact_classify_question_marks_as_question);
    HU_RUN_TEST(dact_classify_clarify_question_when_phrase_present);
    HU_RUN_TEST(dact_classify_short_huh_as_repair);
    HU_RUN_TEST(dact_classify_clarify_returns_clarify_question);
    HU_RUN_TEST(dact_classify_yeah_as_backchannel);
    HU_RUN_TEST(dact_classify_disagreement_with_actually);
    HU_RUN_TEST(dact_classify_boundary_phrases);
    HU_RUN_TEST(dact_classify_abstention_phrases);
    HU_RUN_TEST(dact_classify_greeting_short);
    HU_RUN_TEST(dact_classify_farewell_short);
    HU_RUN_TEST(dact_repair_filter_rejects_long_narrative_with_what);
    HU_RUN_TEST(dact_repair_detects_didnt_catch);
    HU_RUN_TEST(dact_repair_returns_false_for_null_or_empty);
    HU_RUN_TEST(dact_classify_returns_unknown_for_empty);
    HU_RUN_TEST(dact_name_returns_known_strings);
}
