/* tests/test_matrix_reactions.c
 *
 * Phase 2 of docs/plans/2026-05-18-imessage-sota.md: unit tests for
 * Matrix m.reaction /sync-event branch. Branch lives in
 * src/channels/matrix_reactions.c (split out for the same enum-name
 * collision reason as the other reaction emit files).
 *
 * Phase-1 scope cut: REMOVE reactions (delivered as m.room.redaction
 * referencing a prior m.reaction event_id) are out of scope. Only
 * ADD events are emitted; is_removal is always 0.
 *
 * Per test-source-gate-symmetry rule: production symbol lives in
 * src/channels/matrix_reactions.c (gated by HU_ENABLE_MATRIX in
 * CMakeLists.txt). Internal-#ifdef-wrap-with-stub-runner pattern. */

#include "test_framework.h"

#ifdef HU_ENABLE_MATRIX

#include "human/channels/reaction_event.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdlib.h>
#include <string.h>

/* Forward decl — symbol exposed only as a function (no header). The
 * test helper accepts a full JSON body for ergonomics. The production
 * symbol takes a parsed event + room_id since matrix.c parses the
 * /sync response once and walks events with room_id already in scope. */
hu_error_t hu_matrix_handle_reaction_event_for_test(const char *body, size_t body_len,
                                                    hu_allocator_t *alloc, const char *bot_user_id,
                                                    hu_reaction_event_t *out);

static void free_evt(hu_reaction_event_t *e) {
    free((void *)e->target_thread_id);
    free((void *)e->target_message_ref);
    free((void *)e->sender_handle);
    free((void *)e->emoji);
}

static void test_matrix_reaction_heart_emits_love(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* ❤ = \xe2\x9d\xa4 */
    const char *payload = "{\"type\":\"m.reaction\","
                          "\"sender\":\"@alice:example.com\","
                          "\"room_id\":\"!room:example.com\","
                          "\"origin_server_ts\":1730000000000,"
                          "\"event_id\":\"$react1\","
                          "\"content\":{\"m.relates_to\":{"
                          "\"rel_type\":\"m.annotation\","
                          "\"event_id\":\"$target1\","
                          "\"key\":\"\xe2\x9d\xa4\"}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(
        hu_matrix_handle_reaction_event_for_test(payload, 0, &alloc, "@bot:example.com", &e),
        HU_OK);
    HU_ASSERT_EQ(e.kind, HU_REACTION_LOVE);
    HU_ASSERT_EQ(e.polarity, HU_REACTION_POSITIVE);
    HU_ASSERT_STR_EQ(e.channel_id, "matrix");
    HU_ASSERT_STR_EQ(e.target_thread_id, "!room:example.com");
    HU_ASSERT_STR_EQ(e.target_message_ref, "$target1");
    HU_ASSERT_STR_EQ(e.sender_handle, "@alice:example.com");
    HU_ASSERT_EQ(e.is_removal, 0);
    /* ts is ms in matrix, we divide by 1000 */
    HU_ASSERT_EQ(e.timestamp_unix, (int64_t)1730000000);
    HU_ASSERT_NULL(e.emoji);
    free_evt(&e);
}

static void test_matrix_reaction_thumbsdown_emits_dislike(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* 👎 = \xf0\x9f\x91\x8e */
    const char *payload = "{\"type\":\"m.reaction\","
                          "\"sender\":\"@alice:example.com\","
                          "\"room_id\":\"!room:example.com\","
                          "\"origin_server_ts\":1730000001000,"
                          "\"content\":{\"m.relates_to\":{"
                          "\"rel_type\":\"m.annotation\","
                          "\"event_id\":\"$target2\","
                          "\"key\":\"\xf0\x9f\x91\x8e\"}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(
        hu_matrix_handle_reaction_event_for_test(payload, 0, &alloc, "@bot:example.com", &e),
        HU_OK);
    HU_ASSERT_EQ(e.kind, HU_REACTION_DISLIKE);
    HU_ASSERT_EQ(e.polarity, HU_REACTION_NEGATIVE);
    free_evt(&e);
}

static void test_matrix_custom_emoji_populates_emoji_field(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* Arbitrary shortcode — not in our normalizer table, treated as
     * CUSTOM_EMOJI with the key preserved on evt.emoji. */
    const char *payload = "{\"type\":\"m.reaction\","
                          "\"sender\":\"@alice:example.com\","
                          "\"room_id\":\"!room:example.com\","
                          "\"origin_server_ts\":1730000002000,"
                          "\"content\":{\"m.relates_to\":{"
                          "\"rel_type\":\"m.annotation\","
                          "\"event_id\":\"$target3\","
                          "\"key\":\":custom-emoji:\"}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(
        hu_matrix_handle_reaction_event_for_test(payload, 0, &alloc, "@bot:example.com", &e),
        HU_OK);
    HU_ASSERT_EQ(e.kind, HU_REACTION_KIND_CUSTOM_EMOJI);
    HU_ASSERT_NOT_NULL(e.emoji);
    HU_ASSERT_STR_EQ(e.emoji, ":custom-emoji:");
    free_evt(&e);
}

static void test_matrix_self_reaction_dropped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* sender == bot_user_id — self-reaction. */
    const char *payload = "{\"type\":\"m.reaction\","
                          "\"sender\":\"@bot:example.com\","
                          "\"room_id\":\"!room:example.com\","
                          "\"origin_server_ts\":1730000000000,"
                          "\"content\":{\"m.relates_to\":{"
                          "\"rel_type\":\"m.annotation\","
                          "\"event_id\":\"$target1\","
                          "\"key\":\"\xe2\x9d\xa4\"}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(
        hu_matrix_handle_reaction_event_for_test(payload, 0, &alloc, "@bot:example.com", &e),
        HU_ERR_NOT_SUPPORTED);
}

static void test_matrix_wrong_event_type_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* m.room.message is not a reaction. */
    const char *payload = "{\"type\":\"m.room.message\","
                          "\"sender\":\"@alice:example.com\","
                          "\"content\":{\"msgtype\":\"m.text\",\"body\":\"hi\"}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(
        hu_matrix_handle_reaction_event_for_test(payload, 0, &alloc, "@bot:example.com", &e),
        HU_ERR_INVALID_ARGUMENT);
}

static void test_matrix_wrong_rel_type_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* m.replace is for edits, not annotations — should not match. */
    const char *payload = "{\"type\":\"m.reaction\","
                          "\"sender\":\"@alice:example.com\","
                          "\"room_id\":\"!room:example.com\","
                          "\"origin_server_ts\":1730000000000,"
                          "\"content\":{\"m.relates_to\":{"
                          "\"rel_type\":\"m.replace\","
                          "\"event_id\":\"$target1\","
                          "\"key\":\"\xe2\x9d\xa4\"}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(
        hu_matrix_handle_reaction_event_for_test(payload, 0, &alloc, "@bot:example.com", &e),
        HU_ERR_INVALID_ARGUMENT);
}

static void test_matrix_malformed_json_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *payload = "{not json";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(
        hu_matrix_handle_reaction_event_for_test(payload, 0, &alloc, "@bot:example.com", &e),
        HU_ERR_INVALID_ARGUMENT);
}

static void test_matrix_missing_relates_to_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* m.reaction without m.relates_to is malformed. */
    const char *payload = "{\"type\":\"m.reaction\","
                          "\"sender\":\"@alice:example.com\","
                          "\"content\":{}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(
        hu_matrix_handle_reaction_event_for_test(payload, 0, &alloc, "@bot:example.com", &e),
        HU_ERR_INVALID_ARGUMENT);
}

static void test_matrix_question_emits_question(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* ❓ = \xe2\x9d\x93 */
    const char *payload = "{\"type\":\"m.reaction\","
                          "\"sender\":\"@alice:example.com\","
                          "\"room_id\":\"!room:example.com\","
                          "\"origin_server_ts\":1730000003000,"
                          "\"content\":{\"m.relates_to\":{"
                          "\"rel_type\":\"m.annotation\","
                          "\"event_id\":\"$target_q\","
                          "\"key\":\"\xe2\x9d\x93\"}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(
        hu_matrix_handle_reaction_event_for_test(payload, 0, &alloc, "@bot:example.com", &e),
        HU_OK);
    HU_ASSERT_EQ(e.kind, HU_REACTION_KIND_QUESTION);
    HU_ASSERT_EQ(e.polarity, HU_REACTION_NEUTRAL);
    free_evt(&e);
}

static void test_matrix_laugh_emits_laugh(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* 😂 = \xf0\x9f\x98\x82 */
    const char *payload = "{\"type\":\"m.reaction\","
                          "\"sender\":\"@alice:example.com\","
                          "\"room_id\":\"!room:example.com\","
                          "\"origin_server_ts\":1730000004000,"
                          "\"content\":{\"m.relates_to\":{"
                          "\"rel_type\":\"m.annotation\","
                          "\"event_id\":\"$target_l\","
                          "\"key\":\"\xf0\x9f\x98\x82\"}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(
        hu_matrix_handle_reaction_event_for_test(payload, 0, &alloc, "@bot:example.com", &e),
        HU_OK);
    HU_ASSERT_EQ(e.kind, HU_REACTION_LAUGH);
    free_evt(&e);
}

void run_matrix_reactions_tests(void) {
    HU_TEST_SUITE("matrix_reactions");
    HU_RUN_TEST(test_matrix_reaction_heart_emits_love);
    HU_RUN_TEST(test_matrix_reaction_thumbsdown_emits_dislike);
    HU_RUN_TEST(test_matrix_custom_emoji_populates_emoji_field);
    HU_RUN_TEST(test_matrix_self_reaction_dropped);
    HU_RUN_TEST(test_matrix_wrong_event_type_returns_invalid);
    HU_RUN_TEST(test_matrix_wrong_rel_type_returns_invalid);
    HU_RUN_TEST(test_matrix_malformed_json_returns_invalid);
    HU_RUN_TEST(test_matrix_missing_relates_to_returns_invalid);
    HU_RUN_TEST(test_matrix_question_emits_question);
    HU_RUN_TEST(test_matrix_laugh_emits_laugh);
}

#else /* !HU_ENABLE_MATRIX — stub runner so the symbol resolves */

void run_matrix_reactions_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_MATRIX */
