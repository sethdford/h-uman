#include "human/channels/imessage.h"
#include "test_framework.h"
#include <stdbool.h>
#include <string.h>

static int picker_call_count = 0;
static const char *picker_last_emoji = NULL;

static bool picker_succeeds_stub(const char *emoji_utf8) {
    picker_call_count++;
    picker_last_emoji = emoji_utf8;
    return true;
}

static bool picker_fails_stub(const char *emoji_utf8) {
    (void)emoji_utf8;
    picker_call_count++;
    return false;
}

/* AC: custom emoji dispatched to sub-picker; on success returns HU_OK. */
static void custom_emoji_dispatched_to_sub_picker(void) {
    picker_call_count = 0;
    picker_last_emoji = NULL;
    hu_imessage_set_test_react_emoji_stub(picker_succeeds_stub);

    hu_error_t err = hu_imessage_react_emoji_subpicker(NULL, "+15555551212", 12, 99, "😍");
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(picker_call_count, 1);
    HU_ASSERT_STR_EQ(picker_last_emoji, "😍");

    hu_imessage_set_test_react_emoji_stub(NULL);
}

/* AC: sub-picker miss returns NOT_SUPPORTED so caller can fall back
 * (to classic-tapback mapping in D2). */
static void sub_picker_miss_returns_not_supported(void) {
    picker_call_count = 0;
    hu_imessage_set_test_react_emoji_stub(picker_fails_stub);

    hu_error_t err = hu_imessage_react_emoji_subpicker(NULL, "+15555551212", 12, 99, "🦄");
    HU_ASSERT_EQ((int)err, (int)HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(picker_call_count, 1);

    hu_imessage_set_test_react_emoji_stub(NULL);
}

/* AC: NULL or empty emoji is invalid input. */
static void null_or_empty_emoji_invalid_argument(void) {
    HU_ASSERT_EQ((int)hu_imessage_react_emoji_subpicker(NULL, "+15555551212", 12, 99, NULL),
                 (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((int)hu_imessage_react_emoji_subpicker(NULL, "+15555551212", 12, 99, ""),
                 (int)HU_ERR_INVALID_ARGUMENT);
}

void run_imessage_custom_tapback_tests(void) {
    HU_TEST_SUITE("imessage_custom_tapback");
    HU_RUN_TEST(custom_emoji_dispatched_to_sub_picker);
    HU_RUN_TEST(sub_picker_miss_returns_not_supported);
    HU_RUN_TEST(null_or_empty_emoji_invalid_argument);
}
