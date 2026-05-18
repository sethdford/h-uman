/* ─────────────────────────────────────────────────────────────────────────
 * test_imessage_react_contract.c
 *
 * Regression guard for the tapback (react) contract on iMessage.
 *
 * Background: a 2026-05-17 audit alleged "tapback has no fallback on modern
 * macOS — fails silently." Code inspection refuted both claims:
 *   1. The channel has a 3-tier fallback chain (imsg CLI → native AX → JXA
 *      subprocess); see src/channels/imessage.c imessage_react().
 *   2. The daemon catches react() failures and falls back to the LLM/text
 *      path; see src/daemon.c around line 9446 and line 9499.
 *   3. Each tier logs its own failure path so the chain is observable.
 *
 * Rather than land defensive code for a non-issue, this file locks the
 * test-mode contract that EVERY public reaction type round-trips through
 * react() without dropping. If a future refactor accidentally adds a switch
 * branch that doesn't handle some reaction kind (or drops null inputs into
 * UB), one of these tests fails before the change ships.
 * ───────────────────────────────────────────────────────────────────────── */

#if HU_HAS_IMESSAGE
#include "human/channel.h"
#include "human/channels/imessage.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"

#include <stddef.h>

/* Helper: build channel, call react with the given reaction + msg id, read
 * back the recorded reaction + msg id. */
static void react_and_check(hu_reaction_type_t reaction, int64_t msg_id) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15555550200", 12, NULL, 0, &ch), HU_OK);

    HU_ASSERT(ch.vtable->react != NULL);
    HU_ASSERT_EQ(ch.vtable->react(ch.ctx, "+15555550200", 12, msg_id, reaction), HU_OK);

    hu_reaction_type_t got_reaction = HU_REACTION_NONE;
    int64_t got_id = 0;
    hu_imessage_test_get_last_reaction(&ch, &got_reaction, &got_id);

    HU_ASSERT_EQ((int)got_reaction, (int)reaction);
    HU_ASSERT_EQ(got_id, msg_id);

    hu_imessage_destroy(&ch);
}

static void test_react_records_heart(void) {
    react_and_check(HU_REACTION_HEART, 100001);
}

static void test_react_records_thumbs_up(void) {
    react_and_check(HU_REACTION_THUMBS_UP, 100002);
}

static void test_react_records_thumbs_down(void) {
    react_and_check(HU_REACTION_THUMBS_DOWN, 100003);
}

static void test_react_records_haha(void) {
    react_and_check(HU_REACTION_HAHA, 100004);
}

static void test_react_records_emphasis(void) {
    react_and_check(HU_REACTION_EMPHASIS, 100005);
}

static void test_react_records_question(void) {
    react_and_check(HU_REACTION_QUESTION, 100006);
}

static void test_react_records_custom_emoji(void) {
    /* HU_REACTION_CUSTOM_EMOJI is a valid enum slot the channel must accept
     * without falling through to "unknown reaction" sinks. */
    react_and_check(HU_REACTION_CUSTOM_EMOJI, 100007);
}

static void test_react_null_ctx_returns_invalid_argument(void) {
    /* Defensive: react() with a NULL ctx must not crash. The test path
     * checks for NULL and returns HU_ERR_INVALID_ARGUMENT. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15555550201", 12, NULL, 0, &ch), HU_OK);

    HU_ASSERT_EQ(ch.vtable->react(NULL, "+15555550201", 12, 1, HU_REACTION_HEART),
                 HU_ERR_INVALID_ARGUMENT);

    hu_imessage_destroy(&ch);
}

static void test_react_reaction_none_is_recorded_as_none(void) {
    /* HU_REACTION_NONE is a valid input (e.g. the channel was asked to react
     * but the classifier returned no reaction). The contract: don't crash,
     * record the value faithfully. The caller decides whether to skip-send. */
    react_and_check(HU_REACTION_NONE, 100008);
}

static void test_react_overwrites_previous_reaction(void) {
    /* Pinned property: calling react() twice updates last_reaction to the
     * latest call's value. If a future change accumulates rather than
     * overwriting, the test reaction getter returns stale data. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch;
    HU_ASSERT_EQ(hu_imessage_create(&alloc, "+15555550202", 12, NULL, 0, &ch), HU_OK);

    HU_ASSERT_EQ(ch.vtable->react(ch.ctx, "+15555550202", 12, 1, HU_REACTION_HEART), HU_OK);
    HU_ASSERT_EQ(ch.vtable->react(ch.ctx, "+15555550202", 12, 2, HU_REACTION_THUMBS_DOWN), HU_OK);

    hu_reaction_type_t got_reaction = HU_REACTION_NONE;
    int64_t got_id = 0;
    hu_imessage_test_get_last_reaction(&ch, &got_reaction, &got_id);
    HU_ASSERT_EQ((int)got_reaction, (int)HU_REACTION_THUMBS_DOWN);
    HU_ASSERT_EQ(got_id, 2);

    hu_imessage_destroy(&ch);
}

void run_imessage_react_contract_tests(void);
void run_imessage_react_contract_tests(void) {
    HU_TEST_SUITE("iMessage react contract");

    HU_RUN_TEST(test_react_records_heart);
    HU_RUN_TEST(test_react_records_thumbs_up);
    HU_RUN_TEST(test_react_records_thumbs_down);
    HU_RUN_TEST(test_react_records_haha);
    HU_RUN_TEST(test_react_records_emphasis);
    HU_RUN_TEST(test_react_records_question);
    HU_RUN_TEST(test_react_records_custom_emoji);

    HU_RUN_TEST(test_react_null_ctx_returns_invalid_argument);
    HU_RUN_TEST(test_react_reaction_none_is_recorded_as_none);
    HU_RUN_TEST(test_react_overwrites_previous_reaction);
}

#else /* !HU_HAS_IMESSAGE */
void run_imessage_react_contract_tests(void);
void run_imessage_react_contract_tests(void) {
    /* iMessage channel not built — skip silently. */
}
#endif
