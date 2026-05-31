// @covers-none — tests hu_imessage_reply_should_quote_on_fallback from
// src/channels/imessage_action.c; the module-name heuristic resolves
// "imessage_reply_fallback_quote" to no single source, so opt out and assert
// the production symbol directly below.
#include "human/channels/imessage_action.h"
#include "test_framework.h"

/* Baseline = the "fresh, last, single-thread" context where a human would NOT
 * quote: the parent is the newest inbound message (position 0), it arrived
 * recently (<= 180s), and there is at most one pending question. Every case
 * below varies ONE field off this baseline so each test isolates exactly the
 * signal it claims to exercise. */
static hu_reply_style_facts_t no_quote_baseline(void) {
    hu_reply_style_facts_t f = {0};
    f.parent_position_from_bottom = 0; /* newest inbound */
    f.seconds_since_parent = 30;       /* fresh */
    f.pending_questions_in_window = 1; /* single, unambiguous */
    /* The remaining fields don't influence the fallback-quote decision; leave
     * them zero/default so the predicate's inputs are obvious. */
    f.persona_thread_affinity = 0.3f;
    f.persona_formality = 0.5f;
    return f;
}

/* Fresh reply to the message we were just sent → a plain reply is the human
 * shape; the `↩ "quote"` glyph would read as a bot. */
static void fresh_last_single_does_not_quote(void) {
    hu_reply_style_facts_t f = no_quote_baseline();
    HU_ASSERT_FALSE(hu_imessage_reply_should_quote_on_fallback(&f));
}

/* Parent is not the newest inbound (something arrived after it) → a bare reply
 * is ambiguous about which message we mean, so a human references it. */
static void non_newest_parent_quotes(void) {
    hu_reply_style_facts_t f = no_quote_baseline();
    f.parent_position_from_bottom = 1;
    HU_ASSERT_TRUE(hu_imessage_reply_should_quote_on_fallback(&f));

    f.parent_position_from_bottom = 5; /* scrolled well off → still quotes */
    HU_ASSERT_TRUE(hu_imessage_reply_should_quote_on_fallback(&f));
}

/* Stale parent (> 3 min): the conversation may have drifted, so a human
 * reorients the recipient by quoting what they're responding to. */
static void stale_parent_quotes(void) {
    hu_reply_style_facts_t f = no_quote_baseline();
    f.seconds_since_parent = 181; /* just past the 180s boundary */
    HU_ASSERT_TRUE(hu_imessage_reply_should_quote_on_fallback(&f));

    f.seconds_since_parent = 3600; /* an hour later → still quotes */
    HU_ASSERT_TRUE(hu_imessage_reply_should_quote_on_fallback(&f));
}

/* Exactly 180s is still "fresh" — the boundary is `> 180`, so 180 must NOT
 * quote. Pins the off-by-one. */
static void parent_at_180s_boundary_does_not_quote(void) {
    hu_reply_style_facts_t f = no_quote_baseline();
    f.seconds_since_parent = 180;
    HU_ASSERT_FALSE(hu_imessage_reply_should_quote_on_fallback(&f));
}

/* Two or more unresolved questions → the quote disambiguates which one this
 * reply answers. A single pending question (the baseline) does NOT quote. */
static void multiple_pending_questions_quote(void) {
    hu_reply_style_facts_t f = no_quote_baseline();
    f.pending_questions_in_window = 2;
    HU_ASSERT_TRUE(hu_imessage_reply_should_quote_on_fallback(&f));

    f.pending_questions_in_window = 1; /* single → no quote */
    HU_ASSERT_FALSE(hu_imessage_reply_should_quote_on_fallback(&f));
}

/* NULL facts → false (no quote). The plain body is always a safe send, so the
 * predicate must never demand a quote it has no basis to build. */
static void null_facts_does_not_quote(void) {
    HU_ASSERT_FALSE(hu_imessage_reply_should_quote_on_fallback(NULL));
}

/* A combined ambiguous context (non-newest AND stale AND many questions) still
 * quotes — the conditions are OR'd, not AND'd. */
static void combined_ambiguity_quotes(void) {
    hu_reply_style_facts_t f = no_quote_baseline();
    f.parent_position_from_bottom = 3;
    f.seconds_since_parent = 600;
    f.pending_questions_in_window = 4;
    HU_ASSERT_TRUE(hu_imessage_reply_should_quote_on_fallback(&f));
}

void run_imessage_reply_fallback_quote_tests(void) {
    HU_TEST_SUITE("imessage_reply_fallback_quote");
    HU_RUN_TEST(fresh_last_single_does_not_quote);
    HU_RUN_TEST(non_newest_parent_quotes);
    HU_RUN_TEST(stale_parent_quotes);
    HU_RUN_TEST(parent_at_180s_boundary_does_not_quote);
    HU_RUN_TEST(multiple_pending_questions_quote);
    HU_RUN_TEST(null_facts_does_not_quote);
    HU_RUN_TEST(combined_ambiguity_quotes);
}
