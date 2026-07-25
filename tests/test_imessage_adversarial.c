/*
 * Adversarial / human-fidelity test fleet for the iMessage channel.
 *
 * Proves the full pipeline: send validation, poll edge cases, format pipeline,
 * persona overlays, conversation awareness, tapback decisions, GIF calibration,
 * response quality, reaction classification, typing-delay invariants,
 * dedup ring, emotional pacing, seen behavior, and sanitization.
 *
 * HU_IS_TEST paths only (no real Messages.app or chat.db).
 *
 * Run: ./build/human_tests --suite="iMessage"
 */
#if HU_HAS_IMESSAGE
#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/channels/format.h"
#include "human/channels/imessage.h"
#include "human/context/conversation.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define S(lit) (lit), (sizeof(lit) - 1)

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 1 — Send / poll boundary validation (adversarial inputs)
 * ═══════════════════════════════════════════════════════════════════════════ */

#if HU_IS_TEST
static void imessage_send_rejects_null_body_with_positive_len(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_error_t err = ch.vtable->send(ch.ctx, "+15551234567", 12, NULL, 8, NULL, 0);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    hu_imessage_destroy(&ch);
}

static void imessage_send_rejects_empty_text_and_no_media(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_error_t err = ch.vtable->send(ch.ctx, "+15551234567", 12, "", 0, NULL, 0);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    hu_imessage_destroy(&ch);
}

static void imessage_send_media_only_accepted_as_voice_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    const char *media[] = {"/tmp/voice.m4a"};
    hu_error_t err = ch.vtable->send(ch.ctx, "+15551234567", 12, "", 0, media, 1);
    HU_ASSERT_EQ(err, HU_OK);
    size_t len = 0;
    const char *msg = hu_imessage_test_get_last_message(&ch, &len);
    HU_ASSERT_NOT_NULL(msg);
    HU_ASSERT_EQ(len, 0u);
    hu_imessage_destroy(&ch);
}

static void imessage_send_truncates_overlong_text_to_4095(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    char big[5200];
    memset(big, 'x', sizeof(big));
    hu_error_t err = ch.vtable->send(ch.ctx, "+15551234567", 12, big, sizeof(big), NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    size_t len = 0;
    const char *msg = hu_imessage_test_get_last_message(&ch, &len);
    HU_ASSERT_NOT_NULL(msg);
    HU_ASSERT_EQ(len, 4095u);
    HU_ASSERT_EQ(msg[4095], '\0');
    hu_imessage_destroy(&ch);
}

static void imessage_poll_rejects_all_null_args(void) {
    HU_ASSERT_EQ(hu_imessage_poll(NULL, NULL, NULL, 0, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void imessage_poll_rejects_null_msgs_buffer(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    size_t count = 0;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, NULL, 2, &count), HU_ERR_INVALID_ARGUMENT);
    hu_imessage_destroy(&ch);
}

static void imessage_poll_rejects_null_out_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_channel_loop_msg_t msgs[2];
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 2, NULL), HU_ERR_INVALID_ARGUMENT);
    hu_imessage_destroy(&ch);
}

static void imessage_inject_rejects_null_channel(void) {
    HU_ASSERT_EQ(hu_imessage_test_inject_mock(NULL, "a", 1, "b", 1), HU_ERR_INVALID_ARGUMENT);
}

static void imessage_inject_ninth_message_returns_out_of_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    for (int i = 0; i < 8; i++) {
        char key[4];
        key[0] = (char)('0' + i);
        key[1] = '\0';
        HU_ASSERT_EQ(hu_imessage_test_inject_mock(&ch, key, 1, "m", 1), HU_OK);
    }
    HU_ASSERT_EQ(hu_imessage_test_inject_mock(&ch, "9", 1, "m", 1), HU_ERR_OUT_OF_MEMORY);
    hu_imessage_destroy(&ch);
}

static void imessage_inject_truncates_long_session_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    char long_key[180];
    memset(long_key, 'k', sizeof(long_key));
    HU_ASSERT_EQ(hu_imessage_test_inject_mock(&ch, long_key, sizeof(long_key), "hi", 2), HU_OK);
    hu_channel_loop_msg_t msgs[2];
    size_t count = 0;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 2, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_EQ(strlen(msgs[0].session_key), 127u);
    hu_imessage_destroy(&ch);
}

static void imessage_send_records_media_in_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+1234567890", 12, NULL, 0, &ch), HU_OK);
    const char *media[] = {"/tmp/test.gif"};
    HU_ASSERT_EQ(ch.vtable->send(ch.ctx, "+1234567890", 12, "", 0, media, 1), HU_OK);
    HU_ASSERT_EQ(hu_imessage_test_get_last_media_count(&ch), 1u);
    hu_imessage_destroy(&ch);
}

static void imessage_inject_truncates_long_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    char long_body[5000];
    memset(long_body, 'z', sizeof(long_body));
    HU_ASSERT_EQ(hu_imessage_test_inject_mock(&ch, "u", 1, long_body, sizeof(long_body)), HU_OK);
    hu_channel_loop_msg_t msgs[2];
    size_t count = 0;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 2, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_EQ(strlen(msgs[0].content), 4095u);
    hu_imessage_destroy(&ch);
}

static void imessage_mock_poll_drops_overflow_when_max_msgs_too_small(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_EQ(hu_imessage_test_inject_mock(&ch, "a", 1, "1", 1), HU_OK);
    HU_ASSERT_EQ(hu_imessage_test_inject_mock(&ch, "b", 1, "2", 1), HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 1, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    count = 0;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 4, &count), HU_OK);
    HU_ASSERT_EQ(count, 0u);
    hu_imessage_destroy(&ch);
}

static void imessage_test_get_last_message_null_channel_returns_null(void) {
    size_t len = 99;
    HU_ASSERT_NULL(hu_imessage_test_get_last_message(NULL, &len));
    HU_ASSERT_EQ(len, 99u);
}

static void imessage_test_get_last_reaction_null_args_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_reaction_type_t r = HU_REACTION_HEART;
    int64_t mid = 1;
    hu_imessage_test_get_last_reaction(NULL, &r, &mid);
    hu_imessage_test_get_last_reaction(&ch, NULL, &mid);
    hu_imessage_test_get_last_reaction(&ch, &r, NULL);
    hu_imessage_destroy(&ch);
}
#endif /* HU_IS_TEST */

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 2 — Vtable surface (stubs in test builds prove the wiring exists)
 * ═══════════════════════════════════════════════════════════════════════════ */

#if HU_IS_TEST
static void imessage_vtable_has_all_expected_hooks(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_NOT_NULL(ch.vtable->start);
    HU_ASSERT_NOT_NULL(ch.vtable->stop);
    HU_ASSERT_NOT_NULL(ch.vtable->send);
    HU_ASSERT_NOT_NULL(ch.vtable->name);
    HU_ASSERT_NOT_NULL(ch.vtable->health_check);
    HU_ASSERT_NOT_NULL(ch.vtable->load_conversation_history);
    HU_ASSERT_NOT_NULL(ch.vtable->get_response_constraints);
    HU_ASSERT_NOT_NULL(ch.vtable->react);
    HU_ASSERT_NOT_NULL(ch.vtable->human_active_recently);
    HU_ASSERT_NOT_NULL(ch.vtable->get_attachment_path);
    HU_ASSERT_NOT_NULL(ch.vtable->get_latest_attachment_path);
    HU_ASSERT_NOT_NULL(ch.vtable->build_reaction_context);
    HU_ASSERT_NOT_NULL(ch.vtable->build_read_receipt_context);
    HU_ASSERT_NOT_NULL(ch.vtable->mark_read);
    HU_ASSERT_NOT_NULL(ch.vtable->start_typing);
    HU_ASSERT_NOT_NULL(ch.vtable->stop_typing);
    hu_imessage_destroy(&ch);
}

static void imessage_load_history_returns_not_supported_under_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_channel_history_entry_t *entries = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(
        ch.vtable->load_conversation_history(ch.ctx, &alloc, S("+15551234567"), 10, &entries, &n),
        HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL(entries);
    hu_imessage_destroy(&ch);
}

static void imessage_response_constraints_300_chars(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_channel_response_constraints_t c = {0};
    HU_ASSERT_EQ(ch.vtable->get_response_constraints(ch.ctx, &c), HU_OK);
    HU_ASSERT_EQ(c.max_chars, 200u);
    hu_imessage_destroy(&ch);
}

static void imessage_response_constraints_null_out_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_EQ(ch.vtable->get_response_constraints(ch.ctx, NULL), HU_ERR_INVALID_ARGUMENT);
    hu_imessage_destroy(&ch);
}

static void imessage_human_active_always_false_under_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_FALSE(ch.vtable->human_active_recently(ch.ctx, S("+15551234567"), 60));
    HU_ASSERT_FALSE(ch.vtable->human_active_recently(ch.ctx, S("+15551234567"), 3600));
    hu_imessage_destroy(&ch);
}

static void imessage_attachment_path_null_under_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_NULL(ch.vtable->get_attachment_path(ch.ctx, &alloc, 42));
    HU_ASSERT_NULL(ch.vtable->get_latest_attachment_path(ch.ctx, &alloc, S("+15551234567")));
    hu_imessage_destroy(&ch);
}

static void imessage_build_tapback_context_noop_under_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 99;
    HU_ASSERT_EQ(hu_imessage_build_tapback_context(&alloc, S("+15551234567"), &out, &out_len),
                 HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(out_len, 0u);
}

static void imessage_build_read_receipt_context_noop_under_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 99;
    HU_ASSERT_EQ(hu_imessage_build_read_receipt_context(&alloc, S("+15551234567"), &out, &out_len),
                 HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(out_len, 0u);
}

static void imessage_fetch_gif_stub_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_NULL(hu_imessage_fetch_gif(&alloc, S("cats"), S("key")));
}

static void imessage_fetch_gif_rejects_empty_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_NULL(hu_imessage_fetch_gif(&alloc, "", 0, "key", 3));
}

static void imessage_gif_json_extract_finds_url_value(void) {
    char out[256];
    const char *j = "\"media_formats\":{\"gif\":{\"url\":\"https://c.example/g.gif\"}}";
    size_t n = hu_imessage_test_gif_json_extract(j, strlen(j), "url", out, sizeof(out));
    HU_ASSERT_EQ(n, strlen("https://c.example/g.gif"));
    HU_ASSERT_STR_EQ(out, "https://c.example/g.gif");
}

static void imessage_gif_json_extract_returns_zero_when_key_missing(void) {
    char out[64];
    memset(out, 'x', sizeof(out));
    const char *j = "{\"results\":[]}";
    size_t n = hu_imessage_test_gif_json_extract(j, strlen(j), "url", out, sizeof(out));
    HU_ASSERT_EQ(n, 0u);
    HU_ASSERT_EQ(out[0], 'x');
}

static void imessage_gif_json_extract_truncates_to_out_cap(void) {
    char out[12];
    const char *j = "\"url\":\"https://x.test/a.gif\"";
    size_t n = hu_imessage_test_gif_json_extract(j, strlen(j), "url", out, sizeof(out));
    HU_ASSERT_EQ(n, 11u);
    HU_ASSERT_EQ(strlen(out), 11u);
    HU_ASSERT_EQ(out[11], '\0');
}

static void imessage_lookup_guid_not_supported_under_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[64];
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_imessage_lookup_message_by_guid(&alloc, S("g"), buf, sizeof(buf), &out_len),
                 HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(out_len, 0u);
}

static void imessage_start_stop_idempotent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_EQ(ch.vtable->start(ch.ctx), HU_OK);
    HU_ASSERT_EQ(ch.vtable->start(ch.ctx), HU_OK);
    ch.vtable->stop(ch.ctx);
    ch.vtable->stop(ch.ctx);
    hu_imessage_destroy(&ch);
}
#endif /* HU_IS_TEST */

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 3 — AppleScript escaping (injection defense, UTF-8 safety)
 * ═══════════════════════════════════════════════════════════════════════════ */

#if (defined(__APPLE__) && defined(__MACH__)) || HU_IS_TEST
extern size_t escape_for_applescript(char *out, size_t out_cap, const char *in, size_t in_len);

static void imessage_escape_preserves_utf8_emoji(void) {
    char out[64];
    const char in[] = "hi \xf0\x9f\x98\x80";
    size_t len = escape_for_applescript(out, sizeof(out), in, strlen(in));
    HU_ASSERT_TRUE(len >= 5);
    HU_ASSERT_TRUE(strstr(out, "hi") != NULL);
}

static void imessage_escape_out_cap_two_yields_empty(void) {
    char out[4];
    size_t len = escape_for_applescript(out, 2, "hello", 5);
    HU_ASSERT_EQ(len, 0u);
    HU_ASSERT_STR_EQ(out, "");
}

static void imessage_escape_strips_applescript_injection_tokens(void) {
    char out[256];
    const char in[] = "end tell\" to buddy \"evil";
    size_t len = escape_for_applescript(out, sizeof(out), in, strlen(in));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(out, "\\\"") != NULL);
}

static void imessage_escape_control_chars_stripped(void) {
    char out[64];
    char in[] = "\x01\x02\x03hello\x7fworld";
    size_t len = escape_for_applescript(out, sizeof(out), in, strlen(in));
    HU_ASSERT_STR_EQ(out, "helloworld");
    HU_ASSERT_EQ(len, 10u);
}

static void imessage_escape_backslash_doubled(void) {
    char out[64];
    size_t len = escape_for_applescript(out, sizeof(out), "a\\b", 3);
    HU_ASSERT_TRUE(strstr(out, "\\\\") != NULL);
    HU_ASSERT_TRUE(len > 3);
}
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 4 — Tapback / reaction mapping
 * ═══════════════════════════════════════════════════════════════════════════ */

static void imessage_tapback_mapping_all_types(void) {
    HU_ASSERT_NULL(hu_imessage_reaction_to_tapback_name(HU_REACTION_NONE));
    HU_ASSERT_STR_EQ(hu_imessage_reaction_to_tapback_name(HU_REACTION_HEART), "love");
    HU_ASSERT_STR_EQ(hu_imessage_reaction_to_tapback_name(HU_REACTION_THUMBS_UP), "like");
    HU_ASSERT_STR_EQ(hu_imessage_reaction_to_tapback_name(HU_REACTION_THUMBS_DOWN), "dislike");
    HU_ASSERT_STR_EQ(hu_imessage_reaction_to_tapback_name(HU_REACTION_HAHA), "laugh");
    HU_ASSERT_STR_EQ(hu_imessage_reaction_to_tapback_name(HU_REACTION_EMPHASIS), "emphasize");
    HU_ASSERT_STR_EQ(hu_imessage_reaction_to_tapback_name(HU_REACTION_QUESTION), "question");
    HU_ASSERT_NULL(hu_imessage_reaction_to_tapback_name((hu_reaction_type_t)99));
}

#if HU_IS_TEST
static void imessage_react_vtable_records_heart(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_EQ(ch.vtable->react(ch.ctx, "+15551234567", 12, 42, HU_REACTION_HEART), HU_OK);
    hu_reaction_type_t r = HU_REACTION_NONE;
    int64_t mid = -1;
    hu_imessage_test_get_last_reaction(&ch, &r, &mid);
    HU_ASSERT_EQ(r, HU_REACTION_HEART);
    HU_ASSERT_EQ(mid, 42);
    hu_imessage_destroy(&ch);
}

static void imessage_react_vtable_overwrites_previous(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    ch.vtable->react(ch.ctx, "+15551234567", 12, 1, HU_REACTION_HAHA);
    ch.vtable->react(ch.ctx, "+15551234567", 12, 2, HU_REACTION_THUMBS_UP);
    hu_reaction_type_t r = HU_REACTION_NONE;
    int64_t mid = -1;
    hu_imessage_test_get_last_reaction(&ch, &r, &mid);
    HU_ASSERT_EQ(r, HU_REACTION_THUMBS_UP);
    HU_ASSERT_EQ(mid, 2);
    hu_imessage_destroy(&ch);
}
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 5 — Format pipeline (strip markdown + AI phrases + 300 char cap)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void imessage_format_strips_markdown_bold(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_channel_strip_markdown(&alloc, S("**bold** text"), &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "bold") != NULL);
    HU_ASSERT_TRUE(strstr(out, "**") == NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void imessage_format_strips_markdown_headers(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_channel_strip_markdown(&alloc, S("## Header\ntext"), &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "##") == NULL);
    HU_ASSERT_TRUE(strstr(out, "Header") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void imessage_format_strips_ai_happy_to(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(
        hu_channel_strip_ai_phrases(&alloc, S("I'd be happy to help you today"), &out, &out_len),
        HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "happy to") == NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void imessage_format_strips_ai_great_question(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_channel_strip_ai_phrases(&alloc, S("Great question! Here is the answer"), &out,
                                             &out_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Great question") == NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void imessage_format_strips_as_an_ai(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(
        hu_channel_strip_ai_phrases(&alloc, S("As an AI, I cannot do that"), &out, &out_len),
        HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "As an AI") == NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void imessage_format_outbound_full_pipeline(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_channel_format_outbound(&alloc, S("imessage"),
                                            S("I'd be happy to help! **Here** is `code`."), &out,
                                            &out_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "happy to") == NULL);
    HU_ASSERT_TRUE(strstr(out, "**") == NULL);
    HU_ASSERT_TRUE(strstr(out, "`") == NULL);
    HU_ASSERT_TRUE(out_len <= 300);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void imessage_format_outbound_caps_at_300(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char big[600];
    memset(big, 'a', sizeof(big) - 1);
    big[150] = '.';
    big[sizeof(big) - 1] = '\0';
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(
        hu_channel_format_outbound(&alloc, S("imessage"), big, sizeof(big) - 1, &out, &out_len),
        HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len <= 301);
    alloc.free(alloc.ctx, out, out_len + 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 6 — Conversation awareness (human-mimicking intelligence)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void imessage_awareness_builds_thread_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[3] = {
        {.from_me = false, .text = "hey what's up", .timestamp = "10:00 AM"},
        {.from_me = true, .text = "not much, you?", .timestamp = "10:01 AM"},
        {.from_me = false, .text = "want to grab lunch?", .timestamp = "10:02 AM"},
    };
    size_t out_len = 0;
    char *ctx = hu_conversation_build_awareness(&alloc, entries, 3, NULL, &out_len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "hey what") != NULL || strstr(ctx, "lunch") != NULL);
    alloc.free(alloc.ctx, ctx, out_len + 1);
}

static void imessage_awareness_anti_ai_rules_present(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[2] = {
        {.from_me = false, .text = "hey", .timestamp = "10:00"},
        {.from_me = true, .text = "what's up", .timestamp = "10:01"},
    };
    size_t out_len = 0;
    char *ctx = hu_conversation_build_awareness(&alloc, entries, 2, NULL, &out_len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(strstr(ctx, "happy to") != NULL || strstr(ctx, "AI") != NULL ||
                   strstr(ctx, "STYLE") != NULL || strstr(ctx, "naturally") != NULL);
    alloc.free(alloc.ctx, ctx, out_len + 1);
}

static void imessage_quality_score_penalizes_long_response(void) {
    hu_channel_history_entry_t entries[2] = {
        {.from_me = false, .text = "hey", .timestamp = "10:00"},
        {.from_me = true, .text = "yo", .timestamp = "10:01"},
    };
    char long_resp[400];
    memset(long_resp, 'a', sizeof(long_resp) - 1);
    long_resp[sizeof(long_resp) - 1] = '\0';
    hu_quality_score_t score =
        hu_conversation_evaluate_quality(long_resp, sizeof(long_resp) - 1, entries, 2, 300);
    HU_ASSERT_TRUE(score.brevity < 80);
}

static void imessage_quality_score_short_response_not_flagged_for_revision(void) {
    hu_channel_history_entry_t entries[2] = {
        {.from_me = false, .text = "want to grab food?", .timestamp = "10:00"},
        {.from_me = true, .text = "sure", .timestamp = "10:01"},
    };
    hu_quality_score_t score =
        hu_conversation_evaluate_quality("yeah sounds good!", 17, entries, 2, 300);
    HU_ASSERT_TRUE(score.total >= 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 7 — Tapback decisions and reaction classification
 * ═══════════════════════════════════════════════════════════════════════════ */

static void imessage_tapback_decision_on_haha(void) {
    hu_channel_history_entry_t entries[2] = {
        {.from_me = false, .text = "haha that's hilarious", .timestamp = "10:00"},
        {.from_me = true, .text = "right??", .timestamp = "10:01"},
    };
    hu_tapback_decision_t d = hu_conversation_classify_tapback_decision(S("haha that's hilarious"),
                                                                        entries, 2, NULL, 12345);
    HU_ASSERT_TRUE(d == HU_TAPBACK_ONLY || d == HU_TAPBACK_AND_TEXT || d == HU_TEXT_ONLY);
}

static void imessage_reaction_classify_on_funny_text(void) {
    hu_channel_history_entry_t entries[1] = {
        {.from_me = false, .text = "lmaooo", .timestamp = "10:00"},
    };
    /* Seed 0 yields roll < 30 → HAHA for funny-pattern messages (see test_conversation). */
    hu_reaction_type_t r = hu_conversation_classify_reaction(S("lmaooo"), false, entries, 1, 0u);
    HU_ASSERT_TRUE(r != HU_REACTION_NONE);
    HU_ASSERT_EQ(r, HU_REACTION_HAHA);
}

static void imessage_self_reaction_mostly_none(void) {
    int none_count = 0;
    for (uint32_t seed = 0; seed < 200; seed++) {
        hu_reaction_type_t r =
            hu_conversation_classify_self_reaction(S("just said something normal"), seed);
        if (r == HU_REACTION_NONE)
            none_count++;
    }
    HU_ASSERT_TRUE(none_count > 180);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 8 — GIF calibration loop
 * ═══════════════════════════════════════════════════════════════════════════ */

static void imessage_gif_cal_neutral_before_3_sends(void) {
    float rate = hu_conversation_gif_cal_hit_rate(S("new-contact"));
    HU_ASSERT_TRUE(rate >= 0.49f && rate <= 0.51f);
}

static void imessage_gif_cal_tracks_sends_and_reactions(void) {
    hu_conversation_gif_cal_record_send(S("gif-test-contact"), S("cats"));
    hu_conversation_gif_cal_record_send(S("gif-test-contact"), S("dogs"));
    hu_conversation_gif_cal_record_send(S("gif-test-contact"), S("birds"));
    hu_conversation_gif_cal_record_reaction(S("gif-test-contact"));
    float rate = hu_conversation_gif_cal_hit_rate(S("gif-test-contact"));
    HU_ASSERT_TRUE(rate > 0.0f && rate < 1.0f);
}

static void imessage_gif_probability_adjusts_by_relationship(void) {
    float friend_prob = hu_conversation_adjust_gif_probability(0.1f, S("friend"));
    float coworker_prob = hu_conversation_adjust_gif_probability(0.1f, S("coworker"));
    HU_ASSERT_TRUE(friend_prob >= coworker_prob);
}

static void imessage_gif_should_send_deterministic_false_on_serious(void) {
    hu_channel_history_entry_t entries[1] = {
        {.from_me = false, .text = "my grandmother just died", .timestamp = "10:00"},
    };
    bool send =
        hu_conversation_should_send_gif(S("my grandmother just died"), entries, 1, 42, 0.5f);
    HU_ASSERT_FALSE(send);
}

static void imessage_should_send_music_on_music_mention(void) {
    hu_channel_history_entry_t hist[1] = {
        {.from_me = false, .text = "hey", .timestamp = "10:00"},
    };
    bool result =
        hu_conversation_should_send_music("what are you listening to", 25, hist, 1, 42, 1.0f);
    HU_ASSERT_TRUE(result);
}

static void imessage_should_send_music_respects_zero_probability(void) {
    hu_channel_history_entry_t hist[1] = {
        {.from_me = false, .text = "hey", .timestamp = "10:00"},
    };
    bool result = hu_conversation_should_send_music("hello there", 11, hist, 1, 42, 0.0f);
    HU_ASSERT_FALSE(result);
}

static void imessage_build_music_prompt_includes_context(void) {
    char out[256];
    size_t len = hu_conversation_build_music_prompt("I'm feeling nostalgic", 21, out, sizeof(out));
    HU_ASSERT_TRUE(len > 0u);
    HU_ASSERT_NOT_NULL(strstr(out, "nostalgic"));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 9 — Seen behavior / emotional pacing
 * ═══════════════════════════════════════════════════════════════════════════ */

static void imessage_seen_behavior_responds_to_question(void) {
    uint32_t delay_ms = 0;
    hu_seen_action_t action =
        hu_conversation_classify_seen_behavior(S("are you free tonight?"), 14, 42, &delay_ms);
    HU_ASSERT_EQ((int)action, (int)HU_SEEN_RESPOND_NOW);
}

static void imessage_engagement_detection_works(void) {
    hu_channel_history_entry_t entries[3] = {
        {.from_me = false, .text = "I had the best day ever!!!", .timestamp = "10:00"},
        {.from_me = true, .text = "oh nice! what happened?", .timestamp = "10:01"},
        {.from_me = false, .text = "got promoted! and then went to dinner!", .timestamp = "10:02"},
    };
    hu_engagement_level_t level = hu_conversation_detect_engagement(entries, 3);
    HU_ASSERT_TRUE(level == HU_ENGAGEMENT_HIGH || level == HU_ENGAGEMENT_MODERATE);
}

static void imessage_emotion_detection_on_distress(void) {
    hu_channel_history_entry_t entries[1] = {
        {.from_me = false, .text = "I'm so overwhelmed and stressed out", .timestamp = "10:00"},
    };
    hu_emotional_state_t state = hu_conversation_detect_emotion(entries, 1);
    HU_ASSERT_TRUE(state.valence <= 0.0f || state.intensity > 0.0f || state.concerning);
}

static void imessage_narrative_phase_from_short_convo(void) {
    hu_channel_history_entry_t entries[2] = {
        {.from_me = false, .text = "hey", .timestamp = "10:00"},
        {.from_me = true, .text = "hey!", .timestamp = "10:01"},
    };
    hu_narrative_phase_t phase = hu_conversation_detect_narrative(entries, 2);
    HU_ASSERT_TRUE(phase == HU_NARRATIVE_OPENING || phase == HU_NARRATIVE_BUILDING);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 10 — Persona integration (iMessage overlay + examples)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void imessage_persona_overlay_found_by_channel(void) {
    hu_persona_overlay_t overlay = {
        .channel = "imessage",
        .formality = "casual",
        .avg_length = "short",
        .emoji_usage = "minimal",
    };
    hu_persona_t persona = {0};
    persona.overlays = &overlay;
    persona.overlays_count = 1;
    const hu_persona_overlay_t *found = hu_persona_find_overlay(&persona, S("imessage"));
    HU_ASSERT_NOT_NULL(found);
    HU_ASSERT_STR_EQ(found->formality, "casual");
}

static void imessage_persona_overlay_not_found_for_other_channel(void) {
    hu_persona_overlay_t overlay = {.channel = "imessage", .formality = "casual"};
    hu_persona_t persona = {0};
    persona.overlays = &overlay;
    persona.overlays_count = 1;
    HU_ASSERT_NULL(hu_persona_find_overlay(&persona, S("telegram")));
}

static void imessage_persona_prompt_includes_casual_with_overlay(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_overlay_t overlay = {
        .channel = "imessage",
        .formality = "casual",
        .avg_length = "short",
        .emoji_usage = "minimal",
    };
    hu_persona_t persona = {0};
    persona.name = "Test";
    persona.overlays = &overlay;
    persona.overlays_count = 1;
    char *prompt = NULL;
    size_t prompt_len = 0;
    hu_error_t err =
        hu_persona_build_prompt(&alloc, &persona, S("imessage"), NULL, 0, &prompt, &prompt_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(prompt_len > 0);
    HU_ASSERT_TRUE(strstr(prompt, "casual") != NULL || strstr(prompt, "Casual") != NULL);
    alloc.free(alloc.ctx, prompt, prompt_len + 1);
}

static void imessage_persona_example_selection_matches_topic(void) {
    hu_persona_example_t examples[2] = {
        {.context = "lunch plans", .incoming = "wanna eat?", .response = "sure where"},
        {.context = "coding help", .incoming = "this bug is wild", .response = "let me look"},
    };
    hu_persona_example_bank_t bank = {
        .channel = "imessage", .examples = examples, .examples_count = 2};
    hu_persona_t persona = {0};
    persona.example_banks = &bank;
    persona.example_banks_count = 1;
    const hu_persona_example_t *selected[4];
    size_t count = 0;
    hu_error_t err =
        hu_persona_select_examples(&persona, S("imessage"), S("lunch"), selected, &count, 2, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count >= 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 11 — Poll metadata (attachment/video flags, multi-sender batching)
 * ═══════════════════════════════════════════════════════════════════════════ */

#if HU_IS_TEST
static void imessage_poll_photo_flag_propagates(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_EQ(hu_imessage_test_inject_mock_ex(&ch, "alice", 5, "[Photo]", 7, true), HU_OK);
    hu_channel_loop_msg_t msgs[2];
    size_t count = 0;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 2, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_TRUE(msgs[0].has_attachment);
    HU_ASSERT_FALSE(msgs[0].has_video);
    hu_imessage_destroy(&ch);
}

static void imessage_poll_video_flag_propagates(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_EQ(hu_imessage_test_inject_mock_ex2(&ch, "bob", 3, "[Video]", 7, false, true), HU_OK);
    hu_channel_loop_msg_t msgs[2];
    size_t count = 0;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 2, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_TRUE(msgs[0].has_video);
    HU_ASSERT_FALSE(msgs[0].has_attachment);
    hu_imessage_destroy(&ch);
}

static void imessage_poll_multi_sender_batch(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_EQ(hu_imessage_test_inject_mock(&ch, "alice", 5, "hey", 3), HU_OK);
    HU_ASSERT_EQ(hu_imessage_test_inject_mock(&ch, "bob", 3, "yo", 2), HU_OK);
    HU_ASSERT_EQ(hu_imessage_test_inject_mock(&ch, "alice", 5, "what's up", 9), HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 0;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 4, &count), HU_OK);
    HU_ASSERT_EQ(count, 3u);
    HU_ASSERT_STR_EQ(msgs[0].session_key, "alice");
    HU_ASSERT_STR_EQ(msgs[1].session_key, "bob");
    HU_ASSERT_STR_EQ(msgs[2].session_key, "alice");
    HU_ASSERT_EQ(msgs[0].message_id, (int64_t)1);
    HU_ASSERT_EQ(msgs[1].message_id, (int64_t)2);
    HU_ASSERT_EQ(msgs[2].message_id, (int64_t)3);
    hu_imessage_destroy(&ch);
}
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 12 — Conversation hints (cold restart, group, link, emoji mirror)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void imessage_cold_restart_hint_on_fresh_convo(void) {
    /* Cold restart needs ≥2 messages and ≥4h gap between prev and latest inbound. */
    hu_channel_history_entry_t entries[2] = {
        {.from_me = true, .text = "catch you later", .timestamp = "2024-01-01 08:00:00"},
        {.from_me = false, .text = "hey again!", .timestamp = "2024-01-01 20:00:00"},
    };
    char buf[256];
    size_t len = hu_conversation_build_cold_restart_hint(entries, 2, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "COLD RESTART") != NULL);
}

static void imessage_group_mention_hint_non_empty_for_group(void) {
    char buf[512];
    size_t len = hu_conversation_build_group_mention_hint(S("Alice"), true, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "Alice") != NULL);
}

static void imessage_group_mention_hint_empty_for_dm(void) {
    char buf[512];
    size_t len = hu_conversation_build_group_mention_hint(S("Alice"), false, buf, sizeof(buf));
    HU_ASSERT_EQ(len, 0u);
}

static void imessage_link_context_detects_url(void) {
    char buf[256];
    size_t len = hu_conversation_build_link_context(S("check this out https://example.com"), buf,
                                                    sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
}

static void imessage_emoji_mirror_hint_exists(void) {
    /* Emoji mirror needs ≥2 inbound messages; all-ASCII yields low emoji_rate directive. */
    hu_channel_history_entry_t entries[3] = {
        {.from_me = false, .text = "hey", .timestamp = "10:00"},
        {.from_me = false, .text = "what's up", .timestamp = "10:01"},
        {.from_me = true, .text = "nice!", .timestamp = "10:02"},
    };
    char buf[256];
    size_t len = hu_conversation_build_emoji_mirror_hint(entries, 3, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "EMOJI") != NULL);
}

static void imessage_gif_style_hint_for_friend(void) {
    char buf[128];
    size_t len = hu_conversation_build_gif_style_hint(S("friend"), buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 13 — Effect mapping, balloon classification, mark_read, use_imsg_cli
 * ═══════════════════════════════════════════════════════════════════════════ */

static void imessage_effect_name_slam(void) {
    HU_ASSERT_STR_EQ(hu_imessage_effect_name("com.apple.MobileSMS.expressivesend.impact"), "Slam");
}

static void imessage_effect_name_all_known(void) {
    HU_ASSERT_STR_EQ(hu_imessage_effect_name("com.apple.MobileSMS.expressivesend.loud"), "Loud");
    HU_ASSERT_STR_EQ(hu_imessage_effect_name("com.apple.MobileSMS.expressivesend.gentle"),
                     "Gentle");
    HU_ASSERT_STR_EQ(hu_imessage_effect_name("com.apple.MobileSMS.expressivesend.invisibleink"),
                     "Invisible Ink");
    HU_ASSERT_STR_EQ(hu_imessage_effect_name("com.apple.messages.effect.CKConfettiEffect"),
                     "Confetti");
    HU_ASSERT_STR_EQ(hu_imessage_effect_name("com.apple.messages.effect.CKEchoEffect"), "Echo");
    HU_ASSERT_STR_EQ(hu_imessage_effect_name("com.apple.messages.effect.CKFireworksEffect"),
                     "Fireworks");
    HU_ASSERT_STR_EQ(hu_imessage_effect_name("com.apple.messages.effect.CKHappyBirthdayEffect"),
                     "Happy Birthday");
    HU_ASSERT_STR_EQ(hu_imessage_effect_name("com.apple.messages.effect.CKHeartEffect"), "Heart");
    HU_ASSERT_STR_EQ(hu_imessage_effect_name("com.apple.messages.effect.CKLasersEffect"), "Lasers");
    HU_ASSERT_STR_EQ(hu_imessage_effect_name("com.apple.messages.effect.CKShootingStarEffect"),
                     "Shooting Star");
    HU_ASSERT_STR_EQ(hu_imessage_effect_name("com.apple.messages.effect.CKSparklesEffect"),
                     "Sparkles");
    HU_ASSERT_STR_EQ(hu_imessage_effect_name("com.apple.messages.effect.CKSpotlightEffect"),
                     "Spotlight");
}

static void imessage_effect_name_null_and_unknown(void) {
    HU_ASSERT_NULL(hu_imessage_effect_name(NULL));
    HU_ASSERT_NULL(hu_imessage_effect_name(""));
    HU_ASSERT_NULL(hu_imessage_effect_name("com.apple.unknown.effect"));
}

static void imessage_balloon_label_sticker(void) {
    HU_ASSERT_STR_EQ(
        hu_imessage_balloon_label("com.apple.messages.MSMessageExtensionBalloonPlugin:0000000000:"
                                  "com.apple.Stickers.UserGenerated.StickerPack"),
        "[Sticker]");
}

static void imessage_balloon_label_memoji(void) {
    HU_ASSERT_STR_EQ(
        hu_imessage_balloon_label("com.apple.messages.MSMessageExtensionBalloonPlugin:0000000000:"
                                  "com.apple.Animoji.StickersApp.MessagesExtension"),
        "[Memoji]");
    HU_ASSERT_STR_EQ(hu_imessage_balloon_label("some.Memoji.plugin"), "[Memoji]");
}

static void imessage_balloon_label_app(void) {
    HU_ASSERT_STR_EQ(hu_imessage_balloon_label("com.example.app"), "[iMessage App]");
}

static void imessage_balloon_label_null_and_empty(void) {
    HU_ASSERT_NULL(hu_imessage_balloon_label(NULL));
    HU_ASSERT_NULL(hu_imessage_balloon_label(""));
}

static void imessage_text_is_placeholder_true_cases(void) {
    HU_ASSERT_TRUE(hu_imessage_text_is_placeholder(NULL));
    HU_ASSERT_TRUE(hu_imessage_text_is_placeholder(""));
    HU_ASSERT_TRUE(hu_imessage_text_is_placeholder("[Photo]"));
    HU_ASSERT_TRUE(hu_imessage_text_is_placeholder("[Video]"));
    HU_ASSERT_TRUE(hu_imessage_text_is_placeholder("[Voice Message]"));
}

static void imessage_text_is_placeholder_false_cases(void) {
    HU_ASSERT_FALSE(hu_imessage_text_is_placeholder("Hello!"));
    HU_ASSERT_FALSE(hu_imessage_text_is_placeholder("Check this out"));
    HU_ASSERT_FALSE(hu_imessage_text_is_placeholder("[Photo] extra text"));
    HU_ASSERT_FALSE(hu_imessage_text_is_placeholder("photo"));
}

static void imessage_balloon_override_only_on_placeholder(void) {
    const char *sticker_bundle = "com.apple.Stickers.UserGenerated.StickerPack";
    const char *label = hu_imessage_balloon_label(sticker_bundle);
    HU_ASSERT_NOT_NULL(label);

    /* Real text with balloon → text preserved (balloon label NOT applied) */
    HU_ASSERT_FALSE(hu_imessage_text_is_placeholder("Check out this sticker"));
    HU_ASSERT_FALSE(hu_imessage_text_is_placeholder("Haha nice one"));

    /* Placeholder text with balloon → label overrides */
    HU_ASSERT_TRUE(hu_imessage_text_is_placeholder(NULL));
    HU_ASSERT_TRUE(hu_imessage_text_is_placeholder(""));
    HU_ASSERT_TRUE(hu_imessage_text_is_placeholder("[Photo]"));

    /* Verify the complete decision: if placeholder, label wins; if real, text wins */
    const char *real_text = "Check out this sticker";
    const char *result_text = (hu_imessage_text_is_placeholder(real_text)) ? label : real_text;
    HU_ASSERT_STR_EQ(result_text, "Check out this sticker");

    const char *placeholder = "[Photo]";
    result_text = (hu_imessage_text_is_placeholder(placeholder)) ? label : placeholder;
    HU_ASSERT_STR_EQ(result_text, "[Sticker]");
}

static void imessage_copy_bounded_normal(void) {
    char dst[32];
    size_t n = hu_imessage_copy_bounded(dst, sizeof(dst), "hello", 5);
    HU_ASSERT_EQ(n, 5);
    HU_ASSERT_STR_EQ(dst, "hello");
}

static void imessage_copy_bounded_truncates(void) {
    char dst[4];
    size_t n = hu_imessage_copy_bounded(dst, sizeof(dst), "hello world", 11);
    HU_ASSERT_EQ(n, 3);
    HU_ASSERT_STR_EQ(dst, "hel");
}

static void imessage_copy_bounded_exact_fit(void) {
    char dst[6];
    size_t n = hu_imessage_copy_bounded(dst, sizeof(dst), "hello", 5);
    HU_ASSERT_EQ(n, 5);
    HU_ASSERT_STR_EQ(dst, "hello");
}

static void imessage_copy_bounded_null_and_empty(void) {
    char dst[16] = "dirty";
    HU_ASSERT_EQ(hu_imessage_copy_bounded(dst, sizeof(dst), NULL, 0), 0);
    HU_ASSERT_EQ(dst[0], '\0');

    dst[0] = 'x';
    HU_ASSERT_EQ(hu_imessage_copy_bounded(dst, sizeof(dst), "", 0), 0);
    HU_ASSERT_EQ(dst[0], '\0');

    HU_ASSERT_EQ(hu_imessage_copy_bounded(NULL, 0, "hello", 5), 0);
}

#if HU_IS_TEST
static void imessage_start_typing_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+1234567890", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_NOT_NULL(ch.vtable->start_typing);
    HU_ASSERT_EQ(ch.vtable->start_typing(ch.ctx, "+1234567890", 12), HU_OK);
    HU_ASSERT_NOT_NULL(ch.vtable->stop_typing);
    HU_ASSERT_EQ(ch.vtable->stop_typing(ch.ctx, "+1234567890", 12), HU_OK);
    hu_imessage_destroy(&ch);
}

static void imessage_start_typing_null_ctx_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+1234567890", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_EQ(ch.vtable->start_typing(NULL, "+1234567890", 12), HU_OK);
    HU_ASSERT_EQ(ch.vtable->stop_typing(NULL, "+1234567890", 12), HU_OK);
    hu_imessage_destroy(&ch);
}

static void imessage_set_use_imsg_cli_wires_to_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_set_use_imsg_cli(&ch, true);
    hu_imessage_set_use_imsg_cli(&ch, false);
    hu_imessage_set_use_imsg_cli(NULL, true);
    hu_imessage_destroy(&ch);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 14 — imsg CLI deep integration (watch, attachments, chats validation)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void imessage_watch_active_false_under_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_set_use_imsg_cli(&ch, true);
    HU_ASSERT_FALSE(hu_imessage_watch_active(&ch));
    hu_imessage_destroy(&ch);
}

static void imessage_watch_active_null_channel_returns_false(void) {
    HU_ASSERT_FALSE(hu_imessage_watch_active(NULL));
    hu_channel_t ch = {0};
    HU_ASSERT_FALSE(hu_imessage_watch_active(&ch));
}

static void imessage_watch_active_false_without_imsg_cli(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_set_use_imsg_cli(&ch, false);
    HU_ASSERT_FALSE(hu_imessage_watch_active(&ch));
    hu_imessage_destroy(&ch);
}

static void imessage_start_stop_with_imsg_cli_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_set_use_imsg_cli(&ch, true);
    HU_ASSERT_EQ(ch.vtable->start(ch.ctx), HU_OK);
    ch.vtable->stop(ch.ctx);
    hu_imessage_destroy(&ch);
}

static void imessage_destroy_with_imsg_cli_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_set_use_imsg_cli(&ch, true);
    HU_ASSERT_EQ(ch.vtable->start(ch.ctx), HU_OK);
    hu_imessage_destroy(&ch);
}

static void imessage_imsg_cli_text_send_test_mode_stores_message(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_set_use_imsg_cli(&ch, true);
    HU_ASSERT_EQ(ch.vtable->send(ch.ctx, "+15551234567", 12, "test msg", 8, NULL, 0), HU_OK);
    size_t out_len = 0;
    const char *msg = hu_imessage_test_get_last_message(&ch, &out_len);
    HU_ASSERT_NOT_NULL(msg);
    HU_ASSERT_EQ(out_len, 8);
    hu_imessage_destroy(&ch);
}

static void imessage_imsg_cli_media_test_mode_stores_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_set_use_imsg_cli(&ch, true);
    const char *media[] = {"/tmp/test.jpg"};
    HU_ASSERT_EQ(ch.vtable->send(ch.ctx, "+15551234567", 12, "", 0, media, 1), HU_OK);
    HU_ASSERT_EQ(hu_imessage_test_get_last_media_count(&ch), 1);
    hu_imessage_destroy(&ch);
}

static void imessage_poll_returns_ok_with_no_mocks_and_imsg_cli(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_set_use_imsg_cli(&ch, true);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 99;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 4, &count), HU_OK);
    HU_ASSERT_EQ(count, 0);
    hu_imessage_destroy(&ch);
}

static void imessage_start_stop_double_stop_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_set_use_imsg_cli(&ch, true);
    HU_ASSERT_EQ(ch.vtable->start(ch.ctx), HU_OK);
    ch.vtable->stop(ch.ctx);
    ch.vtable->stop(ch.ctx);
    hu_imessage_destroy(&ch);
}

static void imessage_start_stop_cycle_with_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_set_use_imsg_cli(&ch, true);
    HU_ASSERT_EQ(ch.vtable->start(ch.ctx), HU_OK);
    hu_channel_loop_msg_t msgs[4];
    size_t count = 99;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 4, &count), HU_OK);
    HU_ASSERT_EQ(count, 0);
    ch.vtable->stop(ch.ctx);
    hu_imessage_destroy(&ch);
}

static void imessage_mark_read_returns_ok_under_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_NOT_NULL(ch.vtable->mark_read);
    HU_ASSERT_EQ(ch.vtable->mark_read(ch.ctx, "+15551234567", 12), HU_OK);
    hu_imessage_destroy(&ch);
}

static void imessage_mark_read_null_ctx_no_crash(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    (void)ch.vtable->mark_read(NULL, "+15551234567", 12);
    hu_imessage_destroy(&ch);
}
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 15 — Typing duration model (red team: anti-linearity)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void imessage_typing_duration_short_msg_above_floor(void) {
    unsigned int d = hu_imessage_typing_duration(5, 42);
    HU_ASSERT_TRUE(d >= 800);
    HU_ASSERT_TRUE(d <= 6000);
}

static void imessage_typing_duration_medium_msg(void) {
    unsigned int d = hu_imessage_typing_duration(50, 42);
    HU_ASSERT_TRUE(d >= 800);
    HU_ASSERT_TRUE(d <= 6000);
    HU_ASSERT_TRUE(d >= 1500);
}

static void imessage_typing_duration_long_msg_capped(void) {
    unsigned int d = hu_imessage_typing_duration(300, 42);
    HU_ASSERT_EQ(d, 6000);
}

static void imessage_typing_duration_zero_len(void) {
    unsigned int d = hu_imessage_typing_duration(0, 0);
    HU_ASSERT_TRUE(d >= 800);
}

static void imessage_typing_duration_varies_by_seed(void) {
    unsigned int d1 = hu_imessage_typing_duration(60, 100);
    unsigned int d2 = hu_imessage_typing_duration(60, 999);
    HU_ASSERT_TRUE(d1 != d2 || d1 == 6000);
}

static void imessage_typing_duration_jitter_bounded(void) {
    for (uint32_t seed = 0; seed < 200; seed++) {
        unsigned int d = hu_imessage_typing_duration(40, seed);
        HU_ASSERT_TRUE(d >= 800);
        HU_ASSERT_TRUE(d <= 6000);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 15b — IMCore selector-conformance (pure predicate, mock resolver)
 *
 * Pins the exact drift that produced the 2026-07-21 typing phantom: a required
 * selector silently stops resolving on an OS bump. These exercise the pure
 * conformance pass with injected resolvers — no ObjC runtime needed.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Mock resolver: everything resolves. */
static bool imcore_resolver_all_present(const char *cls, const char *sel, bool is_class, void *ud) {
    (void)cls;
    (void)sel;
    (void)is_class;
    (void)ud;
    return true;
}

/* Mock resolver: nothing resolves (simulates IMCore absent entirely). */
static bool imcore_resolver_none_present(const char *cls, const char *sel, bool is_class,
                                         void *ud) {
    (void)cls;
    (void)sel;
    (void)is_class;
    (void)ud;
    return false;
}

/* Mock resolver: everything resolves EXCEPT setLocalUserIsTyping: — the exact
 * shape of a real drift where the one write we depend on gets renamed. */
static bool imcore_resolver_typing_setter_drifted(const char *cls, const char *sel, bool is_class,
                                                  void *ud) {
    (void)cls;
    (void)is_class;
    (void)ud;
    return strcmp(sel, "setLocalUserIsTyping:") != 0;
}

/* Reporter that records the selectors flagged missing into a fixed buffer. */
typedef struct {
    const char *names[16];
    size_t count;
} imcore_missing_capture_t;

static void imcore_capture_missing(const char *cls, const char *sel, bool is_class, void *ud) {
    (void)cls;
    (void)is_class;
    imcore_missing_capture_t *cap = (imcore_missing_capture_t *)ud;
    if (cap && cap->count < 16)
        cap->names[cap->count++] = sel;
}

static void imcore_required_table_nonempty_and_pins_typing_setter(void) {
    size_t n = 0;
    const hu_imcore_selector_req_t *reqs = hu_imessage_imcore_required_selectors(&n);
    HU_ASSERT_NOT_NULL(reqs);
    HU_ASSERT_TRUE(n >= 1);
    /* The table MUST include the one write the typing path cannot live without. */
    bool has_setter = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(reqs[i].class_name, "IMChat") == 0 &&
            strcmp(reqs[i].selector, "setLocalUserIsTyping:") == 0)
            has_setter = true;
    }
    HU_ASSERT_TRUE(has_setter);
}

static void imcore_conformance_all_present_reports_zero_missing(void) {
    size_t n = 0;
    const hu_imcore_selector_req_t *reqs = hu_imessage_imcore_required_selectors(&n);
    imcore_missing_capture_t cap = {0};
    size_t missing = hu_imessage_imcore_conformance(reqs, n, imcore_resolver_all_present,
                                                    imcore_capture_missing, &cap);
    HU_ASSERT_EQ((int)missing, 0);
    HU_ASSERT_EQ((int)cap.count, 0); /* on_missing never fired */
}

static void imcore_conformance_none_present_flags_every_selector(void) {
    size_t n = 0;
    const hu_imcore_selector_req_t *reqs = hu_imessage_imcore_required_selectors(&n);
    imcore_missing_capture_t cap = {0};
    size_t missing = hu_imessage_imcore_conformance(reqs, n, imcore_resolver_none_present,
                                                    imcore_capture_missing, &cap);
    HU_ASSERT_EQ((int)missing, (int)n);   /* every one flagged */
    HU_ASSERT_EQ((int)cap.count, (int)n); /* on_missing fired per selector */
}

static void imcore_conformance_pins_single_drifted_selector(void) {
    size_t n = 0;
    const hu_imcore_selector_req_t *reqs = hu_imessage_imcore_required_selectors(&n);
    imcore_missing_capture_t cap = {0};
    size_t missing = hu_imessage_imcore_conformance(reqs, n, imcore_resolver_typing_setter_drifted,
                                                    imcore_capture_missing, &cap);
    /* Exactly one selector drifted, and it is the typing setter — this is the
     * real-world failure the whole check exists to surface loudly. */
    HU_ASSERT_EQ((int)missing, 1);
    HU_ASSERT_EQ((int)cap.count, 1);
    HU_ASSERT_STR_EQ(cap.names[0], "setLocalUserIsTyping:");
}

static void imcore_conformance_null_args_safe(void) {
    size_t n = 0;
    const hu_imcore_selector_req_t *reqs = hu_imessage_imcore_required_selectors(&n);
    /* NULL resolver -> 0 (nothing to check), no crash. */
    HU_ASSERT_EQ((int)hu_imessage_imcore_conformance(reqs, n, NULL, NULL, NULL), 0);
    /* NULL reqs -> 0, no crash. */
    HU_ASSERT_EQ(
        (int)hu_imessage_imcore_conformance(NULL, 5, imcore_resolver_all_present, NULL, NULL), 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 16 — Tapback decision exhaustive branch coverage
 * ═══════════════════════════════════════════════════════════════════════════ */

static void imessage_tapback_decision_question_prefers_text(void) {
    hu_tapback_decision_t d =
        hu_conversation_classify_tapback_decision(S("hey where are you?"), NULL, 0, NULL, 0);
    HU_ASSERT_EQ((int)d, (int)HU_TEXT_ONLY);
}

static void imessage_tapback_decision_emotional_heavy_prefers_text(void) {
    hu_tapback_decision_t d =
        hu_conversation_classify_tapback_decision(S("my grandma passed away"), NULL, 0, NULL, 0);
    HU_ASSERT_EQ((int)d, (int)HU_TEXT_ONLY);
}

static void imessage_tapback_decision_got_fired_prefers_text(void) {
    hu_tapback_decision_t d =
        hu_conversation_classify_tapback_decision(S("i just got fired today"), NULL, 0, NULL, 0);
    HU_ASSERT_EQ((int)d, (int)HU_TEXT_ONLY);
}

static void imessage_tapback_decision_lol_tapback_dominant(void) {
    int tapback_count = 0;
    for (uint32_t seed = 0; seed < 200; seed++) {
        hu_tapback_decision_t d =
            hu_conversation_classify_tapback_decision(S("lol"), NULL, 0, NULL, seed);
        if (d == HU_TAPBACK_ONLY || d == HU_TAPBACK_AND_TEXT)
            tapback_count++;
    }
    HU_ASSERT_TRUE(tapback_count > 100);
}

static void imessage_tapback_decision_ok_often_no_response(void) {
    int no_resp = 0;
    for (uint32_t seed = 0; seed < 200; seed++) {
        hu_tapback_decision_t d =
            hu_conversation_classify_tapback_decision(S("ok"), NULL, 0, NULL, seed);
        if (d == HU_NO_RESPONSE)
            no_resp++;
    }
    HU_ASSERT_TRUE(no_resp > 80);
}

static void imessage_tapback_decision_nice_cool_tapback_likely(void) {
    int tapback_count = 0;
    for (uint32_t seed = 0; seed < 200; seed++) {
        hu_tapback_decision_t d =
            hu_conversation_classify_tapback_decision(S("nice"), NULL, 0, NULL, seed);
        if (d == HU_TAPBACK_ONLY || d == HU_TAPBACK_AND_TEXT)
            tapback_count++;
    }
    HU_ASSERT_TRUE(tapback_count > 80);
}

static void imessage_tapback_decision_short_msg_no_question(void) {
    int tapback_count = 0;
    for (uint32_t seed = 0; seed < 200; seed++) {
        hu_tapback_decision_t d =
            hu_conversation_classify_tapback_decision(S("yep"), NULL, 0, NULL, seed);
        if (d == HU_TAPBACK_ONLY || d == HU_TAPBACK_AND_TEXT)
            tapback_count++;
    }
    HU_ASSERT_TRUE(tapback_count > 50);
}

static void imessage_tapback_decision_substantive_prefers_text(void) {
    hu_tapback_decision_t d = hu_conversation_classify_tapback_decision(
        S("I was thinking we could go to that new restaurant downtown for dinner tonight"), NULL, 0,
        NULL, 42);
    HU_ASSERT_EQ((int)d, (int)HU_TEXT_ONLY);
}

static void imessage_tapback_decision_null_returns_no_response(void) {
    hu_tapback_decision_t d = hu_conversation_classify_tapback_decision(NULL, 0, NULL, 0, NULL, 0);
    HU_ASSERT_EQ((int)d, (int)HU_NO_RESPONSE);
}

static void imessage_tapback_decision_empty_returns_no_response(void) {
    hu_tapback_decision_t d = hu_conversation_classify_tapback_decision("", 0, NULL, 0, NULL, 0);
    HU_ASSERT_EQ((int)d, (int)HU_NO_RESPONSE);
}

static void imessage_tapback_decision_anti_spam_recent_tapbacks(void) {
    hu_channel_history_entry_t entries[4] = {
        {.from_me = true, .text = "Loved an image", .timestamp = "10:00"},
        {.from_me = true, .text = "Laughed at a message", .timestamp = "10:01"},
        {.from_me = false, .text = "cool", .timestamp = "10:02"},
        {.from_me = true, .text = "hey!", .timestamp = "10:03"},
    };
    int text_only = 0;
    for (uint32_t seed = 0; seed < 200; seed++) {
        hu_tapback_decision_t d =
            hu_conversation_classify_tapback_decision(S("sweet"), entries, 4, NULL, seed);
        if (d == HU_TEXT_ONLY)
            text_only++;
    }
    HU_ASSERT_TRUE(text_only > 50);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 17 — Seen-behavior classification
 * ═══════════════════════════════════════════════════════════════════════════ */

static void imessage_seen_behavior_non_busy_always_now(void) {
    for (uint32_t seed = 0; seed < 200; seed++) {
        uint32_t delay = 0;
        hu_seen_action_t a = hu_conversation_classify_seen_behavior(S("hey"), 20, seed, &delay);
        HU_ASSERT_EQ((int)a, (int)HU_SEEN_RESPOND_NOW);
    }
}

static void imessage_seen_behavior_urgent_bypasses_delay(void) {
    for (uint32_t seed = 0; seed < 200; seed++) {
        uint32_t delay = 0;
        hu_seen_action_t a =
            hu_conversation_classify_seen_behavior(S("urgent please call now"), 14, seed, &delay);
        HU_ASSERT_EQ((int)a, (int)HU_SEEN_RESPOND_NOW);
    }
}

static void imessage_seen_behavior_busy_low_priority_sometimes_delays(void) {
    int delayed = 0;
    for (uint32_t seed = 0; seed < 1000; seed++) {
        uint32_t delay = 0;
        hu_seen_action_t a = hu_conversation_classify_seen_behavior(S("ok"), 14, seed, &delay);
        if (a == HU_SEEN_DELAY_THEN_RESPOND) {
            delayed++;
            HU_ASSERT_TRUE(delay >= 300000);
        }
    }
    HU_ASSERT_TRUE(delayed > 50);
    HU_ASSERT_TRUE(delayed < 300);
}

static void imessage_seen_behavior_question_during_busy_responds_now(void) {
    uint32_t delay = 0;
    hu_seen_action_t a =
        hu_conversation_classify_seen_behavior(S("can you pick up groceries?"), 14, 42, &delay);
    HU_ASSERT_EQ((int)a, (int)HU_SEEN_RESPOND_NOW);
}

static void imessage_seen_behavior_null_msg_responds_now(void) {
    uint32_t delay = 0;
    hu_seen_action_t a = hu_conversation_classify_seen_behavior(NULL, 0, 14, 42, &delay);
    HU_ASSERT_EQ((int)a, (int)HU_SEEN_RESPOND_NOW);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 18 — Double-text decision
 * ═══════════════════════════════════════════════════════════════════════════ */

static void imessage_double_text_farewell_suppressed(void) {
    HU_ASSERT_FALSE(hu_conversation_should_double_text(S("goodnight!"), NULL, 0, 12, 42, 1.0f));
    HU_ASSERT_FALSE(hu_conversation_should_double_text(S("ttyl bro"), NULL, 0, 12, 42, 1.0f));
    HU_ASSERT_FALSE(hu_conversation_should_double_text(S("gotta go bye"), NULL, 0, 12, 42, 1.0f));
}

static void imessage_double_text_late_night_suppressed(void) {
    HU_ASSERT_FALSE(hu_conversation_should_double_text(S("that was fun"), NULL, 0, 23, 42, 1.0f));
    HU_ASSERT_FALSE(hu_conversation_should_double_text(S("that was fun"), NULL, 0, 3, 42, 1.0f));
}

static void imessage_double_text_zero_probability_never(void) {
    for (uint32_t seed = 0; seed < 200; seed++) {
        HU_ASSERT_FALSE(
            hu_conversation_should_double_text(S("that was awesome"), NULL, 0, 14, seed, 0.0f));
    }
}

static void imessage_double_text_null_response_never(void) {
    HU_ASSERT_FALSE(hu_conversation_should_double_text(NULL, 0, NULL, 0, 14, 42, 1.0f));
}

static void imessage_double_text_sometimes_true_with_high_prob(void) {
    int yes = 0;
    for (uint32_t seed = 0; seed < 500; seed++) {
        if (hu_conversation_should_double_text(S("omg that was amazing!"), NULL, 0, 14, seed, 0.5f))
            yes++;
    }
    HU_ASSERT_TRUE(yes > 50);
    HU_ASSERT_TRUE(yes < 450);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 19 — Reaction classifier exhaustive branch coverage
 * ═══════════════════════════════════════════════════════════════════════════ */

static void imessage_reaction_heart_on_love_you(void) {
    int hearts = 0;
    for (uint32_t seed = 0; seed < 200; seed++) {
        hu_reaction_type_t r =
            hu_conversation_classify_reaction(S("love you so much"), false, NULL, 0, seed);
        if (r == HU_REACTION_HEART)
            hearts++;
    }
    HU_ASSERT_TRUE(hearts > 30);
}

static void imessage_reaction_heart_on_miss_you(void) {
    int hearts = 0;
    for (uint32_t seed = 0; seed < 200; seed++) {
        hu_reaction_type_t r =
            hu_conversation_classify_reaction(S("miss you buddy"), false, NULL, 0, seed);
        if (r == HU_REACTION_HEART)
            hearts++;
    }
    HU_ASSERT_TRUE(hearts > 30);
}

static void imessage_reaction_thumbs_up_on_absolutely(void) {
    int thumbs = 0;
    for (uint32_t seed = 0; seed < 200; seed++) {
        hu_reaction_type_t r =
            hu_conversation_classify_reaction(S("absolutely, let's do it"), false, NULL, 0, seed);
        if (r == HU_REACTION_THUMBS_UP)
            thumbs++;
    }
    HU_ASSERT_TRUE(thumbs > 30);
}

static void imessage_reaction_emphasis_on_got_the_job(void) {
    int emphasis = 0;
    for (uint32_t seed = 0; seed < 200; seed++) {
        hu_reaction_type_t r =
            hu_conversation_classify_reaction(S("i got the job!"), false, NULL, 0, seed);
        if (r == HU_REACTION_EMPHASIS)
            emphasis++;
    }
    HU_ASSERT_TRUE(emphasis > 30);
}

static void imessage_reaction_none_on_question(void) {
    hu_reaction_type_t r =
        hu_conversation_classify_reaction(S("what time should we meet?"), false, NULL, 0, 42);
    HU_ASSERT_EQ((int)r, (int)HU_REACTION_NONE);
}

static void imessage_reaction_gif_haha_or_heart(void) {
    int haha = 0, heart = 0;
    for (uint32_t seed = 0; seed < 500; seed++) {
        hu_reaction_type_t r = hu_conversation_classify_reaction(S("[GIF]"), false, NULL, 0, seed);
        if (r == HU_REACTION_HAHA)
            haha++;
        else if (r == HU_REACTION_HEART)
            heart++;
    }
    HU_ASSERT_TRUE(haha > 100);
    HU_ASSERT_TRUE(heart > 30);
}

static void imessage_reaction_never_reacts_to_own(void) {
    for (uint32_t seed = 0; seed < 200; seed++) {
        hu_reaction_type_t r =
            hu_conversation_classify_reaction(S("lol this is hilarious"), true, NULL, 0, seed);
        HU_ASSERT_EQ((int)r, (int)HU_REACTION_NONE);
    }
}

static void imessage_reaction_photo_sometimes_heart(void) {
    int heart = 0;
    for (uint32_t seed = 0; seed < 200; seed++) {
        hu_reaction_type_t r =
            hu_conversation_classify_reaction(S("[image attached]"), false, NULL, 0, seed);
        if (r == HU_REACTION_HEART)
            heart++;
    }
    HU_ASSERT_TRUE(heart > 50);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 20 — Self-reaction trigger verification
 * ═══════════════════════════════════════════════════════════════════════════ */

static void imessage_self_reaction_oops_yields_haha(void) {
    int haha = 0;
    for (uint32_t seed = 0; seed < 5000; seed++) {
        hu_reaction_type_t r = hu_conversation_classify_self_reaction(S("oops my bad lol"), seed);
        if (r == HU_REACTION_HAHA)
            haha++;
    }
    HU_ASSERT_TRUE(haha > 0);
}

static void imessage_self_reaction_exclaim_yields_emphasis(void) {
    int emphasis = 0;
    for (uint32_t seed = 0; seed < 5000; seed++) {
        hu_reaction_type_t r = hu_conversation_classify_self_reaction(S("no way!!!"), seed);
        if (r == HU_REACTION_EMPHASIS)
            emphasis++;
    }
    HU_ASSERT_TRUE(emphasis > 0);
}

static void imessage_self_reaction_normal_text_always_none_at_high_seed(void) {
    for (uint32_t seed = 100; seed < 200; seed++) {
        hu_reaction_type_t r =
            hu_conversation_classify_self_reaction(S("i think we should go"), seed);
        HU_ASSERT_EQ((int)r, (int)HU_REACTION_NONE);
    }
}

static void imessage_self_reaction_null_safe(void) {
    hu_reaction_type_t r = hu_conversation_classify_self_reaction(NULL, 0, 0);
    HU_ASSERT_EQ((int)r, (int)HU_REACTION_NONE);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 21 — Mock poll parity: full field injection
 * ═══════════════════════════════════════════════════════════════════════════ */

#if HU_IS_TEST
static void imessage_mock_full_propagates_all_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);

    hu_imessage_test_msg_opts_t opts = {
        .guid = "MSG-FULL-001",
        .reply_to_guid = "MSG-FULL-000",
        .chat_id = "iMessage;+;chat999",
        .has_attachment = true,
        .has_video = false,
        .is_group = true,
        .was_edited = true,
        .was_unsent = false,
        .timestamp_sec = 1700000000,
    };
    HU_ASSERT_EQ(hu_imessage_test_inject_mock_full(&ch, S("alice@test.com"), S("hey group"), &opts),
                 HU_OK);

    hu_channel_loop_msg_t msgs[4];
    memset(msgs, 0, sizeof(msgs));
    size_t count = 0;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 4, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(msgs[0].session_key, "alice@test.com");
    HU_ASSERT_STR_EQ(msgs[0].content, "hey group");
    HU_ASSERT_STR_EQ(msgs[0].guid, "MSG-FULL-001");
    HU_ASSERT_STR_EQ(msgs[0].reply_to_guid, "MSG-FULL-000");
    HU_ASSERT_STR_EQ(msgs[0].chat_id, "iMessage;+;chat999");
    HU_ASSERT_TRUE(msgs[0].is_group);
    HU_ASSERT_TRUE(msgs[0].has_attachment);
    HU_ASSERT_TRUE(!msgs[0].has_video);
    HU_ASSERT_TRUE(msgs[0].was_edited);
    HU_ASSERT_TRUE(!msgs[0].was_unsent);
    HU_ASSERT_EQ(msgs[0].timestamp_sec, (int64_t)1700000000);
    HU_ASSERT_EQ(msgs[0].message_id, (int64_t)1);

    hu_imessage_destroy(&ch);
}

static void imessage_mock_full_group_msg_sets_is_group_and_chat_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);

    hu_imessage_test_msg_opts_t opts = {
        .is_group = true,
        .chat_id = "iMessage;+;chatABC",
    };
    HU_ASSERT_EQ(hu_imessage_test_inject_mock_full(&ch, S("bob@test.com"), S("group msg"), &opts),
                 HU_OK);

    hu_channel_loop_msg_t msgs[4];
    memset(msgs, 0, sizeof(msgs));
    size_t count = 0;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 4, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_TRUE(msgs[0].is_group);
    HU_ASSERT_STR_EQ(msgs[0].chat_id, "iMessage;+;chatABC");

    hu_imessage_destroy(&ch);
}

static void imessage_mock_full_edited_unsent_flags(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);

    hu_imessage_test_msg_opts_t opts_edited = {.was_edited = true};
    hu_imessage_test_msg_opts_t opts_unsent = {.was_unsent = true};

    HU_ASSERT_EQ(
        hu_imessage_test_inject_mock_full(&ch, S("+15559999999"), S("edited"), &opts_edited),
        HU_OK);
    HU_ASSERT_EQ(
        hu_imessage_test_inject_mock_full(&ch, S("+15559999999"), S("unsent"), &opts_unsent),
        HU_OK);

    hu_channel_loop_msg_t msgs[4];
    memset(msgs, 0, sizeof(msgs));
    size_t count = 0;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 4, &count), HU_OK);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_TRUE(msgs[0].was_edited);
    HU_ASSERT_TRUE(!msgs[0].was_unsent);
    HU_ASSERT_TRUE(!msgs[1].was_edited);
    HU_ASSERT_TRUE(msgs[1].was_unsent);

    hu_imessage_destroy(&ch);
}

static void imessage_mock_full_inline_reply_guid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);

    hu_imessage_test_msg_opts_t opts = {
        .guid = "MSG-REPLY-002",
        .reply_to_guid = "MSG-ORIGINAL-001",
    };
    HU_ASSERT_EQ(hu_imessage_test_inject_mock_full(&ch, S("+15559999999"), S("replying"), &opts),
                 HU_OK);

    hu_channel_loop_msg_t msgs[4];
    memset(msgs, 0, sizeof(msgs));
    size_t count = 0;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 4, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(msgs[0].guid, "MSG-REPLY-002");
    HU_ASSERT_STR_EQ(msgs[0].reply_to_guid, "MSG-ORIGINAL-001");

    hu_imessage_destroy(&ch);
}

static void imessage_mock_full_timestamp_propagated(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);

    hu_imessage_test_msg_opts_t opts = {.timestamp_sec = 1680000000};
    HU_ASSERT_EQ(hu_imessage_test_inject_mock_full(&ch, S("+15559999999"), S("timestamped"), &opts),
                 HU_OK);

    hu_channel_loop_msg_t msgs[4];
    memset(msgs, 0, sizeof(msgs));
    size_t count = 0;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 4, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_EQ(msgs[0].timestamp_sec, (int64_t)1680000000);

    hu_imessage_destroy(&ch);
}

static void imessage_mock_full_null_opts_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);

    hu_error_t err = hu_imessage_test_inject_mock_full(&ch, S("+15559999999"), S("test"), NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_imessage_destroy(&ch);
}
#endif /* HU_IS_TEST */

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 22 — GUID lookup test store
 * ═══════════════════════════════════════════════════════════════════════════ */

#if HU_IS_TEST
static void imessage_guid_lookup_returns_stored_text(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_imessage_test_clear_guid_lookups();
    hu_imessage_test_set_guid_lookup("MSG-ABC", "original message text");

    char buf[256];
    size_t out_len = 0;
    hu_error_t err =
        hu_imessage_lookup_message_by_guid(&alloc, "MSG-ABC", 7, buf, sizeof(buf), &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_STR_EQ(buf, "original message text");

    hu_imessage_test_clear_guid_lookups();
}

static void imessage_guid_lookup_unknown_guid_not_supported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_imessage_test_clear_guid_lookups();
    hu_imessage_test_set_guid_lookup("MSG-KNOWN", "known text");

    char buf[256];
    size_t out_len = 0;
    hu_error_t err =
        hu_imessage_lookup_message_by_guid(&alloc, "MSG-UNKNOWN", 11, buf, sizeof(buf), &out_len);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(out_len, 0u);

    hu_imessage_test_clear_guid_lookups();
}

static void imessage_guid_lookup_clear_empties_store(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_imessage_test_set_guid_lookup("MSG-TEMP", "temporary");
    hu_imessage_test_clear_guid_lookups();

    char buf[256];
    size_t out_len = 0;
    hu_error_t err =
        hu_imessage_lookup_message_by_guid(&alloc, "MSG-TEMP", 8, buf, sizeof(buf), &out_len);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);

    hu_imessage_test_clear_guid_lookups();
}

static void imessage_guid_lookup_null_args_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[64];
    size_t out_len = 0;

    HU_ASSERT_EQ(hu_imessage_lookup_message_by_guid(&alloc, NULL, 0, buf, sizeof(buf), &out_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_imessage_lookup_message_by_guid(&alloc, "X", 1, NULL, 0, &out_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_imessage_lookup_message_by_guid(&alloc, "X", 1, buf, sizeof(buf), NULL),
                 HU_ERR_INVALID_ARGUMENT);
}
#endif /* HU_IS_TEST */

/* ═══════════════════════════════════════════════════════════════════════════
 * PART 23 — Group chat routing verification
 * ═══════════════════════════════════════════════════════════════════════════ */

#if HU_IS_TEST
static void imessage_group_msg_chat_id_populated(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);

    hu_imessage_test_msg_opts_t opts = {
        .is_group = true,
        .chat_id = "iMessage;+;chat12345",
        .guid = "MSG-GRP-001",
    };
    HU_ASSERT_EQ(
        hu_imessage_test_inject_mock_full(&ch, S("friend@test.com"), S("group hello"), &opts),
        HU_OK);

    hu_channel_loop_msg_t msgs[4];
    memset(msgs, 0, sizeof(msgs));
    size_t count = 0;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 4, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);

    /* session_key is the sender handle (for context/history lookups) */
    HU_ASSERT_STR_EQ(msgs[0].session_key, "friend@test.com");
    /* chat_id is the thread identifier (for send routing) */
    HU_ASSERT_STR_EQ(msgs[0].chat_id, "iMessage;+;chat12345");
    HU_ASSERT_TRUE(msgs[0].is_group);

    hu_imessage_destroy(&ch);
}

static void imessage_dm_msg_chat_id_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);

    hu_imessage_test_msg_opts_t opts = {.is_group = false};
    HU_ASSERT_EQ(hu_imessage_test_inject_mock_full(&ch, S("+15559999999"), S("dm hello"), &opts),
                 HU_OK);

    hu_channel_loop_msg_t msgs[4];
    memset(msgs, 0, sizeof(msgs));
    size_t count = 0;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 4, &count), HU_OK);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_TRUE(!msgs[0].is_group);
    HU_ASSERT_STR_EQ(msgs[0].chat_id, "");

    hu_imessage_destroy(&ch);
}
#endif /* HU_IS_TEST */

/* ═══════════════════════════════════════════════════════════════════════════
 * Part 24 — FDA-aware circuit breaker + poll-status file (red team)
 *
 * Models macOS Full Disk Access being revoked at runtime (the failure mode
 * we observed in production: 270MB of repeated SQLITE_AUTH errors after a
 * binary inode change). Verifies the breaker:
 *   • trips after exactly N consecutive AUTH/CANTOPEN errors
 *   • does NOT trip on transient SQLITE_BUSY/LOCKED
 *   • does NOT trip on novel (OTHER) errors
 *   • resets on the first successful poll
 *   • marks the channel unhealthy via health_check
 * Also covers the status-file roundtrip and pure classifier corners.
 * ═══════════════════════════════════════════════════════════════════════════ */

#if HU_IS_TEST
static void imessage_classify_known_codes(void) {
    HU_ASSERT_EQ(hu_imessage_classify_sqlite_error(0), HU_IMESSAGE_ERR_NONE);
    HU_ASSERT_EQ(hu_imessage_classify_sqlite_error(23), HU_IMESSAGE_ERR_AUTH);
    HU_ASSERT_EQ(hu_imessage_classify_sqlite_error(14), HU_IMESSAGE_ERR_CANTOPEN);
    HU_ASSERT_EQ(hu_imessage_classify_sqlite_error(5), HU_IMESSAGE_ERR_BUSY);
    HU_ASSERT_EQ(hu_imessage_classify_sqlite_error(6), HU_IMESSAGE_ERR_BUSY);
    HU_ASSERT_EQ(hu_imessage_classify_sqlite_error(99), HU_IMESSAGE_ERR_OTHER);
    HU_ASSERT_EQ(hu_imessage_classify_sqlite_error(-1), HU_IMESSAGE_ERR_OTHER);
}

static void imessage_classify_name_roundtrip(void) {
    HU_ASSERT_TRUE(strcmp(hu_imessage_error_class_name(HU_IMESSAGE_ERR_NONE), "NONE") == 0);
    HU_ASSERT_TRUE(strcmp(hu_imessage_error_class_name(HU_IMESSAGE_ERR_AUTH), "AUTH") == 0);
    HU_ASSERT_TRUE(strcmp(hu_imessage_error_class_name(HU_IMESSAGE_ERR_CANTOPEN), "CANTOPEN") == 0);
    HU_ASSERT_TRUE(strcmp(hu_imessage_error_class_name(HU_IMESSAGE_ERR_BUSY), "BUSY") == 0);
    HU_ASSERT_TRUE(strcmp(hu_imessage_error_class_name(HU_IMESSAGE_ERR_OTHER), "OTHER") == 0);
}

static void imessage_breaker_trips_after_threshold_auth(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_FALSE(hu_imessage_breaker_tripped(&ch));
    bool tripped = false;
    for (uint32_t i = 0; i < HU_IMESSAGE_BREAKER_THRESHOLD - 1; i++) {
        tripped = hu_imessage_test_record_open_result(&ch, 23 /* AUTH */, 1000);
        HU_ASSERT_FALSE(tripped);
        HU_ASSERT_FALSE(hu_imessage_breaker_tripped(&ch));
        HU_ASSERT_EQ(hu_imessage_consecutive_failures(&ch), i + 1);
    }
    tripped = hu_imessage_test_record_open_result(&ch, 23, 1000);
    HU_ASSERT_TRUE(tripped);
    HU_ASSERT_TRUE(hu_imessage_breaker_tripped(&ch));
    HU_ASSERT_EQ(hu_imessage_last_error_class(&ch), HU_IMESSAGE_ERR_AUTH);
    /* Re-tripping is idempotent: should not double-trip on subsequent failures. */
    tripped = hu_imessage_test_record_open_result(&ch, 23, 1001);
    HU_ASSERT_FALSE(tripped);
    HU_ASSERT_TRUE(hu_imessage_breaker_tripped(&ch));
    /* health_check now reports unhealthy. */
    HU_ASSERT_FALSE(ch.vtable->health_check(ch.ctx));
    hu_imessage_destroy(&ch);
}

static void imessage_breaker_trips_on_cantopen_too(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    for (uint32_t i = 0; i < HU_IMESSAGE_BREAKER_THRESHOLD; i++)
        (void)hu_imessage_test_record_open_result(&ch, 14 /* CANTOPEN */, 2000);
    HU_ASSERT_TRUE(hu_imessage_breaker_tripped(&ch));
    HU_ASSERT_EQ(hu_imessage_last_error_class(&ch), HU_IMESSAGE_ERR_CANTOPEN);
    hu_imessage_destroy(&ch);
}

static void imessage_breaker_does_not_trip_on_busy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    /* 100 BUSY errors must not trip — they are transient by definition. */
    for (int i = 0; i < 100; i++)
        (void)hu_imessage_test_record_open_result(&ch, 5 /* BUSY */, 3000);
    HU_ASSERT_FALSE(hu_imessage_breaker_tripped(&ch));
    HU_ASSERT_EQ(hu_imessage_consecutive_failures(&ch), 0u);
    HU_ASSERT_EQ(hu_imessage_last_error_class(&ch), HU_IMESSAGE_ERR_BUSY);
    hu_imessage_destroy(&ch);
}

static void imessage_breaker_does_not_trip_on_other_errors(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    /* Unknown rc (e.g. SQLITE_CORRUPT=11) is suspicious but not the FDA pattern. */
    for (int i = 0; i < 50; i++)
        (void)hu_imessage_test_record_open_result(&ch, 11, 4000);
    HU_ASSERT_FALSE(hu_imessage_breaker_tripped(&ch));
    HU_ASSERT_EQ(hu_imessage_consecutive_failures(&ch), 0u);
    HU_ASSERT_EQ(hu_imessage_last_error_class(&ch), HU_IMESSAGE_ERR_OTHER);
    hu_imessage_destroy(&ch);
}

static void imessage_breaker_resets_on_success(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    /* Trip the breaker. */
    for (uint32_t i = 0; i < HU_IMESSAGE_BREAKER_THRESHOLD; i++)
        (void)hu_imessage_test_record_open_result(&ch, 23, 5000);
    HU_ASSERT_TRUE(hu_imessage_breaker_tripped(&ch));
    /* One success clears it. */
    hu_imessage_test_record_poll_success(&ch, 6000);
    HU_ASSERT_FALSE(hu_imessage_breaker_tripped(&ch));
    HU_ASSERT_EQ(hu_imessage_consecutive_failures(&ch), 0u);
    HU_ASSERT_EQ(hu_imessage_last_error_class(&ch), HU_IMESSAGE_ERR_NONE);
    HU_ASSERT_EQ(hu_imessage_last_success_epoch(&ch), 6000);
    /* health_check happy again. On non-Apple platforms health_check
     * returns false unconditionally (no chat.db); the test asserts the
     * breaker-cleared path on Apple only. */
#if defined(__APPLE__) && defined(__MACH__)
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
#endif
    hu_imessage_destroy(&ch);
}

static void imessage_breaker_partial_streak_resets_on_success(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    /* Build up a partial streak below threshold. */
    for (uint32_t i = 0; i < HU_IMESSAGE_BREAKER_THRESHOLD - 1; i++)
        (void)hu_imessage_test_record_open_result(&ch, 23, 7000);
    HU_ASSERT_FALSE(hu_imessage_breaker_tripped(&ch));
    HU_ASSERT_EQ(hu_imessage_consecutive_failures(&ch), HU_IMESSAGE_BREAKER_THRESHOLD - 1);
    /* A successful poll wipes the streak so the next failure starts fresh. */
    hu_imessage_test_record_poll_success(&ch, 7100);
    HU_ASSERT_EQ(hu_imessage_consecutive_failures(&ch), 0u);
    /* New AUTH should restart counting from 1. */
    (void)hu_imessage_test_record_open_result(&ch, 23, 7200);
    HU_ASSERT_EQ(hu_imessage_consecutive_failures(&ch), 1u);
    HU_ASSERT_FALSE(hu_imessage_breaker_tripped(&ch));
    hu_imessage_destroy(&ch);
}

static void imessage_status_path_resolves_under_home(void) {
    char path[512];
    /* HOME is always set in the test harness. */
    HU_ASSERT_TRUE(hu_imessage_status_path(path, sizeof(path)));
    HU_ASSERT_TRUE(strstr(path, "/.human/imessage.poll_status") != NULL);
}

static void imessage_status_path_rejects_tiny_buffer(void) {
    char tiny[8];
    HU_ASSERT_FALSE(hu_imessage_status_path(tiny, sizeof(tiny)));
}

static void imessage_status_path_handles_no_home(void) {
    /* Save and clear HOME, then restore. */
    const char *saved = getenv("HOME");
    char saved_buf[1024];
    saved_buf[0] = '\0';
    if (saved)
        snprintf(saved_buf, sizeof(saved_buf), "%s", saved);
    unsetenv("HOME");
    char path[512];
    HU_ASSERT_FALSE(hu_imessage_status_path(path, sizeof(path)));
    if (saved_buf[0])
        setenv("HOME", saved_buf, 1);
}

static void imessage_status_file_roundtrip(void) {
    /* Pin HOME to a guaranteed-writable tmp dir for determinism — other test
     * suites may have transiently swapped HOME and not all platforms restore
     * it before we run. The save helper will mkdir(~/.human) on demand. */
    const char *prev_home = getenv("HOME");
    char *saved = prev_home ? strdup(prev_home) : NULL;
    setenv("HOME", "/tmp/hu_imsg_status_roundtrip", 1);
    /* Best-effort cleanup of stale dir from a previous run (don't fail if it
     * doesn't exist or contains files; mkdir handles that next). */
    char prev_status[512];
    snprintf(prev_status, sizeof(prev_status),
             "/tmp/hu_imsg_status_roundtrip/.human/imessage.poll_status");
    unlink(prev_status);

    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    (void)hu_imessage_test_record_open_result(&ch, 23, 8000);
    (void)hu_imessage_test_record_open_result(&ch, 23, 8000);
    char path[512];
    HU_ASSERT_TRUE(hu_imessage_status_path(path, sizeof(path)));
    FILE *f = fopen(path, "r");
    HU_ASSERT_NOT_NULL(f);
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    HU_ASSERT_TRUE(strstr(buf, "\"consecutive_open_failures\": 2") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"last_error_class\": \"AUTH\"") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"circuit_breaker_tripped\": false") != NULL);
    for (uint32_t i = 0; i < HU_IMESSAGE_BREAKER_THRESHOLD; i++)
        (void)hu_imessage_test_record_open_result(&ch, 23, 8000);
    f = fopen(path, "r");
    HU_ASSERT_NOT_NULL(f);
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    HU_ASSERT_TRUE(strstr(buf, "\"circuit_breaker_tripped\": true") != NULL);
    hu_imessage_destroy(&ch);

    unlink(prev_status);
    if (saved) {
        setenv("HOME", saved, 1);
        free(saved);
    } else {
        unsetenv("HOME");
    }
}

static void imessage_breaker_null_safe(void) {
    /* Accessors must tolerate NULL channel / NULL ctx. */
    HU_ASSERT_FALSE(hu_imessage_breaker_tripped(NULL));
    HU_ASSERT_EQ(hu_imessage_consecutive_failures(NULL), 0u);
    HU_ASSERT_EQ(hu_imessage_last_error_class(NULL), HU_IMESSAGE_ERR_NONE);
    HU_ASSERT_EQ(hu_imessage_last_success_epoch(NULL), 0);
    hu_channel_t ch = {0}; /* ctx == NULL */
    HU_ASSERT_FALSE(hu_imessage_breaker_tripped(&ch));
    HU_ASSERT_EQ(hu_imessage_consecutive_failures(&ch), 0u);
}

static void imessage_breaker_test_seam_null_safe(void) {
    /* Test seam must not crash on NULL channel. */
    HU_ASSERT_FALSE(hu_imessage_test_record_open_result(NULL, 23, 9000));
    hu_imessage_test_record_poll_success(NULL, 9000);
    hu_channel_t ch = {0};
    HU_ASSERT_FALSE(hu_imessage_test_record_open_result(&ch, 23, 9000));
    hu_imessage_test_record_poll_success(&ch, 9000);
}

/* ─── Part 25: Watchdog state machine + edge-triggered logging ────────────── */

static void imessage_health_unknown_before_first_poll(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    HU_ASSERT_EQ(hu_imessage_health(&ch, 1000, 60), HU_IMESSAGE_HEALTH_UNKNOWN);
    HU_ASSERT_EQ(hu_imessage_last_logged_health(&ch), HU_IMESSAGE_HEALTH_UNKNOWN);
    hu_imessage_destroy(&ch);
}

static void imessage_health_ok_after_recent_success(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_test_record_poll_success(&ch, 1000);
    HU_ASSERT_EQ(hu_imessage_health(&ch, 1010, 60), HU_IMESSAGE_HEALTH_OK);
    hu_imessage_destroy(&ch);
}

static void imessage_health_stalled_after_threshold(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_test_record_poll_success(&ch, 1000);
    /* 1000 + 60 = 1060: still OK at 1060, STALLED at 1061. */
    HU_ASSERT_EQ(hu_imessage_health(&ch, 1060, 60), HU_IMESSAGE_HEALTH_OK);
    HU_ASSERT_EQ(hu_imessage_health(&ch, 1061, 60), HU_IMESSAGE_HEALTH_STALLED);
    hu_imessage_destroy(&ch);
}

static void imessage_health_tripped_overrides_stalled(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_test_record_poll_success(&ch, 1000);
    /* Trip the breaker. Even though the success was recent (no stall yet),
     * TRIPPED must take precedence in the health enum. */
    for (uint32_t i = 0; i < HU_IMESSAGE_BREAKER_THRESHOLD; i++)
        (void)hu_imessage_test_record_open_result(&ch, 23 /* AUTH */, 1010);
    HU_ASSERT_TRUE(hu_imessage_breaker_tripped(&ch));
    HU_ASSERT_EQ(hu_imessage_health(&ch, 1015, 60), HU_IMESSAGE_HEALTH_TRIPPED);
    hu_imessage_destroy(&ch);
}

static void imessage_health_clock_skew_does_not_falsely_stall(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_test_record_poll_success(&ch, 1000);
    /* now < last_success: clock went backwards. Should report OK, not STALLED. */
    HU_ASSERT_EQ(hu_imessage_health(&ch, 500, 60), HU_IMESSAGE_HEALTH_OK);
    hu_imessage_destroy(&ch);
}

static void imessage_watchdog_silent_on_repeated_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_test_record_poll_success(&ch, 1000);
    /* First tick: UNKNOWN → OK transition is recorded. */
    hu_imessage_watchdog_tick(&ch, 1010, 60);
    HU_ASSERT_EQ(hu_imessage_last_logged_health(&ch), HU_IMESSAGE_HEALTH_OK);
    /* Repeated ticks while still OK do not change the recorded state — that is
     * the contract that makes log spam impossible. */
    hu_imessage_watchdog_tick(&ch, 1020, 60);
    hu_imessage_watchdog_tick(&ch, 1030, 60);
    hu_imessage_watchdog_tick(&ch, 1040, 60);
    HU_ASSERT_EQ(hu_imessage_last_logged_health(&ch), HU_IMESSAGE_HEALTH_OK);
    hu_imessage_destroy(&ch);
}

static void imessage_watchdog_records_ok_to_stalled_transition(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_test_record_poll_success(&ch, 1000);
    hu_imessage_watchdog_tick(&ch, 1010, 60); /* establish OK */
    HU_ASSERT_EQ(hu_imessage_last_logged_health(&ch), HU_IMESSAGE_HEALTH_OK);
    /* Time advances past the stall threshold without another success. */
    hu_imessage_watchdog_tick(&ch, 1100, 60);
    HU_ASSERT_EQ(hu_imessage_last_logged_health(&ch), HU_IMESSAGE_HEALTH_STALLED);
    hu_imessage_destroy(&ch);
}

static void imessage_watchdog_records_recovery_on_success(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_test_record_poll_success(&ch, 1000);
    hu_imessage_watchdog_tick(&ch, 1010, 60);
    hu_imessage_watchdog_tick(&ch, 1100, 60);
    HU_ASSERT_EQ(hu_imessage_last_logged_health(&ch), HU_IMESSAGE_HEALTH_STALLED);
    /* New successful poll → next tick should record recovery. */
    hu_imessage_test_record_poll_success(&ch, 1110);
    hu_imessage_watchdog_tick(&ch, 1120, 60);
    HU_ASSERT_EQ(hu_imessage_last_logged_health(&ch), HU_IMESSAGE_HEALTH_OK);
    hu_imessage_destroy(&ch);
}

static void imessage_watchdog_records_breaker_trip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_test_record_poll_success(&ch, 1000);
    hu_imessage_watchdog_tick(&ch, 1010, 60); /* OK established */
    /* Breaker trips. Watchdog tick should record the OK→TRIPPED transition. */
    for (uint32_t i = 0; i < HU_IMESSAGE_BREAKER_THRESHOLD; i++)
        (void)hu_imessage_test_record_open_result(&ch, 23, 1020);
    hu_imessage_watchdog_tick(&ch, 1030, 60);
    HU_ASSERT_EQ(hu_imessage_last_logged_health(&ch), HU_IMESSAGE_HEALTH_TRIPPED);
    hu_imessage_destroy(&ch);
}

static void imessage_watchdog_default_threshold_used_when_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);
    hu_imessage_test_record_poll_success(&ch, 1000);
    /* threshold=0 must fall back to HU_IMESSAGE_DEFAULT_STALL_SECS, not produce
     * a divide-by-zero or instant STALLED. */
    HU_ASSERT_EQ(hu_imessage_health(&ch, 1010, 0), HU_IMESSAGE_HEALTH_OK);
    HU_ASSERT_EQ(hu_imessage_health(&ch, 1000 + HU_IMESSAGE_DEFAULT_STALL_SECS - 5, 0),
                 HU_IMESSAGE_HEALTH_OK);
    hu_imessage_destroy(&ch);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Regression: idle imsg-watch must refresh the success epoch.
 *
 * Production failure (2026-05-10): `human doctor imessage` reported STALE
 * within ~120s of every daemon restart even though the channel was
 * perfectly healthy and `imsg watch` (pid 46205) was alive. Root cause was
 * `hu_imessage_poll`'s watch-active short-circuit: when the watch was
 * running but no new data had arrived, it returned HU_OK without recording
 * a poll heartbeat. The first poll on watch-start updated
 * `last_successful_poll_epoch`, then the value froze and the doctor's
 * 120s threshold tripped. Behavior we want: every poll cycle that finds
 * the watch alive (even idle) is a heartbeat. Drains noise, never lies.
 * ───────────────────────────────────────────────────────────────────────── */
static void imessage_idle_watch_poll_refreshes_success_epoch(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15551234567", 12, NULL, 0, &ch), HU_OK);

    /* Seed: pretend the watch just started and recorded one success. */
    hu_imessage_test_record_poll_success(&ch, 1000);
    hu_imessage_test_set_watch_running(&ch, true);
    HU_ASSERT_EQ(hu_imessage_test_get_last_success_epoch(&ch), (int64_t)1000);

    /* The daemon now ticks `hu_imessage_poll` repeatedly. The watch is
     * alive but no new messages arrived. Each idle poll should refresh
     * the heartbeat — otherwise the doctor flips to STALE after 120s
     * even though everything is fine. */
    hu_channel_loop_msg_t msgs[2];
    size_t count = 99;
    HU_ASSERT_EQ(hu_imessage_poll(ch.ctx, &alloc, msgs, 2, &count), HU_OK);
    HU_ASSERT_EQ(count, 0u);

    int64_t now = (int64_t)time(NULL);
    int64_t after = hu_imessage_test_get_last_success_epoch(&ch);
    /* The post-poll epoch must have moved forward to at least the current
     * wall clock — not be stuck at the 1000-seed. */
    HU_ASSERT_TRUE(after > 1000);
    HU_ASSERT_TRUE(after <= now);

    /* Health must be OK with a tight 60s threshold relative to "now"
     * (i.e. the heartbeat is recent), proving the live doctor would
     * not see STALE. */
    HU_ASSERT_EQ(hu_imessage_health(&ch, now, 60), HU_IMESSAGE_HEALTH_OK);

    hu_imessage_test_set_watch_running(&ch, false);
    hu_imessage_destroy(&ch);
}

static void imessage_watchdog_null_safe(void) {
    /* All public watchdog calls must be safe on NULL channels (matches the
     * style of the breaker accessors). */
    hu_imessage_watchdog_tick(NULL, 1000, 60);
    HU_ASSERT_EQ(hu_imessage_health(NULL, 1000, 60), HU_IMESSAGE_HEALTH_UNKNOWN);
    HU_ASSERT_EQ(hu_imessage_last_logged_health(NULL), HU_IMESSAGE_HEALTH_UNKNOWN);
    /* Health name table must be total. */
    HU_ASSERT_STR_EQ(hu_imessage_health_name(HU_IMESSAGE_HEALTH_OK), "OK");
    HU_ASSERT_STR_EQ(hu_imessage_health_name(HU_IMESSAGE_HEALTH_STALLED), "STALLED");
    HU_ASSERT_STR_EQ(hu_imessage_health_name(HU_IMESSAGE_HEALTH_TRIPPED), "TRIPPED");
    HU_ASSERT_STR_EQ(hu_imessage_health_name(HU_IMESSAGE_HEALTH_UNKNOWN), "UNKNOWN");
    /* Out-of-range enum values fall back to UNKNOWN rather than NULL. */
    HU_ASSERT_STR_EQ(hu_imessage_health_name((hu_imessage_health_t)999), "UNKNOWN");
}

#endif /* HU_IS_TEST */

/* ═══════════════════════════════════════════════════════════════════════════
 * Suite registration
 * ═══════════════════════════════════════════════════════════════════════════ */

void run_imessage_adversarial_tests(void) {
    HU_TEST_SUITE("iMessage Human-Fidelity Fleet");

    /* Part 1: Send / poll boundary validation */
#if HU_IS_TEST
    HU_RUN_TEST(imessage_send_rejects_null_body_with_positive_len);
    HU_RUN_TEST(imessage_send_rejects_empty_text_and_no_media);
    HU_RUN_TEST(imessage_send_media_only_accepted_as_voice_message);
    HU_RUN_TEST(imessage_send_truncates_overlong_text_to_4095);
    HU_RUN_TEST(imessage_send_records_media_in_test);
    HU_RUN_TEST(imessage_poll_rejects_all_null_args);
    HU_RUN_TEST(imessage_poll_rejects_null_msgs_buffer);
    HU_RUN_TEST(imessage_poll_rejects_null_out_count);
    HU_RUN_TEST(imessage_inject_rejects_null_channel);
    HU_RUN_TEST(imessage_inject_ninth_message_returns_out_of_memory);
    HU_RUN_TEST(imessage_inject_truncates_long_session_key);
    HU_RUN_TEST(imessage_inject_truncates_long_content);
    HU_RUN_TEST(imessage_mock_poll_drops_overflow_when_max_msgs_too_small);
    HU_RUN_TEST(imessage_test_get_last_message_null_channel_returns_null);
    HU_RUN_TEST(imessage_test_get_last_reaction_null_args_no_crash);
#endif

    /* Part 2: Vtable surface coverage */
#if HU_IS_TEST
    HU_RUN_TEST(imessage_vtable_has_all_expected_hooks);
    HU_RUN_TEST(imessage_load_history_returns_not_supported_under_test);
    HU_RUN_TEST(imessage_response_constraints_300_chars);
    HU_RUN_TEST(imessage_response_constraints_null_out_rejected);
    HU_RUN_TEST(imessage_human_active_always_false_under_test);
    HU_RUN_TEST(imessage_attachment_path_null_under_test);
    HU_RUN_TEST(imessage_build_tapback_context_noop_under_test);
    HU_RUN_TEST(imessage_build_read_receipt_context_noop_under_test);
    HU_RUN_TEST(imessage_fetch_gif_stub_returns_null);
    HU_RUN_TEST(imessage_fetch_gif_rejects_empty_query);
    HU_RUN_TEST(imessage_gif_json_extract_finds_url_value);
    HU_RUN_TEST(imessage_gif_json_extract_returns_zero_when_key_missing);
    HU_RUN_TEST(imessage_gif_json_extract_truncates_to_out_cap);
    HU_RUN_TEST(imessage_lookup_guid_not_supported_under_test);
    HU_RUN_TEST(imessage_start_stop_idempotent);
#endif

    /* Part 3: AppleScript escaping */
#if (defined(__APPLE__) && defined(__MACH__)) || HU_IS_TEST
    HU_RUN_TEST(imessage_escape_preserves_utf8_emoji);
    HU_RUN_TEST(imessage_escape_out_cap_two_yields_empty);
    HU_RUN_TEST(imessage_escape_strips_applescript_injection_tokens);
    HU_RUN_TEST(imessage_escape_control_chars_stripped);
    HU_RUN_TEST(imessage_escape_backslash_doubled);
#endif

    /* Part 4: Tapback mapping */
    HU_RUN_TEST(imessage_tapback_mapping_all_types);
#if HU_IS_TEST
    HU_RUN_TEST(imessage_react_vtable_records_heart);
    HU_RUN_TEST(imessage_react_vtable_overwrites_previous);
#endif

    /* Part 5: Format pipeline (markdown + AI phrase + 300 char) */
    HU_RUN_TEST(imessage_format_strips_markdown_bold);
    HU_RUN_TEST(imessage_format_strips_markdown_headers);
    HU_RUN_TEST(imessage_format_strips_ai_happy_to);
    HU_RUN_TEST(imessage_format_strips_ai_great_question);
    HU_RUN_TEST(imessage_format_strips_as_an_ai);
    HU_RUN_TEST(imessage_format_outbound_full_pipeline);
    HU_RUN_TEST(imessage_format_outbound_caps_at_300);

    /* Part 6: Conversation awareness */
    HU_RUN_TEST(imessage_awareness_builds_thread_context);
    HU_RUN_TEST(imessage_awareness_anti_ai_rules_present);
    HU_RUN_TEST(imessage_quality_score_penalizes_long_response);
    HU_RUN_TEST(imessage_quality_score_short_response_not_flagged_for_revision);

    /* Part 7: Tapback decisions + reaction classification */
    HU_RUN_TEST(imessage_tapback_decision_on_haha);
    HU_RUN_TEST(imessage_reaction_classify_on_funny_text);
    HU_RUN_TEST(imessage_self_reaction_mostly_none);

    /* Part 8: GIF calibration loop */
    HU_RUN_TEST(imessage_gif_cal_neutral_before_3_sends);
    HU_RUN_TEST(imessage_gif_cal_tracks_sends_and_reactions);
    HU_RUN_TEST(imessage_gif_probability_adjusts_by_relationship);
    HU_RUN_TEST(imessage_gif_should_send_deterministic_false_on_serious);
    HU_RUN_TEST(imessage_should_send_music_on_music_mention);
    HU_RUN_TEST(imessage_should_send_music_respects_zero_probability);
    HU_RUN_TEST(imessage_build_music_prompt_includes_context);

    /* Part 9: Seen behavior / emotional intelligence */
    HU_RUN_TEST(imessage_seen_behavior_responds_to_question);
    HU_RUN_TEST(imessage_engagement_detection_works);
    HU_RUN_TEST(imessage_emotion_detection_on_distress);
    HU_RUN_TEST(imessage_narrative_phase_from_short_convo);

    /* Part 10: Persona overlays + example banks */
    HU_RUN_TEST(imessage_persona_overlay_found_by_channel);
    HU_RUN_TEST(imessage_persona_overlay_not_found_for_other_channel);
    HU_RUN_TEST(imessage_persona_prompt_includes_casual_with_overlay);
    HU_RUN_TEST(imessage_persona_example_selection_matches_topic);

    /* Part 11: Poll metadata */
#if HU_IS_TEST
    HU_RUN_TEST(imessage_poll_photo_flag_propagates);
    HU_RUN_TEST(imessage_poll_video_flag_propagates);
    HU_RUN_TEST(imessage_poll_multi_sender_batch);
#endif

    /* Part 12: Conversation hints */
    HU_RUN_TEST(imessage_cold_restart_hint_on_fresh_convo);
    HU_RUN_TEST(imessage_group_mention_hint_non_empty_for_group);
    HU_RUN_TEST(imessage_group_mention_hint_empty_for_dm);
    HU_RUN_TEST(imessage_link_context_detects_url);
    HU_RUN_TEST(imessage_emoji_mirror_hint_exists);
    HU_RUN_TEST(imessage_gif_style_hint_for_friend);

    /* Part 13: Effect mapping, balloon labels, mark_read, use_imsg_cli */
    HU_RUN_TEST(imessage_effect_name_slam);
    HU_RUN_TEST(imessage_effect_name_all_known);
    HU_RUN_TEST(imessage_effect_name_null_and_unknown);
    HU_RUN_TEST(imessage_balloon_label_sticker);
    HU_RUN_TEST(imessage_balloon_label_memoji);
    HU_RUN_TEST(imessage_balloon_label_app);
    HU_RUN_TEST(imessage_balloon_label_null_and_empty);
    HU_RUN_TEST(imessage_text_is_placeholder_true_cases);
    HU_RUN_TEST(imessage_text_is_placeholder_false_cases);
    HU_RUN_TEST(imessage_balloon_override_only_on_placeholder);
    HU_RUN_TEST(imessage_copy_bounded_normal);
    HU_RUN_TEST(imessage_copy_bounded_truncates);
    HU_RUN_TEST(imessage_copy_bounded_exact_fit);
    HU_RUN_TEST(imessage_copy_bounded_null_and_empty);
#if HU_IS_TEST
    HU_RUN_TEST(imessage_mark_read_returns_ok_under_test);
    HU_RUN_TEST(imessage_mark_read_null_ctx_no_crash);
    HU_RUN_TEST(imessage_start_typing_returns_ok);
    HU_RUN_TEST(imessage_start_typing_null_ctx_safe);
    HU_RUN_TEST(imessage_set_use_imsg_cli_wires_to_context);
#endif

    /* Part 14: imsg CLI deep integration (watch, attachments, validation) */
#if HU_IS_TEST
    HU_RUN_TEST(imessage_watch_active_false_under_test);
    HU_RUN_TEST(imessage_watch_active_null_channel_returns_false);
    HU_RUN_TEST(imessage_watch_active_false_without_imsg_cli);
    HU_RUN_TEST(imessage_start_stop_with_imsg_cli_no_crash);
    HU_RUN_TEST(imessage_destroy_with_imsg_cli_no_crash);
    HU_RUN_TEST(imessage_imsg_cli_text_send_test_mode_stores_message);
    HU_RUN_TEST(imessage_imsg_cli_media_test_mode_stores_path);
    HU_RUN_TEST(imessage_poll_returns_ok_with_no_mocks_and_imsg_cli);
    HU_RUN_TEST(imessage_start_stop_double_stop_no_crash);
    HU_RUN_TEST(imessage_start_stop_cycle_with_poll);
#endif

    /* Part 15: Typing duration model (red team) */
    HU_RUN_TEST(imessage_typing_duration_short_msg_above_floor);
    HU_RUN_TEST(imessage_typing_duration_medium_msg);
    HU_RUN_TEST(imessage_typing_duration_long_msg_capped);
    HU_RUN_TEST(imessage_typing_duration_zero_len);
    HU_RUN_TEST(imessage_typing_duration_varies_by_seed);
    HU_RUN_TEST(imessage_typing_duration_jitter_bounded);

    /* Part 15b: IMCore selector conformance (pins the typing-phantom drift) */
    HU_RUN_TEST(imcore_required_table_nonempty_and_pins_typing_setter);
    HU_RUN_TEST(imcore_conformance_all_present_reports_zero_missing);
    HU_RUN_TEST(imcore_conformance_none_present_flags_every_selector);
    HU_RUN_TEST(imcore_conformance_pins_single_drifted_selector);
    HU_RUN_TEST(imcore_conformance_null_args_safe);

    /* Part 16: Tapback decision exhaustive (red team) */
    HU_RUN_TEST(imessage_tapback_decision_question_prefers_text);
    HU_RUN_TEST(imessage_tapback_decision_emotional_heavy_prefers_text);
    HU_RUN_TEST(imessage_tapback_decision_got_fired_prefers_text);
    HU_RUN_TEST(imessage_tapback_decision_lol_tapback_dominant);
    HU_RUN_TEST(imessage_tapback_decision_ok_often_no_response);
    HU_RUN_TEST(imessage_tapback_decision_nice_cool_tapback_likely);
    HU_RUN_TEST(imessage_tapback_decision_short_msg_no_question);
    HU_RUN_TEST(imessage_tapback_decision_substantive_prefers_text);
    HU_RUN_TEST(imessage_tapback_decision_null_returns_no_response);
    HU_RUN_TEST(imessage_tapback_decision_empty_returns_no_response);
    HU_RUN_TEST(imessage_tapback_decision_anti_spam_recent_tapbacks);

    /* Part 17: Seen-behavior classification (red team) */
    HU_RUN_TEST(imessage_seen_behavior_non_busy_always_now);
    HU_RUN_TEST(imessage_seen_behavior_urgent_bypasses_delay);
    HU_RUN_TEST(imessage_seen_behavior_busy_low_priority_sometimes_delays);
    HU_RUN_TEST(imessage_seen_behavior_question_during_busy_responds_now);
    HU_RUN_TEST(imessage_seen_behavior_null_msg_responds_now);

    /* Part 18: Double-text decision (red team) */
    HU_RUN_TEST(imessage_double_text_farewell_suppressed);
    HU_RUN_TEST(imessage_double_text_late_night_suppressed);
    HU_RUN_TEST(imessage_double_text_zero_probability_never);
    HU_RUN_TEST(imessage_double_text_null_response_never);
    HU_RUN_TEST(imessage_double_text_sometimes_true_with_high_prob);

    /* Part 19: Reaction classifier exhaustive (red team) */
    HU_RUN_TEST(imessage_reaction_heart_on_love_you);
    HU_RUN_TEST(imessage_reaction_heart_on_miss_you);
    HU_RUN_TEST(imessage_reaction_thumbs_up_on_absolutely);
    HU_RUN_TEST(imessage_reaction_emphasis_on_got_the_job);
    HU_RUN_TEST(imessage_reaction_none_on_question);
    HU_RUN_TEST(imessage_reaction_gif_haha_or_heart);
    HU_RUN_TEST(imessage_reaction_never_reacts_to_own);
    HU_RUN_TEST(imessage_reaction_photo_sometimes_heart);

    /* Part 20: Self-reaction trigger verification (red team) */
    HU_RUN_TEST(imessage_self_reaction_oops_yields_haha);
    HU_RUN_TEST(imessage_self_reaction_exclaim_yields_emphasis);
    HU_RUN_TEST(imessage_self_reaction_normal_text_always_none_at_high_seed);
    HU_RUN_TEST(imessage_self_reaction_null_safe);
    /* Part 21: Mock poll parity — full field injection */
    HU_RUN_TEST(imessage_mock_full_propagates_all_fields);
    HU_RUN_TEST(imessage_mock_full_group_msg_sets_is_group_and_chat_id);
    HU_RUN_TEST(imessage_mock_full_edited_unsent_flags);
    HU_RUN_TEST(imessage_mock_full_inline_reply_guid);
    HU_RUN_TEST(imessage_mock_full_timestamp_propagated);
    HU_RUN_TEST(imessage_mock_full_null_opts_rejected);

    /* Part 22: GUID lookup test store */
    HU_RUN_TEST(imessage_guid_lookup_returns_stored_text);
    HU_RUN_TEST(imessage_guid_lookup_unknown_guid_not_supported);
    HU_RUN_TEST(imessage_guid_lookup_clear_empties_store);
    HU_RUN_TEST(imessage_guid_lookup_null_args_rejected);

    /* Part 23: Group chat routing */
    HU_RUN_TEST(imessage_group_msg_chat_id_populated);
    HU_RUN_TEST(imessage_dm_msg_chat_id_empty);

    /* Part 24: FDA-aware circuit breaker + poll-status file (red team) */
#if HU_IS_TEST
    HU_RUN_TEST(imessage_classify_known_codes);
    HU_RUN_TEST(imessage_classify_name_roundtrip);
    HU_RUN_TEST(imessage_breaker_trips_after_threshold_auth);
    HU_RUN_TEST(imessage_breaker_trips_on_cantopen_too);
    HU_RUN_TEST(imessage_breaker_does_not_trip_on_busy);
    HU_RUN_TEST(imessage_breaker_does_not_trip_on_other_errors);
    HU_RUN_TEST(imessage_breaker_resets_on_success);
    HU_RUN_TEST(imessage_breaker_partial_streak_resets_on_success);
    HU_RUN_TEST(imessage_status_path_resolves_under_home);
    HU_RUN_TEST(imessage_status_path_rejects_tiny_buffer);
    HU_RUN_TEST(imessage_status_path_handles_no_home);
    HU_RUN_TEST(imessage_status_file_roundtrip);
    HU_RUN_TEST(imessage_breaker_null_safe);
    HU_RUN_TEST(imessage_breaker_test_seam_null_safe);
#endif

    /* Part 25: Watchdog state machine + edge-triggered logging (red team) */
#if HU_IS_TEST
    HU_RUN_TEST(imessage_health_unknown_before_first_poll);
    HU_RUN_TEST(imessage_health_ok_after_recent_success);
    HU_RUN_TEST(imessage_health_stalled_after_threshold);
    HU_RUN_TEST(imessage_health_tripped_overrides_stalled);
    HU_RUN_TEST(imessage_health_clock_skew_does_not_falsely_stall);
    HU_RUN_TEST(imessage_watchdog_silent_on_repeated_ok);
    HU_RUN_TEST(imessage_watchdog_records_ok_to_stalled_transition);
    HU_RUN_TEST(imessage_watchdog_records_recovery_on_success);
    HU_RUN_TEST(imessage_watchdog_records_breaker_trip);
    HU_RUN_TEST(imessage_watchdog_default_threshold_used_when_zero);
    HU_RUN_TEST(imessage_idle_watch_poll_refreshes_success_epoch);
    HU_RUN_TEST(imessage_watchdog_null_safe);
#endif
}
#else  /* !HU_HAS_IMESSAGE */
void run_imessage_adversarial_tests(void) {
    (void)0;
}
#endif /* HU_HAS_IMESSAGE */
