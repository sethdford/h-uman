/* tests/test_discord_reactions.c
 *
 * RL SOTA — unit tests for the Discord MESSAGE_REACTION_ADD /
 * MESSAGE_REACTION_REMOVE gateway-event branch. The branch lives in
 * src/channels/discord_reactions.c (split out from discord.c for the
 * same enum-name collision reason as slack_reactions.c — see the
 * breadcrumb in discord_reactions.c for the full story).
 *
 * Test contract: hu_discord_handle_reaction_event_for_test returns
 *   - HU_OK on a valid reaction (event filled).
 *   - HU_ERR_NOT_SUPPORTED when a filter rejects the event
 *     (self-reaction, reaction on a non-bot message).
 *   - HU_ERR_INVALID_ARGUMENT on parse / shape errors / unknown emoji.
 * The inline gateway branch in discord.c silently absorbs filter
 * rejections; the test helper has the stricter return contract so
 * tests can pin the filter behavior. Both contracts are intentional. */
#include "human/channels/reaction_event.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

/* Forward decl — symbol is exposed only as a function from
 * discord_reactions.c (no header) to keep the public surface unchanged. */
hu_error_t hu_discord_handle_reaction_event_for_test(const char *payload, size_t payload_len,
                                                     hu_allocator_t *alloc, const char *bot_user_id,
                                                     hu_reaction_event_t *out);

static void free_evt_fields(hu_reaction_event_t *e) {
    free((void *)e->target_thread_id);
    free((void *)e->target_message_ref);
    free((void *)e->sender_handle);
    free((void *)e->emoji);
}

static void test_discord_reaction_add_heart_emits_love(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* ❤ = \xe2\x9d\xa4 */
    const char *payload = "{\"op\":0,\"t\":\"MESSAGE_REACTION_ADD\",\"d\":{"
                          "\"user_id\":\"U_REAL\",\"message_id\":\"M_123\","
                          "\"channel_id\":\"C_456\",\"message_author_id\":\"U_BOT\","
                          "\"emoji\":{\"id\":null,\"name\":\"\xe2\x9d\xa4\",\"animated\":false}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_discord_handle_reaction_event_for_test(payload, 0, &alloc, "U_BOT", &e), HU_OK);
    HU_ASSERT_EQ(e.kind, HU_REACTION_LOVE);
    HU_ASSERT_EQ(e.polarity, HU_REACTION_POSITIVE);
    HU_ASSERT_STR_EQ(e.channel_id, "discord");
    HU_ASSERT_STR_EQ(e.target_thread_id, "C_456");
    HU_ASSERT_STR_EQ(e.target_message_ref, "M_123");
    HU_ASSERT_STR_EQ(e.sender_handle, "U_REAL");
    HU_ASSERT_EQ(e.is_removal, 0);
    free_evt_fields(&e);
}

static void test_discord_reaction_add_thumbsdown_emits_dislike(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* 👎 = \xf0\x9f\x91\x8e */
    const char *payload = "{\"op\":0,\"t\":\"MESSAGE_REACTION_ADD\",\"d\":{"
                          "\"user_id\":\"U_REAL\",\"message_id\":\"M_123\","
                          "\"channel_id\":\"C_456\",\"message_author_id\":\"U_BOT\","
                          "\"emoji\":{\"id\":null,\"name\":\"\xf0\x9f\x91\x8e\"}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_discord_handle_reaction_event_for_test(payload, 0, &alloc, "U_BOT", &e), HU_OK);
    HU_ASSERT_EQ(e.kind, HU_REACTION_DISLIKE);
    HU_ASSERT_EQ(e.polarity, HU_REACTION_NEGATIVE);
    free_evt_fields(&e);
}

static void test_discord_reaction_add_thumbsup_skin_tone_emits_like(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* 👍🏿 = \xf0\x9f\x91\x8d + \xf0\x9f\x8f\xbf — base codepoint precedes
     * modifier, so the substring matcher still picks up 👍. */
    const char *payload = "{\"op\":0,\"t\":\"MESSAGE_REACTION_ADD\",\"d\":{"
                          "\"user_id\":\"U_REAL\",\"message_id\":\"M_X\","
                          "\"channel_id\":\"C_X\",\"message_author_id\":\"U_BOT\","
                          "\"emoji\":{\"id\":null,\"name\":\"\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbf\"}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_discord_handle_reaction_event_for_test(payload, 0, &alloc, "U_BOT", &e), HU_OK);
    HU_ASSERT_EQ(e.kind, HU_REACTION_LIKE);
    HU_ASSERT_EQ(e.polarity, HU_REACTION_POSITIVE);
    free_evt_fields(&e);
}

static void test_discord_reaction_custom_emoji_populates_emoji_field(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* Custom server emoji: emoji.id is a snowflake string. */
    const char *payload =
        "{\"op\":0,\"t\":\"MESSAGE_REACTION_ADD\",\"d\":{"
        "\"user_id\":\"U_REAL\",\"message_id\":\"M_1\","
        "\"channel_id\":\"C_1\",\"message_author_id\":\"U_BOT\","
        "\"emoji\":{\"id\":\"123456789\",\"name\":\"blobwave\",\"animated\":false}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_discord_handle_reaction_event_for_test(payload, 0, &alloc, "U_BOT", &e), HU_OK);
    HU_ASSERT_EQ(e.kind, HU_REACTION_KIND_CUSTOM_EMOJI);
    HU_ASSERT_EQ(e.polarity, HU_REACTION_POSITIVE);
    HU_ASSERT_NOT_NULL(e.emoji);
    HU_ASSERT_STR_EQ(e.emoji, "blobwave");
    free_evt_fields(&e);
}

static void test_discord_reaction_remove_sets_is_removal(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *payload = "{\"op\":0,\"t\":\"MESSAGE_REACTION_REMOVE\",\"d\":{"
                          "\"user_id\":\"U_REAL\",\"message_id\":\"M_RM\","
                          "\"channel_id\":\"C_RM\",\"message_author_id\":\"U_BOT\","
                          "\"emoji\":{\"id\":null,\"name\":\"\xe2\x9d\xa4\"}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_discord_handle_reaction_event_for_test(payload, 0, &alloc, "U_BOT", &e), HU_OK);
    HU_ASSERT_EQ(e.is_removal, 1);
    HU_ASSERT_EQ(e.kind, HU_REACTION_LOVE);
    free_evt_fields(&e);
}

static void test_discord_self_reaction_is_dropped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* user_id == bot_user_id — self-reaction. */
    const char *payload = "{\"op\":0,\"t\":\"MESSAGE_REACTION_ADD\",\"d\":{"
                          "\"user_id\":\"U_BOT\",\"message_id\":\"M_1\","
                          "\"channel_id\":\"C_1\",\"message_author_id\":\"U_BOT\","
                          "\"emoji\":{\"id\":null,\"name\":\"\xe2\x9d\xa4\"}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_discord_handle_reaction_event_for_test(payload, 0, &alloc, "U_BOT", &e),
                 HU_ERR_NOT_SUPPORTED);
}

static void test_discord_reaction_on_third_party_message_dropped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* message_author_id != bot_user_id — reaction on someone else's
     * message. We only collect on our own outbound. */
    const char *payload = "{\"op\":0,\"t\":\"MESSAGE_REACTION_ADD\",\"d\":{"
                          "\"user_id\":\"U_REAL\",\"message_id\":\"M_1\","
                          "\"channel_id\":\"C_1\",\"message_author_id\":\"U_OTHER\","
                          "\"emoji\":{\"id\":null,\"name\":\"\xe2\x9d\xa4\"}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_discord_handle_reaction_event_for_test(payload, 0, &alloc, "U_BOT", &e),
                 HU_ERR_NOT_SUPPORTED);
}

static void test_discord_unknown_event_type_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* MESSAGE_CREATE is not a reaction event; the helper should reject
     * it so the caller knows to fall through to the normal dispatcher. */
    const char *payload = "{\"op\":0,\"t\":\"MESSAGE_CREATE\",\"d\":{\"content\":\"hi\"}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_discord_handle_reaction_event_for_test(payload, 0, &alloc, "U_BOT", &e),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_discord_malformed_json_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *payload = "{not json";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_discord_handle_reaction_event_for_test(payload, 0, &alloc, "U_BOT", &e),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_discord_unknown_standard_emoji_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* 🦄 unicorn — not in our normalizer table. */
    const char *payload = "{\"op\":0,\"t\":\"MESSAGE_REACTION_ADD\",\"d\":{"
                          "\"user_id\":\"U_REAL\",\"message_id\":\"M_1\","
                          "\"channel_id\":\"C_1\",\"message_author_id\":\"U_BOT\","
                          "\"emoji\":{\"id\":null,\"name\":\"\xf0\x9f\xa6\x84\"}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_discord_handle_reaction_event_for_test(payload, 0, &alloc, "U_BOT", &e),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_discord_question_mark_emits_question(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* ❓ = \xe2\x9d\x93 */
    const char *payload = "{\"op\":0,\"t\":\"MESSAGE_REACTION_ADD\",\"d\":{"
                          "\"user_id\":\"U_REAL\",\"message_id\":\"M_Q\","
                          "\"channel_id\":\"C_Q\",\"message_author_id\":\"U_BOT\","
                          "\"emoji\":{\"id\":null,\"name\":\"\xe2\x9d\x93\"}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_discord_handle_reaction_event_for_test(payload, 0, &alloc, "U_BOT", &e), HU_OK);
    HU_ASSERT_EQ(e.kind, HU_REACTION_KIND_QUESTION);
    HU_ASSERT_EQ(e.polarity, HU_REACTION_NEUTRAL);
    HU_ASSERT_NULL(e.emoji); /* standard emoji must NOT populate emoji field */
    free_evt_fields(&e);
}

static void test_discord_emphasize_emits_emphasize(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* ❗ = \xe2\x9d\x97 */
    const char *payload = "{\"op\":0,\"t\":\"MESSAGE_REACTION_ADD\",\"d\":{"
                          "\"user_id\":\"U_REAL\",\"message_id\":\"M_E\","
                          "\"channel_id\":\"C_E\",\"message_author_id\":\"U_BOT\","
                          "\"emoji\":{\"id\":null,\"name\":\"\xe2\x9d\x97\"}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_discord_handle_reaction_event_for_test(payload, 0, &alloc, "U_BOT", &e), HU_OK);
    HU_ASSERT_EQ(e.kind, HU_REACTION_EMPHASIZE);
    HU_ASSERT_EQ(e.polarity, HU_REACTION_POSITIVE);
    free_evt_fields(&e);
}

void run_discord_reactions_tests(void) {
    HU_TEST_SUITE("discord_reactions");
    HU_RUN_TEST(test_discord_reaction_add_heart_emits_love);
    HU_RUN_TEST(test_discord_reaction_add_thumbsdown_emits_dislike);
    HU_RUN_TEST(test_discord_reaction_add_thumbsup_skin_tone_emits_like);
    HU_RUN_TEST(test_discord_reaction_custom_emoji_populates_emoji_field);
    HU_RUN_TEST(test_discord_reaction_remove_sets_is_removal);
    HU_RUN_TEST(test_discord_self_reaction_is_dropped);
    HU_RUN_TEST(test_discord_reaction_on_third_party_message_dropped);
    HU_RUN_TEST(test_discord_unknown_event_type_returns_invalid);
    HU_RUN_TEST(test_discord_malformed_json_returns_invalid);
    HU_RUN_TEST(test_discord_unknown_standard_emoji_returns_invalid);
    HU_RUN_TEST(test_discord_question_mark_emits_question);
    HU_RUN_TEST(test_discord_emphasize_emits_emphasize);
}
