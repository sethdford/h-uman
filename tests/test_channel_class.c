#include "human/channel_class.h"
#include "test_framework.h"
#include <string.h>

static void known_imessage_returns_text_fast(void) {
    HU_ASSERT_EQ(hu_channel_class_for_name("imessage"), HU_CHANNEL_CLASS_TEXT_FAST);
}

static void known_slack_returns_text_async(void) {
    HU_ASSERT_EQ(hu_channel_class_for_name("slack"), HU_CHANNEL_CLASS_TEXT_ASYNC);
}

static void known_voice_returns_voice(void) {
    HU_ASSERT_EQ(hu_channel_class_for_name("voice"), HU_CHANNEL_CLASS_VOICE);
}

static void case_insensitive_imessage(void) {
    HU_ASSERT_EQ(hu_channel_class_for_name("ImEssAgE"), HU_CHANNEL_CLASS_TEXT_FAST);
}

static void case_insensitive_slack(void) {
    HU_ASSERT_EQ(hu_channel_class_for_name("SLACK"), HU_CHANNEL_CLASS_TEXT_ASYNC);
    HU_ASSERT_EQ(hu_channel_class_for_name("Slack"), HU_CHANNEL_CLASS_TEXT_ASYNC);
    HU_ASSERT_EQ(hu_channel_class_for_name("sLaCk"), HU_CHANNEL_CLASS_TEXT_ASYNC);
}

static void unknown_returns_unknown(void) {
    HU_ASSERT_EQ(hu_channel_class_for_name("definitely_not_a_channel"), HU_CHANNEL_CLASS_UNKNOWN);
}

static void null_returns_unknown(void) {
    HU_ASSERT_EQ(hu_channel_class_for_name(NULL), HU_CHANNEL_CLASS_UNKNOWN);
}

static void empty_string_returns_unknown(void) {
    HU_ASSERT_EQ(hu_channel_class_for_name(""), HU_CHANNEL_CLASS_UNKNOWN);
}

static void overlong_string_returns_unknown(void) {
    /* 200 'a' characters — well past the 63-byte limit */
    char buf[201];
    memset(buf, 'a', 200);
    buf[200] = '\0';
    HU_ASSERT_EQ(hu_channel_class_for_name(buf), HU_CHANNEL_CLASS_UNKNOWN);
}

void run_channel_class_tests(void) {
    HU_TEST_SUITE("channel_class");
    HU_RUN_TEST(known_imessage_returns_text_fast);
    HU_RUN_TEST(known_slack_returns_text_async);
    HU_RUN_TEST(known_voice_returns_voice);
    HU_RUN_TEST(case_insensitive_imessage);
    HU_RUN_TEST(case_insensitive_slack);
    HU_RUN_TEST(unknown_returns_unknown);
    HU_RUN_TEST(null_returns_unknown);
    HU_RUN_TEST(empty_string_returns_unknown);
    HU_RUN_TEST(overlong_string_returns_unknown);
}
