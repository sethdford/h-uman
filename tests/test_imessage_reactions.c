/* tests/test_imessage_reactions.c
 *
 * Phase 2 Task 11 (RL SOTA): unit test for hu_imessage_poll_reactions.
 *
 * Default mode: HU_HAVE_CHATDB undefined → the `_skipped` stub runs.
 * Opt-in mode: -DHU_HAVE_CHATDB=1 + HU_CHATDB env var → exercises the
 *   real SQLite path against a user-supplied chat.db.
 *
 * The forward declaration below is intentional: hu_imessage_poll_reactions
 * is defined in src/channels/imessage.c but not exposed via
 * include/human/channels/imessage.h (test-only consumer for now). */
#include "test_framework.h"
#include "human/channels/reaction_event.h"
#include <time.h>

hu_error_t hu_imessage_poll_reactions(const char *db_path,
                                      int64_t since_unix,
                                      hu_reaction_event_t *out_events,
                                      size_t out_cap,
                                      size_t *out_n);

#ifndef HU_HAVE_CHATDB
static void test_imessage_poll_reactions_skipped(void) {
    fprintf(stderr, "[skip] HU_HAVE_CHATDB not defined\n");
}
#else
static void test_imessage_poll_reactions_returns_recent_tapbacks(void) {
    hu_reaction_event_t events[16] = {0};
    size_t n = 0;
    hu_error_t err = hu_imessage_poll_reactions(getenv("HU_CHATDB"),
                                                time(NULL) - 86400,
                                                events, 16, &n);
    /* hu_imessage_poll_reactions strdup's target_thread_id, target_message_ref,
     * sender_handle into each event. MUST free or ASan reports leaks. */
    for (size_t i = 0; i < n; i++) {
        free((void *)events[i].target_thread_id);
        free((void *)events[i].target_message_ref);
        free((void *)events[i].sender_handle);
    }
    HU_ASSERT_EQ(err, HU_OK);
}
#endif

void run_imessage_reactions_tests(void) {
    HU_TEST_SUITE("imessage_reactions");
#ifdef HU_HAVE_CHATDB
    HU_RUN_TEST(test_imessage_poll_reactions_returns_recent_tapbacks);
#else
    HU_RUN_TEST(test_imessage_poll_reactions_skipped);
#endif
}
