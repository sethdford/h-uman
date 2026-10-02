/* test_daemon_outbound_sanitize.c — the three pieces moved out of daemon.c so
 * the replay harness shares them: the pre-split sanitizer, the length
 * calibration (step 2c) and the director silence override. Each test pins
 * the behaviour daemon.c had inline. */
#include "human/agent.h"
#include "human/context/conversation.h"
#include "human/core/allocator.h"
#include "human/daemon/director.h"
#include "human/daemon/outbound_sanitize.h"
#include "human/daemon/reactive_calibration.h"
#include "test_framework.h"

#include <string.h>

static void sanitize_strips_invalid_utf8_and_controls_keeps_emoji(void) {
    char buf[64];
    /* "hi" BEL "x" <lone 0xff> <encoded surrogate ED A0 80> " " <😀> "\n" */
    const char in[] = "hi\ax\xff\xed\xa0\x80 \xf0\x9f\x98\x80\n";
    memcpy(buf, in, sizeof(in));
    size_t len = sizeof(in) - 1;
    hu_daemon_outbound_sanitize(buf, &len, sizeof(buf), false, NULL);
    HU_ASSERT_EQ(len, 9);
    HU_ASSERT_EQ(memcmp(buf, "hix \xf0\x9f\x98\x80\n", 9), 0);
}

static void sanitize_meta_reasoning_only_under_llm_decides(void) {
    char a[64] = "(they seem tired, keep it short) yeah get some sleep";
    size_t alen = strlen(a);
    hu_daemon_outbound_sanitize(a, &alen, sizeof(a), false, NULL);
    HU_ASSERT_EQ(alen, strlen("(they seem tired, keep it short) yeah get some sleep"));

    hu_daemon_outbound_sanitize(a, &alen, sizeof(a), true, NULL);
    HU_ASSERT_STR_EQ(a, "yeah get some sleep");
    HU_ASSERT_EQ(alen, strlen("yeah get some sleep"));
}

static void sanitize_all_reasoning_falls_back_only_when_it_fits(void) {
    char big[32] = "(just analysis)";
    size_t blen = strlen(big);
    hu_daemon_outbound_sanitize(big, &blen, sizeof(big), true, NULL);
    HU_ASSERT_STR_EQ(big, "hey whats up");
    char small[4] = "(x)";
    size_t slen = 3;
    hu_daemon_outbound_sanitize(small, &slen, sizeof(small), true, NULL);
    HU_ASSERT_EQ(slen, 0);
    HU_ASSERT_EQ(small[0], '\0');
}

static void length_calibration_appends_after_existing_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    const char *msg = "are you coming tonight?";
    char expect[1024];
    size_t expect_len = hu_conversation_calibrate_length_for_contact(
        msg, strlen(msg), NULL, 0, false, NULL, agent.relationship.stage, expect, sizeof(expect));
    HU_ASSERT_GT(expect_len, 0);

    char *ctx = NULL;
    size_t ctx_len = 0;
    hu_daemon_append_length_calibration(&alloc, &agent, "+1", 2, msg, strlen(msg), false, &ctx,
                                        &ctx_len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_EQ(ctx_len, expect_len);
    HU_ASSERT_EQ(memcmp(ctx, expect, expect_len), 0);

    hu_daemon_append_length_calibration(&alloc, &agent, "+1", 2, msg, strlen(msg), false, &ctx,
                                        &ctx_len);
    HU_ASSERT_EQ(ctx_len, expect_len * 2 + 2);
    HU_ASSERT_EQ(memcmp(ctx + expect_len, "\n\n", 2), 0);
    alloc.free(alloc.ctx, ctx, ctx_len + 1);
}

static void director_silence_overridden_for_questions_and_short_greetings(void) {
    const char *q = "did you get my email about the long thing from last week?";
    HU_ASSERT_TRUE(hu_daemon_director_silence_overridden(q, strlen(q)));
    HU_ASSERT_TRUE(hu_daemon_director_silence_overridden("hey", 3));
    HU_ASSERT_FALSE(hu_daemon_director_silence_overridden("ok cool", 7));
    const char *long_hey = "hey so that whole thing happened again today at work";
    HU_ASSERT_FALSE(hu_daemon_director_silence_overridden(long_hey, strlen(long_hey)));
    HU_ASSERT_FALSE(hu_daemon_director_silence_overridden(NULL, 0));
}

void run_daemon_outbound_sanitize_tests(void) {
    HU_TEST_SUITE("daemon_outbound_sanitize");
    HU_RUN_TEST(sanitize_strips_invalid_utf8_and_controls_keeps_emoji);
    HU_RUN_TEST(sanitize_meta_reasoning_only_under_llm_decides);
    HU_RUN_TEST(sanitize_all_reasoning_falls_back_only_when_it_fits);
    HU_RUN_TEST(length_calibration_appends_after_existing_context);
    HU_RUN_TEST(director_silence_overridden_for_questions_and_short_greetings);
}
