#if HU_HAS_IMESSAGE
#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/channels/imessage.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <string.h>

#if (defined(__APPLE__) && defined(__MACH__)) || HU_IS_TEST
extern size_t escape_for_applescript(char *out, size_t out_cap, const char *in, size_t in_len);
#endif

#if (defined(__APPLE__) && defined(__MACH__)) || HU_IS_TEST
static void test_imessage_escape_basic(void) {
    char out[256];
    size_t len = escape_for_applescript(out, sizeof(out), "hello", 5);
    HU_ASSERT_EQ(len, 5u);
    HU_ASSERT_STR_EQ(out, "hello");
}

static void test_imessage_escape_quotes(void) {
    char out[256];
    size_t len = escape_for_applescript(out, sizeof(out), "he said \"hi\"", 12);
    HU_ASSERT_TRUE(len > 12); /* escaped quotes should be longer */
    HU_ASSERT_TRUE(strstr(out, "\\\"") != NULL);
}

static void test_imessage_escape_backslash(void) {
    char out[256];
    size_t len = escape_for_applescript(out, sizeof(out), "path\\file", 9);
    HU_ASSERT_TRUE(len > 9);
    HU_ASSERT_TRUE(strstr(out, "\\\\") != NULL);
}

static void test_imessage_escape_empty(void) {
    char out[16];
    size_t len = escape_for_applescript(out, sizeof(out), "", 0);
    HU_ASSERT_EQ(len, 0u);
    HU_ASSERT_STR_EQ(out, "");
}

static void test_imessage_escape_truncation(void) {
    char out[8]; /* small buffer */
    size_t len = escape_for_applescript(out, sizeof(out), "hello world this is a long string", 33);
    HU_ASSERT_TRUE(len < sizeof(out));
    HU_ASSERT_TRUE(out[len] == '\0');
}
#endif

static void test_imessage_create_with_allow_from(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    const char *allow_from[] = {"+15551234567", "user@example.com"};
    hu_error_t err = hu_imessage_create(&alloc, "+15559876543", 11, allow_from, 2, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "imessage");
    hu_imessage_destroy(&ch);
}

static void test_imessage_create_null_alloc(void) {
    hu_channel_t ch;
    hu_error_t err = hu_imessage_create(NULL, "+15551234567", 11, NULL, 0, &ch);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_imessage_health_check(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imessage_create(&alloc, "+15551234567", 11, NULL, 0, &ch);
    (void)ch.vtable->health_check(
        ch.ctx); /* platform-dependent: true on macOS with db, false otherwise */
    hu_imessage_destroy(&ch);
}

static void test_imessage_send_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imessage_create(&alloc, "+15551234567", 11, NULL, 0, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "+15551234567", 11, "test message", 12, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_imessage_destroy(&ch);
}

static void test_imessage_send_media_only_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imessage_create(&alloc, "+15551234567", 11, NULL, 0, &ch);
    const char *media[] = {"/tmp/voice.m4a"};
    hu_error_t err = ch.vtable->send(ch.ctx, "+15551234567", 11, "", 0, media, 1);
    HU_ASSERT_EQ(err, HU_OK);
    size_t len = 0;
    const char *msg = hu_imessage_test_get_last_message(&ch, &len);
    HU_ASSERT(msg != NULL);
    HU_ASSERT_EQ(len, 0u);
    HU_ASSERT_STR_EQ(msg, "");
    hu_imessage_destroy(&ch);
}

static void test_imessage_poll_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imessage_create(&alloc, "+15551234567", 11, NULL, 0, &ch);
    hu_channel_loop_msg_t msgs[4];
    size_t out_count = 99;
    hu_error_t err = hu_imessage_poll(ch.ctx, &alloc, msgs, 4, &out_count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out_count, 0u);
    hu_imessage_destroy(&ch);
}

static void test_imessage_reaction_to_tapback_mapping(void) {
    HU_ASSERT_NULL(hu_imessage_reaction_to_tapback_name(HU_REACTION_NONE));
    HU_ASSERT_STR_EQ(hu_imessage_reaction_to_tapback_name(HU_REACTION_HEART), "love");
    HU_ASSERT_STR_EQ(hu_imessage_reaction_to_tapback_name(HU_REACTION_THUMBS_UP), "like");
    HU_ASSERT_STR_EQ(hu_imessage_reaction_to_tapback_name(HU_REACTION_THUMBS_DOWN), "dislike");
    HU_ASSERT_STR_EQ(hu_imessage_reaction_to_tapback_name(HU_REACTION_HAHA), "laugh");
    HU_ASSERT_STR_EQ(hu_imessage_reaction_to_tapback_name(HU_REACTION_EMPHASIS), "emphasize");
    HU_ASSERT_STR_EQ(hu_imessage_reaction_to_tapback_name(HU_REACTION_QUESTION), "question");
    /* Invalid enum value */
    HU_ASSERT_NULL(hu_imessage_reaction_to_tapback_name((hu_reaction_type_t)99));
}

#if HU_IS_TEST
static void test_imessage_react_test_records(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imessage_create(&alloc, "+15551234567", 11, NULL, 0, &ch);
    HU_ASSERT_NOT_NULL(ch.vtable->react);

    const char *target = "+15551234567";
    hu_error_t err =
        ch.vtable->react(ch.ctx, target, 11, 12345, HU_REACTION_HEART);
    HU_ASSERT_EQ(err, HU_OK);

    hu_reaction_type_t out_reaction = HU_REACTION_NONE;
    int64_t out_message_id = -1;
    hu_imessage_test_get_last_reaction(&ch, &out_reaction, &out_message_id);
    HU_ASSERT_EQ(out_reaction, HU_REACTION_HEART);
    HU_ASSERT_EQ(out_message_id, 12345);

    err = ch.vtable->react(ch.ctx, target, 11, 67890, HU_REACTION_HAHA);
    HU_ASSERT_EQ(err, HU_OK);
    hu_imessage_test_get_last_reaction(&ch, &out_reaction, &out_message_id);
    HU_ASSERT_EQ(out_reaction, HU_REACTION_HAHA);
    HU_ASSERT_EQ(out_message_id, 67890);

    hu_imessage_destroy(&ch);
}
#endif

void run_imessage_extended_tests(void) {
    HU_TEST_SUITE("iMessage Extended");

#if (defined(__APPLE__) && defined(__MACH__)) || HU_IS_TEST
    HU_RUN_TEST(test_imessage_escape_basic);
    HU_RUN_TEST(test_imessage_escape_quotes);
    HU_RUN_TEST(test_imessage_escape_backslash);
    HU_RUN_TEST(test_imessage_escape_empty);
    HU_RUN_TEST(test_imessage_escape_truncation);
#endif
    HU_RUN_TEST(test_imessage_reaction_to_tapback_mapping);
    HU_RUN_TEST(test_imessage_create_with_allow_from);
    HU_RUN_TEST(test_imessage_create_null_alloc);
    HU_RUN_TEST(test_imessage_health_check);
#if defined(__APPLE__) && defined(__MACH__)
    HU_RUN_TEST(test_imessage_send_test_mode);
    HU_RUN_TEST(test_imessage_poll_test_mode);
#endif
#if HU_IS_TEST
    HU_RUN_TEST(test_imessage_react_test_records);
#endif
}
#else
void run_imessage_extended_tests(void) {
    (void)0; /* iMessage channel not built */
}
#endif
