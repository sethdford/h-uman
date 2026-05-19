/* tests/test_whatsapp_reactions.c
 *
 * Phase 2 of docs/plans/2026-05-18-imessage-sota.md: unit tests for the
 * WhatsApp reaction webhook branch. Branch lives in
 * src/channels/whatsapp_reactions.c (split out for the same enum-name
 * collision reason as the other reaction emit files — see the
 * breadcrumb in that file for the full story).
 *
 * Per test-source-gate-symmetry rule: production symbol lives in
 * src/channels/whatsapp_reactions.c (gated by HU_ENABLE_WHATSAPP in
 * CMakeLists.txt). Internal-#ifdef-wrap-with-stub-runner pattern: test
 * bodies guarded on HU_HAS_WHATSAPP so non-whatsapp build variants
 * link cleanly. */

#include "test_framework.h"

#ifdef HU_HAS_WHATSAPP

#include "human/channels/reaction_event.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdlib.h>
#include <string.h>

/* Forward decl — symbol exposed only as a function (no header). */
hu_error_t hu_whatsapp_handle_reaction_event_for_test(const char *body, size_t body_len,
                                                      hu_allocator_t *alloc,
                                                      const char *bot_user_id,
                                                      hu_reaction_event_t *out);

static void free_evt(hu_reaction_event_t *e) {
    free((void *)e->target_thread_id);
    free((void *)e->target_message_ref);
    free((void *)e->sender_handle);
    free((void *)e->emoji);
}

/* Standard reaction body shape, hard-coded fields for assertion. */
#define WA_BODY_ENVELOPE_BEGIN "{\"entry\":[{\"changes\":[{\"value\":{\"messages\":[{"
#define WA_BODY_ENVELOPE_END   "}]}}]}]}"

static void test_whatsapp_reaction_add_heart_emits_love(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* ❤️ = U+2764 U+FE0F = \xe2\x9d\xa4\xef\xb8\x8f */
    const char *payload = WA_BODY_ENVELOPE_BEGIN
        "\"from\":\"16315551234\",\"id\":\"wamid.IN\",\"timestamp\":\"1730000000\","
        "\"type\":\"reaction\",\"reaction\":{\"message_id\":\"wamid.TGT\","
        "\"emoji\":\"\xe2\x9d\xa4\xef\xb8\x8f\"}" WA_BODY_ENVELOPE_END;
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_whatsapp_handle_reaction_event_for_test(payload, 0, &alloc, "BOT_PHONE_ID", &e),
                 HU_OK);
    HU_ASSERT_EQ(e.kind, HU_REACTION_LOVE);
    HU_ASSERT_EQ(e.polarity, HU_REACTION_POSITIVE);
    HU_ASSERT_STR_EQ(e.channel_id, "whatsapp");
    HU_ASSERT_STR_EQ(e.target_thread_id, "16315551234");
    HU_ASSERT_STR_EQ(e.target_message_ref, "wamid.TGT");
    HU_ASSERT_STR_EQ(e.sender_handle, "16315551234");
    HU_ASSERT_EQ(e.is_removal, 0);
    HU_ASSERT_EQ(e.timestamp_unix, (int64_t)1730000000);
    HU_ASSERT_NULL(e.emoji);
    free_evt(&e);
}

static void test_whatsapp_reaction_add_thumbsdown_emits_dislike(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* 👎 = \xf0\x9f\x91\x8e */
    const char *payload = WA_BODY_ENVELOPE_BEGIN
        "\"from\":\"16315551234\",\"id\":\"wamid.IN\",\"timestamp\":\"1730000001\","
        "\"type\":\"reaction\",\"reaction\":{\"message_id\":\"wamid.TGT\","
        "\"emoji\":\"\xf0\x9f\x91\x8e\"}" WA_BODY_ENVELOPE_END;
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_whatsapp_handle_reaction_event_for_test(payload, 0, &alloc, "BOT_PHONE_ID", &e),
                 HU_OK);
    HU_ASSERT_EQ(e.kind, HU_REACTION_DISLIKE);
    HU_ASSERT_EQ(e.polarity, HU_REACTION_NEGATIVE);
    free_evt(&e);
}

static void test_whatsapp_custom_emoji_populates_emoji_field(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* 🦄 unicorn — not in our normalizer table, falls through to
     * CUSTOM_EMOJI with the glyph preserved on evt.emoji. */
    const char *payload = WA_BODY_ENVELOPE_BEGIN
        "\"from\":\"16315551234\",\"id\":\"wamid.IN\",\"timestamp\":\"1730000002\","
        "\"type\":\"reaction\",\"reaction\":{\"message_id\":\"wamid.TGT\","
        "\"emoji\":\"\xf0\x9f\xa6\x84\"}" WA_BODY_ENVELOPE_END;
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_whatsapp_handle_reaction_event_for_test(payload, 0, &alloc, "BOT_PHONE_ID", &e),
                 HU_OK);
    HU_ASSERT_EQ(e.kind, HU_REACTION_KIND_CUSTOM_EMOJI);
    HU_ASSERT_NOT_NULL(e.emoji);
    HU_ASSERT_STR_EQ(e.emoji, "\xf0\x9f\xa6\x84");
    free_evt(&e);
}

static void test_whatsapp_empty_emoji_signals_removal(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* Meta sends emoji="" when a user retracts their reaction. */
    const char *payload = WA_BODY_ENVELOPE_BEGIN
        "\"from\":\"16315551234\",\"id\":\"wamid.IN\",\"timestamp\":\"1730000003\","
        "\"type\":\"reaction\",\"reaction\":{\"message_id\":\"wamid.TGT\","
        "\"emoji\":\"\"}" WA_BODY_ENVELOPE_END;
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_whatsapp_handle_reaction_event_for_test(payload, 0, &alloc, "BOT_PHONE_ID", &e),
                 HU_OK);
    HU_ASSERT_EQ(e.is_removal, 1);
    HU_ASSERT_STR_EQ(e.target_message_ref, "wamid.TGT");
    free_evt(&e);
}

static void test_whatsapp_null_emoji_signals_removal(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* JSON null is the other documented retraction shape. */
    const char *payload = WA_BODY_ENVELOPE_BEGIN
        "\"from\":\"16315551234\",\"id\":\"wamid.IN\",\"timestamp\":\"1730000004\","
        "\"type\":\"reaction\",\"reaction\":{\"message_id\":\"wamid.TGT\","
        "\"emoji\":null}" WA_BODY_ENVELOPE_END;
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_whatsapp_handle_reaction_event_for_test(payload, 0, &alloc, "BOT_PHONE_ID", &e),
                 HU_OK);
    HU_ASSERT_EQ(e.is_removal, 1);
    free_evt(&e);
}

static void test_whatsapp_self_reaction_dropped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* from == bot_user_id — self-reaction. */
    const char *payload = WA_BODY_ENVELOPE_BEGIN
        "\"from\":\"BOT_PHONE_ID\",\"id\":\"wamid.IN\",\"timestamp\":\"1730000000\","
        "\"type\":\"reaction\",\"reaction\":{\"message_id\":\"wamid.TGT\","
        "\"emoji\":\"\xe2\x9d\xa4\"}" WA_BODY_ENVELOPE_END;
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_whatsapp_handle_reaction_event_for_test(payload, 0, &alloc, "BOT_PHONE_ID", &e),
                 HU_ERR_NOT_SUPPORTED);
}

static void test_whatsapp_malformed_json_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *payload = "{not json";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_whatsapp_handle_reaction_event_for_test(payload, 0, &alloc, "BOT_PHONE_ID", &e),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_whatsapp_text_message_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* type="text" is not a reaction; helper should reject so caller
     * knows to fall through to the normal text dispatcher. */
    const char *payload = WA_BODY_ENVELOPE_BEGIN
        "\"from\":\"16315551234\",\"id\":\"wamid.IN\",\"timestamp\":\"1730000000\","
        "\"type\":\"text\",\"text\":{\"body\":\"hi\"}" WA_BODY_ENVELOPE_END;
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_whatsapp_handle_reaction_event_for_test(payload, 0, &alloc, "BOT_PHONE_ID", &e),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_whatsapp_thumbsup_emits_like(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* 👍 = \xf0\x9f\x91\x8d */
    const char *payload = WA_BODY_ENVELOPE_BEGIN
        "\"from\":\"16315551234\",\"id\":\"wamid.IN\",\"timestamp\":\"1730000005\","
        "\"type\":\"reaction\",\"reaction\":{\"message_id\":\"wamid.TGT\","
        "\"emoji\":\"\xf0\x9f\x91\x8d\"}" WA_BODY_ENVELOPE_END;
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_whatsapp_handle_reaction_event_for_test(payload, 0, &alloc, "BOT_PHONE_ID", &e),
                 HU_OK);
    HU_ASSERT_EQ(e.kind, HU_REACTION_LIKE);
    HU_ASSERT_EQ(e.polarity, HU_REACTION_POSITIVE);
    free_evt(&e);
}

static void test_whatsapp_laugh_emits_laugh(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* 😂 = \xf0\x9f\x98\x82 */
    const char *payload = WA_BODY_ENVELOPE_BEGIN
        "\"from\":\"16315551234\",\"id\":\"wamid.IN\",\"timestamp\":\"1730000006\","
        "\"type\":\"reaction\",\"reaction\":{\"message_id\":\"wamid.TGT\","
        "\"emoji\":\"\xf0\x9f\x98\x82\"}" WA_BODY_ENVELOPE_END;
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_whatsapp_handle_reaction_event_for_test(payload, 0, &alloc, "BOT_PHONE_ID", &e),
                 HU_OK);
    HU_ASSERT_EQ(e.kind, HU_REACTION_LAUGH);
    free_evt(&e);
}

static void test_whatsapp_question_emits_question(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* ❓ = \xe2\x9d\x93 */
    const char *payload = WA_BODY_ENVELOPE_BEGIN
        "\"from\":\"16315551234\",\"id\":\"wamid.IN\",\"timestamp\":\"1730000007\","
        "\"type\":\"reaction\",\"reaction\":{\"message_id\":\"wamid.TGT\","
        "\"emoji\":\"\xe2\x9d\x93\"}" WA_BODY_ENVELOPE_END;
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_whatsapp_handle_reaction_event_for_test(payload, 0, &alloc, "BOT_PHONE_ID", &e),
                 HU_OK);
    HU_ASSERT_EQ(e.kind, HU_REACTION_KIND_QUESTION);
    HU_ASSERT_EQ(e.polarity, HU_REACTION_NEUTRAL);
    free_evt(&e);
}

void run_whatsapp_reactions_tests(void) {
    HU_TEST_SUITE("whatsapp_reactions");
    HU_RUN_TEST(test_whatsapp_reaction_add_heart_emits_love);
    HU_RUN_TEST(test_whatsapp_reaction_add_thumbsdown_emits_dislike);
    HU_RUN_TEST(test_whatsapp_custom_emoji_populates_emoji_field);
    HU_RUN_TEST(test_whatsapp_empty_emoji_signals_removal);
    HU_RUN_TEST(test_whatsapp_null_emoji_signals_removal);
    HU_RUN_TEST(test_whatsapp_self_reaction_dropped);
    HU_RUN_TEST(test_whatsapp_malformed_json_returns_invalid);
    HU_RUN_TEST(test_whatsapp_text_message_returns_invalid);
    HU_RUN_TEST(test_whatsapp_thumbsup_emits_like);
    HU_RUN_TEST(test_whatsapp_laugh_emits_laugh);
    HU_RUN_TEST(test_whatsapp_question_emits_question);
}

#else /* !HU_HAS_WHATSAPP */

void run_whatsapp_reactions_tests(void) {
    (void)0;
}

#endif
