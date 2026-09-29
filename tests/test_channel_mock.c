/* Contract tests for the shared channel mock harness (src/channels/channel_mock.c).
 * The 27 per-channel copies this replaced enforced these caps by hand; pin them
 * once here so a channel cannot drift. */
#include "human/channels/channel_mock.h"
#include "test_framework.h"
#include <string.h>

static void mock_inject_stores_key_and_content(void) {
    hu_channel_mock_t m;
    memset(&m, 0, sizeof(m));
    HU_ASSERT_EQ(hu_channel_mock_inject(&m, "sess-1", 6, "hello", 5), HU_OK);
    HU_ASSERT_EQ(m.count, 1);
    HU_ASSERT_STR_EQ(m.msgs[0].session_key, "sess-1");
    HU_ASSERT_STR_EQ(m.msgs[0].content, "hello");
}

static void mock_inject_truncates_to_historical_caps(void) {
    hu_channel_mock_t m;
    memset(&m, 0, sizeof(m));
    char key[300], body[6000];
    memset(key, 'k', sizeof(key));
    memset(body, 'b', sizeof(body));
    HU_ASSERT_EQ(hu_channel_mock_inject(&m, key, sizeof(key), body, sizeof(body)), HU_OK);
    HU_ASSERT_EQ(strlen(m.msgs[0].session_key), (size_t)(HU_CHANNEL_MOCK_KEY_CAP - 1));
    HU_ASSERT_EQ(strlen(m.msgs[0].content), (size_t)(HU_CHANNEL_MOCK_CONTENT_CAP - 1));
}

static void mock_inject_refuses_ninth_message(void) {
    hu_channel_mock_t m;
    memset(&m, 0, sizeof(m));
    for (int i = 0; i < HU_CHANNEL_MOCK_MAX_MSGS; i++)
        HU_ASSERT_EQ(hu_channel_mock_inject(&m, "s", 1, "c", 1), HU_OK);
    HU_ASSERT_EQ(hu_channel_mock_inject(&m, "s", 1, "c", 1), HU_ERR_OUT_OF_MEMORY);
    HU_ASSERT_EQ(m.count, (size_t)HU_CHANNEL_MOCK_MAX_MSGS);
}

static void mock_inject_null_args_are_empty_strings(void) {
    hu_channel_mock_t m;
    memset(&m, 0, sizeof(m));
    HU_ASSERT_EQ(hu_channel_mock_inject(&m, NULL, 0, NULL, 0), HU_OK);
    HU_ASSERT_STR_EQ(m.msgs[0].session_key, "");
    HU_ASSERT_STR_EQ(m.msgs[0].content, "");
    HU_ASSERT_EQ(hu_channel_mock_inject(NULL, "s", 1, "c", 1), HU_ERR_INVALID_ARGUMENT);
}

static void mock_record_send_keeps_last_message(void) {
    hu_channel_mock_t m;
    memset(&m, 0, sizeof(m));
    hu_channel_mock_record_send(&m, "first", 5);
    hu_channel_mock_record_send(&m, "second", 6);
    HU_ASSERT_STR_EQ(m.last_message, "second");
    HU_ASSERT_EQ(m.last_message_len, 6);
    char big[6000];
    memset(big, 'x', sizeof(big));
    hu_channel_mock_record_send(&m, big, sizeof(big));
    HU_ASSERT_EQ(m.last_message_len, (size_t)(HU_CHANNEL_MOCK_CONTENT_CAP - 1));
}

void run_channel_mock_tests(void) {
    HU_TEST_SUITE("ChannelMock");
    HU_RUN_TEST(mock_inject_stores_key_and_content);
    HU_RUN_TEST(mock_inject_truncates_to_historical_caps);
    HU_RUN_TEST(mock_inject_refuses_ninth_message);
    HU_RUN_TEST(mock_inject_null_args_are_empty_strings);
    HU_RUN_TEST(mock_record_send_keeps_last_message);
}
