/* tests/test_telegram_reactions.c
 *
 * Unit tests for the Telegram message_reaction update branch. Pin every
 * diff case (add / remove / replace / no-op), normalization for every
 * supported emoji family, self-filtering, custom_emoji handling, and
 * malformed-JSON absorption.
 *
 * Mirror of test_slack_reactions.c — the test helper has a strict
 * return contract (HU_OK / HU_ERR_NOT_SUPPORTED / HU_ERR_INVALID_ARGUMENT)
 * so we can assert on filter behavior. The inline production branch in
 * telegram.c silently absorbs filter rejections.
 *
 * Per test-source-gate-symmetry rule: production symbol lives in
 * src/channels/telegram_reactions.c (gated by HU_ENABLE_TELEGRAM in
 * CMakeLists.txt). We use the internal-#ifdef-wrap-with-stub-runner
 * pattern: the test bodies are gated on HU_ENABLE_TELEGRAM so non-telegram
 * build variants link cleanly. */

#include "test_framework.h"

#ifdef HU_ENABLE_TELEGRAM

#include "human/channels/reaction_event.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdlib.h>
#include <string.h>

/* Forward decl — symbol exposed only as a function from
 * telegram_reactions.c (no header) to keep the public surface
 * unchanged. */
hu_error_t hu_telegram_handle_reaction_event_for_test(const char *body, size_t body_len,
                                                      hu_allocator_t *alloc,
                                                      const char *bot_user_id,
                                                      hu_reaction_event_t *out_events, size_t cap,
                                                      size_t *out_n);

static void free_events(hu_reaction_event_t *e, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free((void *)e[i].target_thread_id);
        free((void *)e[i].target_message_ref);
        free((void *)e[i].sender_handle);
        free((void *)e[i].emoji);
    }
}

/* Helper: run the parser on a payload and return the event count. */
static hu_error_t parse(const char *payload, const char *bot_id, hu_reaction_event_t *out,
                        size_t cap, size_t *n) {
    hu_allocator_t alloc = hu_system_allocator();
    return hu_telegram_handle_reaction_event_for_test(payload, strlen(payload), &alloc, bot_id, out,
                                                      cap, n);
}

/* old=[] new=[❤️] → one ADD event, kind=LOVE */
static void test_telegram_reaction_add_heart_emits_love(void) {
    const char *payload =
        "{\"update_id\":1,\"message_reaction\":{"
        "\"chat\":{\"id\":-1001234567890,\"type\":\"supergroup\"},"
        "\"message_id\":4567,"
        "\"user\":{\"id\":7890,\"username\":\"alice\"},"
        "\"date\":1730000000,"
        "\"old_reaction\":[],"
        "\"new_reaction\":[{\"type\":\"emoji\",\"emoji\":\"\xe2\x9d\xa4\xef\xb8\x8f\"}]"
        "}}";
    hu_reaction_event_t e[4] = {0};
    size_t n = 0;
    HU_ASSERT_EQ(parse(payload, "999", e, 4, &n), HU_OK);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ((int)e[0].kind, (int)HU_REACTION_LOVE);
    HU_ASSERT_EQ((int)e[0].polarity, (int)HU_REACTION_POSITIVE);
    HU_ASSERT_EQ(e[0].is_removal, 0);
    HU_ASSERT_STR_EQ(e[0].channel_id, "telegram");
    HU_ASSERT_STR_EQ(e[0].target_thread_id, "-1001234567890");
    HU_ASSERT_STR_EQ(e[0].target_message_ref, "4567");
    HU_ASSERT_STR_EQ(e[0].sender_handle, "alice");
    HU_ASSERT_EQ(e[0].timestamp_unix, (int64_t)1730000000);
    free_events(e, n);
}

/* old=[👍] new=[❤️] → one REMOVE 👍 + one ADD ❤️ */
static void test_telegram_reaction_replace_emits_remove_and_add(void) {
    const char *payload =
        "{\"message_reaction\":{"
        "\"chat\":{\"id\":100,\"type\":\"private\"},"
        "\"message_id\":1,"
        "\"user\":{\"id\":2,\"username\":\"bob\"},"
        "\"date\":1000,"
        "\"old_reaction\":[{\"type\":\"emoji\",\"emoji\":\"\xf0\x9f\x91\x8d\"}],"
        "\"new_reaction\":[{\"type\":\"emoji\",\"emoji\":\"\xe2\x9d\xa4\xef\xb8\x8f\"}]"
        "}}";
    hu_reaction_event_t e[4] = {0};
    size_t n = 0;
    HU_ASSERT_EQ(parse(payload, NULL, e, 4, &n), HU_OK);
    HU_ASSERT_EQ(n, 2u);
    /* Order: ADDs are emitted before REMOVEs by the implementation. */
    HU_ASSERT_EQ((int)e[0].kind, (int)HU_REACTION_LOVE);
    HU_ASSERT_EQ(e[0].is_removal, 0);
    HU_ASSERT_EQ((int)e[1].kind, (int)HU_REACTION_LIKE);
    HU_ASSERT_EQ(e[1].is_removal, 1);
    free_events(e, n);
}

/* old=[❤️] new=[] → one REMOVE ❤️ */
static void test_telegram_reaction_clear_emits_remove(void) {
    const char *payload =
        "{\"message_reaction\":{"
        "\"chat\":{\"id\":5,\"type\":\"private\"},"
        "\"message_id\":10,"
        "\"user\":{\"id\":2,\"username\":\"bob\"},"
        "\"date\":1000,"
        "\"old_reaction\":[{\"type\":\"emoji\",\"emoji\":\"\xe2\x9d\xa4\xef\xb8\x8f\"}],"
        "\"new_reaction\":[]"
        "}}";
    hu_reaction_event_t e[4] = {0};
    size_t n = 0;
    HU_ASSERT_EQ(parse(payload, NULL, e, 4, &n), HU_OK);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ((int)e[0].kind, (int)HU_REACTION_LOVE);
    HU_ASSERT_EQ(e[0].is_removal, 1);
    free_events(e, n);
}

/* old=[❤️] new=[❤️] → zero events (no change) */
static void test_telegram_reaction_no_change_emits_nothing(void) {
    const char *payload =
        "{\"message_reaction\":{"
        "\"chat\":{\"id\":1,\"type\":\"private\"},"
        "\"message_id\":1,"
        "\"user\":{\"id\":2,\"username\":\"bob\"},"
        "\"date\":1000,"
        "\"old_reaction\":[{\"type\":\"emoji\",\"emoji\":\"\xe2\x9d\xa4\xef\xb8\x8f\"}],"
        "\"new_reaction\":[{\"type\":\"emoji\",\"emoji\":\"\xe2\x9d\xa4\xef\xb8\x8f\"}]"
        "}}";
    hu_reaction_event_t e[4] = {0};
    size_t n = 0;
    HU_ASSERT_EQ(parse(payload, NULL, e, 4, &n), HU_OK);
    HU_ASSERT_EQ(n, 0u);
    free_events(e, n);
}

/* old=[] new=[custom_emoji "555"] → one ADD with CUSTOM_EMOJI + emoji="555" */
static void test_telegram_reaction_custom_emoji_id_preserved(void) {
    const char *payload =
        "{\"message_reaction\":{"
        "\"chat\":{\"id\":1,\"type\":\"private\"},"
        "\"message_id\":1,"
        "\"user\":{\"id\":2,\"username\":\"bob\"},"
        "\"date\":1000,"
        "\"old_reaction\":[],"
        "\"new_reaction\":[{\"type\":\"custom_emoji\",\"custom_emoji_id\":\"555\"}]"
        "}}";
    hu_reaction_event_t e[4] = {0};
    size_t n = 0;
    HU_ASSERT_EQ(parse(payload, NULL, e, 4, &n), HU_OK);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ((int)e[0].kind, (int)HU_REACTION_KIND_CUSTOM_EMOJI);
    HU_ASSERT_STR_EQ(e[0].emoji, "555");
    HU_ASSERT_EQ(e[0].is_removal, 0);
    free_events(e, n);
}

/* Self-reaction (user.id == bot_user_id) → filtered */
static void test_telegram_reaction_self_is_dropped(void) {
    const char *payload =
        "{\"message_reaction\":{"
        "\"chat\":{\"id\":1,\"type\":\"private\"},"
        "\"message_id\":1,"
        "\"user\":{\"id\":4242,\"username\":\"mybot\"},"
        "\"date\":1000,"
        "\"old_reaction\":[],"
        "\"new_reaction\":[{\"type\":\"emoji\",\"emoji\":\"\xe2\x9d\xa4\xef\xb8\x8f\"}]"
        "}}";
    hu_reaction_event_t e[4] = {0};
    size_t n = 0;
    HU_ASSERT_EQ(parse(payload, "4242", e, 4, &n), HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(n, 0u);
}

/* Malformed JSON → absorbed, no crash */
static void test_telegram_reaction_malformed_json_absorbed(void) {
    const char *payload = "{not json";
    hu_reaction_event_t e[4] = {0};
    size_t n = 0;
    HU_ASSERT_EQ(parse(payload, NULL, e, 4, &n), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(n, 0u);
}

/* Non-message_reaction update → NOT_SUPPORTED (callers fall through). */
static void test_telegram_reaction_non_reaction_update_falls_through(void) {
    const char *payload = "{\"update_id\":1,\"message\":{\"text\":\"hi\"}}";
    hu_reaction_event_t e[4] = {0};
    size_t n = 0;
    HU_ASSERT_EQ(parse(payload, NULL, e, 4, &n), HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(n, 0u);
}

/* Multiple emoji in new_reaction → multiple ADD events */
static void test_telegram_reaction_multiple_adds(void) {
    const char *payload = "{\"message_reaction\":{"
                          "\"chat\":{\"id\":1,\"type\":\"private\"},"
                          "\"message_id\":1,"
                          "\"user\":{\"id\":2,\"username\":\"bob\"},"
                          "\"date\":1000,"
                          "\"old_reaction\":[],"
                          "\"new_reaction\":["
                          "{\"type\":\"emoji\",\"emoji\":\"\xe2\x9d\xa4\xef\xb8\x8f\"},"
                          "{\"type\":\"emoji\",\"emoji\":\"\xf0\x9f\x91\x8d\"}"
                          "]}}";
    hu_reaction_event_t e[4] = {0};
    size_t n = 0;
    HU_ASSERT_EQ(parse(payload, NULL, e, 4, &n), HU_OK);
    HU_ASSERT_EQ(n, 2u);
    HU_ASSERT_EQ((int)e[0].kind, (int)HU_REACTION_LOVE);
    HU_ASSERT_EQ((int)e[1].kind, (int)HU_REACTION_LIKE);
    free_events(e, n);
}

/* Unknown emoji "🔥" (U+1F525) → CUSTOM_EMOJI kind, glyph preserved on evt.emoji */
static void test_telegram_reaction_unknown_emoji_is_custom(void) {
    const char *payload = "{\"message_reaction\":{"
                          "\"chat\":{\"id\":1,\"type\":\"private\"},"
                          "\"message_id\":1,"
                          "\"user\":{\"id\":2,\"username\":\"bob\"},"
                          "\"date\":1000,"
                          "\"old_reaction\":[],"
                          "\"new_reaction\":[{\"type\":\"emoji\",\"emoji\":\"\xf0\x9f\x94\xa5\"}]"
                          "}}";
    hu_reaction_event_t e[4] = {0};
    size_t n = 0;
    HU_ASSERT_EQ(parse(payload, NULL, e, 4, &n), HU_OK);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ((int)e[0].kind, (int)HU_REACTION_KIND_CUSTOM_EMOJI);
    HU_ASSERT_STR_EQ(e[0].emoji, "\xf0\x9f\x94\xa5");
    free_events(e, n);
}

/* Thumbs-down → DISLIKE / NEGATIVE */
static void test_telegram_reaction_thumbs_down_is_dislike(void) {
    const char *payload = "{\"message_reaction\":{"
                          "\"chat\":{\"id\":1,\"type\":\"private\"},"
                          "\"message_id\":1,"
                          "\"user\":{\"id\":2,\"username\":\"bob\"},"
                          "\"date\":1000,"
                          "\"old_reaction\":[],"
                          "\"new_reaction\":[{\"type\":\"emoji\",\"emoji\":\"\xf0\x9f\x91\x8e\"}]"
                          "}}";
    hu_reaction_event_t e[4] = {0};
    size_t n = 0;
    HU_ASSERT_EQ(parse(payload, NULL, e, 4, &n), HU_OK);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ((int)e[0].kind, (int)HU_REACTION_DISLIKE);
    HU_ASSERT_EQ((int)e[0].polarity, (int)HU_REACTION_NEGATIVE);
    free_events(e, n);
}

/* 😂 → LAUGH */
static void test_telegram_reaction_joy_is_laugh(void) {
    const char *payload = "{\"message_reaction\":{"
                          "\"chat\":{\"id\":1,\"type\":\"private\"},"
                          "\"message_id\":1,"
                          "\"user\":{\"id\":2,\"username\":\"bob\"},"
                          "\"date\":1000,"
                          "\"old_reaction\":[],"
                          "\"new_reaction\":[{\"type\":\"emoji\",\"emoji\":\"\xf0\x9f\x98\x82\"}]"
                          "}}";
    hu_reaction_event_t e[4] = {0};
    size_t n = 0;
    HU_ASSERT_EQ(parse(payload, NULL, e, 4, &n), HU_OK);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ((int)e[0].kind, (int)HU_REACTION_LAUGH);
    HU_ASSERT_EQ((int)e[0].polarity, (int)HU_REACTION_POSITIVE);
    free_events(e, n);
}

/* Question marks → QUESTION (neutral) */
static void test_telegram_reaction_question_is_neutral(void) {
    const char *payload = "{\"message_reaction\":{"
                          "\"chat\":{\"id\":1,\"type\":\"private\"},"
                          "\"message_id\":1,"
                          "\"user\":{\"id\":2,\"username\":\"bob\"},"
                          "\"date\":1000,"
                          "\"old_reaction\":[],"
                          "\"new_reaction\":[{\"type\":\"emoji\",\"emoji\":\"\xe2\x9d\x93\"}]"
                          "}}";
    hu_reaction_event_t e[4] = {0};
    size_t n = 0;
    HU_ASSERT_EQ(parse(payload, NULL, e, 4, &n), HU_OK);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ((int)e[0].kind, (int)HU_REACTION_KIND_QUESTION);
    HU_ASSERT_EQ((int)e[0].polarity, (int)HU_REACTION_NEUTRAL);
    free_events(e, n);
}

void run_telegram_reactions_tests(void) {
    HU_TEST_SUITE("telegram_reactions");
    HU_RUN_TEST(test_telegram_reaction_add_heart_emits_love);
    HU_RUN_TEST(test_telegram_reaction_replace_emits_remove_and_add);
    HU_RUN_TEST(test_telegram_reaction_clear_emits_remove);
    HU_RUN_TEST(test_telegram_reaction_no_change_emits_nothing);
    HU_RUN_TEST(test_telegram_reaction_custom_emoji_id_preserved);
    HU_RUN_TEST(test_telegram_reaction_self_is_dropped);
    HU_RUN_TEST(test_telegram_reaction_malformed_json_absorbed);
    HU_RUN_TEST(test_telegram_reaction_non_reaction_update_falls_through);
    HU_RUN_TEST(test_telegram_reaction_multiple_adds);
    HU_RUN_TEST(test_telegram_reaction_unknown_emoji_is_custom);
    HU_RUN_TEST(test_telegram_reaction_thumbs_down_is_dislike);
    HU_RUN_TEST(test_telegram_reaction_joy_is_laugh);
    HU_RUN_TEST(test_telegram_reaction_question_is_neutral);
}

#else /* !HU_ENABLE_TELEGRAM — stub runner so the symbol resolves */

void run_telegram_reactions_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_TELEGRAM */
