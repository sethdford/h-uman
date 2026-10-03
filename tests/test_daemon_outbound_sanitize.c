/* test_daemon_outbound_sanitize.c — the three pieces moved out of daemon.c so
 * the replay harness shares them: the pre-split sanitizer, the length
 * calibration (step 2c) and the director silence override. Each test pins
 * the behaviour daemon.c had inline. */
#include "human/agent.h"
#include "human/channel.h"
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

static void join_ack_sends_only_the_ack_when_reasoning_emptied_the_reply(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char small[4] = "(x)"; /* all reasoning, buffer too small for the fallback */
    size_t slen = 3;
    hu_daemon_outbound_sanitize(small, &slen, sizeof(small), true, NULL);
    HU_ASSERT_EQ(slen, 0);
    size_t n = 0;
    char *joined = hu_daemon_join_ack(&alloc, "sorry just saw this", small, slen, &n);
    HU_ASSERT_NOT_NULL(joined);
    HU_ASSERT_STR_EQ(joined, "sorry just saw this");
    HU_ASSERT_EQ(n, strlen("sorry just saw this"));
    alloc.free(alloc.ctx, joined, n + 1);

    joined = hu_daemon_join_ack(&alloc, "sorry just saw this", "yeah", 4, &n);
    HU_ASSERT_STR_EQ(joined, "sorry just saw this\n\nyeah");
    alloc.free(alloc.ctx, joined, n + 1);
}

static void length_calibration_appends_after_existing_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    const char *msg = "are you coming tonight?";
    char expect[1024];
    size_t expect_len = hu_conversation_calibrate_length_capped(
        msg, strlen(msg), false, NULL, agent.relationship.stage, 0, expect, sizeof(expect));
    HU_ASSERT_GT(expect_len, 0);

    char *ctx = NULL;
    size_t ctx_len = 0;
    hu_daemon_append_length_calibration(&alloc, &agent, "+1", 2, msg, strlen(msg), false, 0, &ctx,
                                        &ctx_len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_EQ(ctx_len, expect_len);
    HU_ASSERT_EQ(memcmp(ctx, expect, expect_len), 0);

    hu_daemon_append_length_calibration(&alloc, &agent, "+1", 2, msg, strlen(msg), false, 0, &ctx,
                                        &ctx_len);
    HU_ASSERT_EQ(ctx_len, expect_len * 2 + 2);
    HU_ASSERT_EQ(memcmp(ctx + expect_len, "\n\n", 2), 0);
    alloc.free(alloc.ctx, ctx, ctx_len + 1);
}

static hu_error_t budget_constraints(void *ctx, hu_channel_response_constraints_t *out) {
    out->max_chars = *(const uint32_t *)ctx;
    return HU_OK;
}

static uint32_t budget_cap(const hu_agent_t *agent, hu_channel_t *ch, size_t in_len, bool brief) {
    char in[64];
    memset(in, 'a', sizeof(in));
    hu_length_turn_result_t r;
    hu_daemon_reply_budget(agent, ch, "+1", 2, in, in_len, false, brief, &r);
    return r.cap;
}

static void reply_budget_takes_the_smallest_of_channel_ratio_and_brief(void) {
    /* HU_LENGTH_POLICY off (unset): the pre-move daemon.c step 4, exactly. */
    const char *prev = getenv("HU_LENGTH_POLICY");
    char saved[32] = "";
    if (prev)
        snprintf(saved, sizeof(saved), "%s", prev);
    unsetenv("HU_LENGTH_POLICY");
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    uint32_t chan_cap = 200;
    hu_channel_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.get_response_constraints = budget_constraints;
    hu_channel_t ch = {.ctx = &chan_cap, .vtable = &vt};
    const size_t in_len = 20;
    int rel = hu_conversation_max_response_chars_relational(in_len, NULL, agent.relationship.stage);
    uint32_t want = (rel > 0 && (uint32_t)rel < chan_cap) ? (uint32_t)rel : chan_cap;
    uint32_t got = budget_cap(&agent, &ch, in_len, false);
    uint32_t brief = hu_conversation_brief_char_cap(false, NULL, agent.relationship.stage);
    uint32_t want_brief = want > brief ? brief : want;
    uint32_t got_brief = budget_cap(&agent, &ch, in_len, true);
    chan_cap = 10; /* a tighter channel wins */
    uint32_t got_tight = budget_cap(&agent, &ch, in_len, false);
    hu_length_turn_result_t none;
    hu_daemon_reply_budget(NULL, &ch, "+1", 2, "x", 1, false, false, &none);
    if (prev)
        setenv("HU_LENGTH_POLICY", saved, 1);
    HU_ASSERT_EQ(got, want);
    HU_ASSERT_EQ(got_brief, want_brief);
    HU_ASSERT_EQ(got_tight, 10);
    HU_ASSERT_EQ(none.cap, 0);
}

static void director_decide_runs_the_director(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_director_result_t r;
    memset(&r, 0, sizeof(r));
    /* The test-build director maps a bare "k" to a thumbs-up tapback. */
    HU_ASSERT_TRUE(hu_daemon_director_decide(&alloc, NULL, NULL, "+1", 2, "k", 1, NULL, 0, "", &r));
    HU_ASSERT_EQ(r.action, DIR_TAPBACK);
    HU_ASSERT_EQ(r.reaction, HU_REACTION_THUMBS_UP);
    HU_ASSERT_TRUE(hu_daemon_director_decide(&alloc, NULL, NULL, "+1", 2, "what's up tonight", 17,
                                             NULL, 0, "", &r));
    HU_ASSERT_EQ(r.action, DIR_TEXT);
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
    HU_RUN_TEST(join_ack_sends_only_the_ack_when_reasoning_emptied_the_reply);
    HU_RUN_TEST(length_calibration_appends_after_existing_context);
    HU_RUN_TEST(director_silence_overridden_for_questions_and_short_greetings);
    HU_RUN_TEST(reply_budget_takes_the_smallest_of_channel_ratio_and_brief);
    HU_RUN_TEST(director_decide_runs_the_director);
}
