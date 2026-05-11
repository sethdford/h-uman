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
extern size_t imessage_sanitize_output(char *buf, size_t len);
extern size_t imessage_build_attach_script(char *out, size_t out_cap,
                                           const char *target_escaped,
                                           const char *path_escaped);
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

static void test_imessage_escape_strips_control_chars(void) {
    char out[256];
    char input[] = "hello\x01\x1F\x7Fworld";
    size_t len = escape_for_applescript(out, sizeof(out), input, strlen(input));
    HU_ASSERT_STR_EQ(out, "helloworld");
    HU_ASSERT_EQ(len, (size_t)10);
}

static void test_imessage_escape_tabs_and_newlines_stripped(void) {
    char out[256];
    char input[] = "a\tb\nc\rd";
    size_t len = escape_for_applescript(out, sizeof(out), input, strlen(input));
    HU_ASSERT_STR_EQ(out, "abcd");
    HU_ASSERT_EQ(len, (size_t)4);
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
    /* Voice-only: empty message + media → no text sent, media sent */
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

static void test_imessage_send_text_and_media_both_sent(void) {
    /* Normal send: message + media → both sent (mock records text) */
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imessage_create(&alloc, "+15551234567", 11, NULL, 0, &ch);
    const char *media[] = {"/tmp/photo.jpg"};
    hu_error_t err = ch.vtable->send(ch.ctx, "+15551234567", 11, "Check this out", 14, media, 1);
    HU_ASSERT_EQ(err, HU_OK);
    size_t len = 0;
    const char *msg = hu_imessage_test_get_last_message(&ch, &len);
    HU_ASSERT(msg != NULL);
    HU_ASSERT_EQ(len, 14u);
    HU_ASSERT_STR_EQ(msg, "Check this out");
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
    HU_ASSERT_STR_EQ(hu_imessage_reaction_to_tapback_name(HU_REACTION_CUSTOM_EMOJI), "emoji");
    /* Invalid enum value */
    HU_ASSERT_NULL(hu_imessage_reaction_to_tapback_name((hu_reaction_type_t)99));
}

#if HU_IS_TEST
static void test_imessage_custom_emoji_react_records(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imessage_create(&alloc, "+15551234567", 11, NULL, 0, &ch);
    HU_ASSERT_NOT_NULL(ch.vtable->react);

    hu_error_t err =
        ch.vtable->react(ch.ctx, "+15551234567", 11, 42, HU_REACTION_CUSTOM_EMOJI);
    HU_ASSERT_EQ(err, HU_OK);

    hu_reaction_type_t out_reaction = HU_REACTION_NONE;
    int64_t out_message_id = -1;
    hu_imessage_test_get_last_reaction(&ch, &out_reaction, &out_message_id);
    HU_ASSERT_EQ(out_reaction, HU_REACTION_CUSTOM_EMOJI);
    HU_ASSERT_EQ(out_message_id, 42);

    hu_imessage_destroy(&ch);
}
#endif

static void test_imessage_extract_attributed_body_basic(void) {
    unsigned char blob[] = {0x00, 0x00, 0x01, 0x2B, 0x05, 'H', 'e', 'l', 'l', 'o'};
    char out[64];
    size_t len = hu_imessage_extract_attributed_body(blob, sizeof(blob), out, sizeof(out));
    HU_ASSERT_EQ(len, 5u);
    HU_ASSERT_STR_EQ(out, "Hello");
}

static void test_imessage_extract_attributed_body_null_blob(void) {
    char out[32];
    HU_ASSERT_EQ(hu_imessage_extract_attributed_body(NULL, 0, out, sizeof(out)), 0u);
}

static void test_imessage_extract_attributed_body_small_blob(void) {
    unsigned char blob[] = {0x01, 0x2B};
    char out[32];
    HU_ASSERT_EQ(hu_imessage_extract_attributed_body(blob, sizeof(blob), out, sizeof(out)), 0u);
}

static void test_imessage_extract_attributed_body_small_output(void) {
    unsigned char blob[] = {0x01, 0x2B, 0x05, 'H', 'e', 'l', 'l', 'o'};
    char out[4];
    size_t len = hu_imessage_extract_attributed_body(blob, sizeof(blob), out, sizeof(out));
    HU_ASSERT_EQ(len, 3u);
    HU_ASSERT_EQ(out[3], '\0');
}

static void test_imessage_extract_attributed_body_no_marker(void) {
    unsigned char blob[] = {0x00, 0x00, 0x00, 0x00, 0x00};
    char out[32];
    HU_ASSERT_EQ(hu_imessage_extract_attributed_body(blob, sizeof(blob), out, sizeof(out)), 0u);
}

static void test_imessage_loop_msg_reply_to_guid_field(void) {
    hu_channel_loop_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    HU_ASSERT_EQ(msg.reply_to_guid[0], '\0');
    strncpy(msg.reply_to_guid, "p:0/ABCD-1234", sizeof(msg.reply_to_guid) - 1);
    HU_ASSERT_STR_EQ(msg.reply_to_guid, "p:0/ABCD-1234");
}

static void test_imessage_guid_lookup_stub_returns_not_supported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char out[128];
    size_t out_len = 0;
    hu_error_t err =
        hu_imessage_lookup_message_by_guid(&alloc, "test-guid", 9, out, sizeof(out), &out_len);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(out_len, 0u);
}

static void test_imessage_loop_msg_unsent_field(void) {
    hu_channel_loop_msg_t msg = {0};
    HU_ASSERT_EQ(msg.was_unsent, false);
    msg.was_unsent = true;
    HU_ASSERT_EQ(msg.was_unsent, true);
}

#if HU_IS_TEST
static void test_imessage_typing_cache_field_exists(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imessage_create(&alloc, "+15551234567", 11, NULL, 0, &ch);
    HU_ASSERT_NOT_NULL(ch.vtable->send);
    hu_error_t err = ch.vtable->send(ch.ctx, "+15551234567", 11, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    err = ch.vtable->send(ch.ctx, "+15551234567", 11, "again", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_imessage_destroy(&ch);
}
#endif

#if HU_IS_TEST
static void test_imessage_react_test_records(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    hu_imessage_create(&alloc, "+15551234567", 11, NULL, 0, &ch);
    HU_ASSERT_NOT_NULL(ch.vtable->react);

    const char *target = "+15551234567";
    hu_error_t err = ch.vtable->react(ch.ctx, target, 11, 12345, HU_REACTION_HEART);
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

static void test_imessage_gif_tapback_stub_returns_zero(void) {
    int count = hu_imessage_count_recent_gif_tapbacks("alice@example.com", 17);
    HU_ASSERT_EQ(count, 0);
}

static void test_imessage_allow_from_filter_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    const char *allowed[] = {"+15559999999"};
    hu_error_t err = hu_imessage_create(&alloc, "+15551234567", 12, allowed, 1, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    hu_imessage_destroy(&ch);
}


/* -- imessage_sanitize_output tests -- */

static void test_sanitize_strips_ai_phrases(void) {
    char buf[256];
    strcpy(buf, "I'd be happy to help you with that");
    size_t len = imessage_sanitize_output(buf, strlen(buf));
    HU_ASSERT(strstr(buf, "I'd be happy to") == NULL);
    HU_ASSERT(len < 35);
    HU_ASSERT(len > 0);
}

static void test_sanitize_strips_great_question(void) {
    char buf[256];
    strcpy(buf, "Great question! The answer is 42");
    size_t len = imessage_sanitize_output(buf, strlen(buf));
    HU_ASSERT(strstr(buf, "Great question!") == NULL);
    HU_ASSERT(strstr(buf, "42") != NULL);
    (void)len;
}

static void test_sanitize_strips_as_an_ai(void) {
    char buf[256];
    strcpy(buf, "As an AI, I think that's cool");
    size_t len = imessage_sanitize_output(buf, strlen(buf));
    HU_ASSERT(strstr(buf, "As an AI") == NULL);
    (void)len;
}

static void test_sanitize_preserves_normal_text(void) {
    char buf[256];
    strcpy(buf, "haha yeah that's wild");
    size_t orig_len = strlen(buf);
    size_t len = imessage_sanitize_output(buf, orig_len);
    HU_ASSERT_EQ(len, orig_len);
    HU_ASSERT_STR_EQ(buf, "haha yeah that's wild");
}

static void test_sanitize_collapses_double_spaces(void) {
    char buf[256];
    strcpy(buf, "hello  world");
    size_t len = imessage_sanitize_output(buf, strlen(buf));
    HU_ASSERT_STR_EQ(buf, "hello world");
    HU_ASSERT_EQ(len, 11u);
}

static void test_sanitize_trims_whitespace(void) {
    char buf[256];
    strcpy(buf, "  hello world  ");
    size_t len = imessage_sanitize_output(buf, strlen(buf));
    HU_ASSERT_STR_EQ(buf, "hello world");
    HU_ASSERT_EQ(len, 11u);
}

static void test_sanitize_empty_safe(void) {
    char buf[4] = "";
    size_t len = imessage_sanitize_output(buf, 0);
    HU_ASSERT_EQ(len, 0u);
}

static void test_sanitize_null_safe(void) {
    size_t len = imessage_sanitize_output(NULL, 10);
    HU_ASSERT_EQ(len, 0u);
}

/* -- imessage_build_attach_script tests -- */

static void test_build_attach_script_basic(void) {
    char out[1024];
    size_t len = imessage_build_attach_script(out, sizeof(out),
                                              "+15551234567",
                                              "/tmp/human_img_test.png");
    HU_ASSERT(len > 0);
    HU_ASSERT(strstr(out, "POSIX file") != NULL);
    HU_ASSERT(strstr(out, "/tmp/human_img_test.png") != NULL);
    HU_ASSERT(strstr(out, "+15551234567") != NULL);
    HU_ASSERT(strstr(out, "tell application") != NULL);
    HU_ASSERT(strstr(out, "end tell") != NULL);
}

static void test_build_attach_script_small_buf_fails(void) {
    char out[32];
    size_t len = imessage_build_attach_script(out, sizeof(out),
                                              "+15551234567",
                                              "/tmp/img.png");
    HU_ASSERT_EQ(len, 0u);
}

static void test_build_attach_script_null_args(void) {
    char out[512];
    HU_ASSERT_EQ(imessage_build_attach_script(NULL, 512, "t", "p"), 0u);
    HU_ASSERT_EQ(imessage_build_attach_script(out, 512, NULL, "p"), 0u);
    HU_ASSERT_EQ(imessage_build_attach_script(out, 512, "t", NULL), 0u);
}

static void test_build_attach_script_with_escaped_path(void) {
    char tgt[256], path[256], out[1024];
    escape_for_applescript(tgt, sizeof(tgt), "alice@me.com", 12);
    escape_for_applescript(path, sizeof(path), "/tmp/file with spaces.jpg", 25);
    size_t len = imessage_build_attach_script(out, sizeof(out), tgt, path);
    HU_ASSERT(len > 0);
    HU_ASSERT(strstr(out, "alice@me.com") != NULL);
    HU_ASSERT(strstr(out, "/tmp/file with spaces.jpg") != NULL);
}

void run_imessage_extended_tests(void) {
    HU_TEST_SUITE("iMessage Extended");

#if (defined(__APPLE__) && defined(__MACH__)) || HU_IS_TEST
    HU_RUN_TEST(test_imessage_escape_basic);
    HU_RUN_TEST(test_imessage_escape_quotes);
    HU_RUN_TEST(test_imessage_escape_backslash);
    HU_RUN_TEST(test_imessage_escape_empty);
    HU_RUN_TEST(test_imessage_escape_truncation);
    HU_RUN_TEST(test_imessage_escape_strips_control_chars);
    HU_RUN_TEST(test_imessage_escape_tabs_and_newlines_stripped);
    HU_RUN_TEST(test_sanitize_strips_ai_phrases);
    HU_RUN_TEST(test_sanitize_strips_great_question);
    HU_RUN_TEST(test_sanitize_strips_as_an_ai);
    HU_RUN_TEST(test_sanitize_preserves_normal_text);
    HU_RUN_TEST(test_sanitize_collapses_double_spaces);
    HU_RUN_TEST(test_sanitize_trims_whitespace);
    HU_RUN_TEST(test_sanitize_empty_safe);
    HU_RUN_TEST(test_sanitize_null_safe);
    HU_RUN_TEST(test_build_attach_script_basic);
    HU_RUN_TEST(test_build_attach_script_small_buf_fails);
    HU_RUN_TEST(test_build_attach_script_null_args);
    HU_RUN_TEST(test_build_attach_script_with_escaped_path);
#endif
    HU_RUN_TEST(test_imessage_reaction_to_tapback_mapping);
    HU_RUN_TEST(test_imessage_create_with_allow_from);
    HU_RUN_TEST(test_imessage_create_null_alloc);
    HU_RUN_TEST(test_imessage_health_check);
    HU_RUN_TEST(test_imessage_gif_tapback_stub_returns_zero);
    HU_RUN_TEST(test_imessage_allow_from_filter_no_crash);
#if defined(__APPLE__) && defined(__MACH__)
    HU_RUN_TEST(test_imessage_send_test_mode);
    HU_RUN_TEST(test_imessage_send_media_only_no_crash);
    HU_RUN_TEST(test_imessage_send_text_and_media_both_sent);
    HU_RUN_TEST(test_imessage_poll_test_mode);
#endif
    HU_RUN_TEST(test_imessage_loop_msg_reply_to_guid_field);
    HU_RUN_TEST(test_imessage_guid_lookup_stub_returns_not_supported);
    HU_RUN_TEST(test_imessage_loop_msg_unsent_field);
    HU_RUN_TEST(test_imessage_extract_attributed_body_basic);
    HU_RUN_TEST(test_imessage_extract_attributed_body_null_blob);
    HU_RUN_TEST(test_imessage_extract_attributed_body_small_blob);
    HU_RUN_TEST(test_imessage_extract_attributed_body_small_output);
    HU_RUN_TEST(test_imessage_extract_attributed_body_no_marker);
#if HU_IS_TEST
    HU_RUN_TEST(test_imessage_typing_cache_field_exists);
    HU_RUN_TEST(test_imessage_react_test_records);
    HU_RUN_TEST(test_imessage_custom_emoji_react_records);
#endif
}
#else
void run_imessage_extended_tests(void) {
    (void)0; /* iMessage channel not built */
}
#endif
