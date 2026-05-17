/* tests/test_slack_reactions.c
 *
 * Phase 2 Task 12 (RL SOTA): unit tests for the Slack reaction_added /
 * reaction_removed webhook branch. The branch lives in
 * src/channels/slack_reactions.c (split out from slack.c for the same
 * enum-name collision reason as Task 11's imessage_reactions.c — see
 * the breadcrumb in slack_reactions.c for the full story).
 *
 * Test contract: hu_slack_handle_reaction_event_for_test returns
 *   - HU_OK on a valid reaction (event filled).
 *   - HU_ERR_NOT_SUPPORTED when a filter rejects the event (self-reaction,
 *     non-message item).
 *   - HU_ERR_INVALID_ARGUMENT on parse / shape errors.
 * The inline webhook branch in slack.c silently absorbs filter rejections
 * as HU_OK acks (Slack retries on non-200 within 3s); the test helper has
 * the stricter return contract so these tests can assert on filter
 * behavior. Both contracts are intentional. */
#include "test_framework.h"
#include "human/channels/reaction_event.h"
#include "human/core/allocator.h"
#include <stdlib.h>

/* Forward decl — symbol is exposed only as a function from slack_reactions.c
 * (no header) to keep the public surface unchanged. */
hu_error_t hu_slack_handle_reaction_event_for_test(const char *json_payload,
                                                    hu_allocator_t *alloc,
                                                    const char *bot_user_id,
                                                    hu_reaction_event_t *out);

static void test_slack_reaction_added_thumbsup_emits_event(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *payload =
        "{\"event\": {\"type\": \"reaction_added\", \"reaction\": \"+1\", "
        "\"user\": \"U_REAL_USER\", \"item\": {\"type\": \"message\", "
        "\"channel\": \"C_TEST\", \"ts\": \"1234567890.123\"}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_slack_handle_reaction_event_for_test(payload, &alloc, "U_BOT", &e), HU_OK);
    HU_ASSERT_EQ(e.kind, HU_REACTION_LIKE);
    HU_ASSERT_EQ(e.polarity, HU_REACTION_POSITIVE);
    HU_ASSERT_STR_EQ(e.channel_id, "slack");
    HU_ASSERT_STR_EQ(e.target_thread_id, "C_TEST");
    HU_ASSERT_STR_EQ(e.target_message_ref, "1234567890.123");
    HU_ASSERT_EQ(e.is_removal, 0);
    /* free strdup'd fields */
    free((void *)e.target_thread_id);
    free((void *)e.target_message_ref);
    free((void *)e.sender_handle);
}

static void test_slack_reaction_from_bot_self_is_dropped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *payload =
        "{\"event\": {\"type\": \"reaction_added\", \"reaction\": \"+1\", "
        "\"user\": \"U_BOT\", \"item\": {\"type\": \"message\", "
        "\"channel\": \"C_TEST\", \"ts\": \"1234567890.123\"}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_slack_handle_reaction_event_for_test(payload, &alloc, "U_BOT", &e), HU_ERR_NOT_SUPPORTED);
}

static void test_slack_reaction_on_file_item_is_dropped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *payload =
        "{\"event\": {\"type\": \"reaction_added\", \"reaction\": \"+1\", "
        "\"user\": \"U_REAL_USER\", \"item\": {\"type\": \"file\", \"file\": \"F_X\"}}}";
    hu_reaction_event_t e = {0};
    HU_ASSERT_EQ(hu_slack_handle_reaction_event_for_test(payload, &alloc, "U_BOT", &e), HU_ERR_NOT_SUPPORTED);
}

void run_slack_reactions_tests(void) {
    HU_TEST_SUITE("slack_reactions");
    HU_RUN_TEST(test_slack_reaction_added_thumbsup_emits_event);
    HU_RUN_TEST(test_slack_reaction_from_bot_self_is_dropped);
    HU_RUN_TEST(test_slack_reaction_on_file_item_is_dropped);
}
