/* test_outbound_shape.c — Sprint 59 shape stage contract.
 *
 * Shape validates message length + sentence structure. Returns
 * REGENERATE with a stricter hint when shape is wrong. Never REJECT
 * (the LLM regenerate is the right escape).
 *
 * Corpus coverage:
 *   - #1, #2, #3 — 60+ char cross-contact fragments → REGENERATE
 *   - #6        — 150-char [SAFETY] directive block → REGENERATE
 *   - #19..#24  — PASS cases must remain SEND
 */

#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

#include "human/agent/outbound_pipeline.h"
#include "human/core/allocator.h"

extern hu_outbound_stage_t hu_outbound_stage_shape;

static hu_allocator_t *test_alloc(void) {
    static hu_allocator_t a;
    static int init = 0;
    if (!init) {
        a = hu_system_allocator();
        init = 1;
    }
    return &a;
}

static hu_outbound_verdict_t run_shape(const char *content) {
    hu_outbound_message_t msg = {0};
    msg.content = (char *)content; /* shape doesn't mutate; const-cast safe */
    msg.content_len = strlen(content);

    hu_outbound_context_t ctx = {0};
    ctx.alloc = test_alloc();
    ctx.path = HU_OUTBOUND_PATH_PROACTIVE;
    ctx.regenerate_budget = 1;

    return hu_outbound_stage_shape.run(&hu_outbound_stage_shape, &msg, &ctx);
}

/* ----------------------------------------------------------------- */

static void test_shape_short_message_passes(void) {
    hu_outbound_verdict_t v = run_shape("how are you");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

static void test_shape_single_sentence_long_message_passes(void) {
    /* 90 chars, single question mark — legitimate. */
    hu_outbound_verdict_t v =
        run_shape("how are those funny looking dogs doing this summer in the back yard buddy boy?");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

static void test_shape_empty_returns_send(void) {
    hu_outbound_verdict_t v = run_shape("");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

static void test_shape_null_content_returns_send(void) {
    hu_outbound_message_t msg = {0};
    hu_outbound_context_t ctx = {0};
    ctx.alloc = test_alloc();
    hu_outbound_verdict_t v = hu_outbound_stage_shape.run(&hu_outbound_stage_shape, &msg, &ctx);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

/* Rule A — over 200 chars. */
static void test_shape_over_200_chars_regenerates(void) {
    char big[260];
    memset(big, 'x', 250);
    big[250] = '\0';
    hu_outbound_verdict_t v = run_shape(big);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REGENERATE);
    HU_ASSERT_STR_EQ(v.reason, "shape_too_long");
    HU_ASSERT_NOT_NULL(v.regenerate_hint);
}

/* Rule B — corpus #1, #2, #3: 60+ chars, multi-sentence cross-contact bleed. */
static void test_shape_corpus_cross_contact_bleed_regenerates(void) {
    /* The actual message that hit Annie/Mindy/Betty. 70 chars,
     * period + question mark. */
    hu_outbound_verdict_t v =
        run_shape("but boy I am just more lonely now than ever. I am skeptical ?");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REGENERATE);
    HU_ASSERT_STR_EQ(v.reason, "shape_multi_sentence_long");
}

/* Rule A — corpus #6: the [SAFETY] block, ~150 chars. Below 200 but
 * triggers Rule B (multi-sentence + long). */
static void test_shape_corpus_safety_block_regenerates(void) {
    hu_outbound_verdict_t v = run_shape("[SAFETY] This response touches on violence. De-escalate: "
                                        "acknowledge feelings without endorsing harm. Redirect "
                                        "toward constructive alternatives.");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REGENERATE);
}

/* Short multi-sentence text passes — "hey! how are you?" must work. */
static void test_shape_short_multi_sentence_passes(void) {
    hu_outbound_verdict_t v = run_shape("hey! how are you?");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

/* URL exception — long messages with URLs are allowed. */
static void test_shape_long_with_url_passes(void) {
    hu_outbound_verdict_t v = run_shape(
        "check this out — https://example.com/some/path. It's pretty cool what they're doing.");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

/* Ellipsis "..." counts as ONE terminator, not three. */
static void test_shape_ellipsis_counts_as_one(void) {
    /* 60 chars, single ellipsis → single sentence by the rule. */
    hu_outbound_verdict_t v =
        run_shape("well that's just how things go sometimes in the back yard...");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

/* Combo "?!" counts as ONE terminator. */
static void test_shape_question_exclam_combo_counts_as_one(void) {
    hu_outbound_verdict_t v =
        run_shape("did you really just say that whole long thing about the dogs?!");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

/* All corpus PASS cases pass. */
static void test_shape_corpus_pass_cases_send(void) {
    const char *pass_cases[] = {
        "how'd it go with the loan?",
        "you still getting that loan tomorrow?",
        "morning! How's the garden doing?",
        "how are those funny looking dogs doing?",
        "see any more funny looking dogs lately?",
        "how are you",
        NULL,
    };
    for (int i = 0; pass_cases[i]; i++) {
        hu_outbound_verdict_t v = run_shape(pass_cases[i]);
        HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
    }
}

void run_outbound_shape_tests(void) {
    HU_TEST_SUITE("outbound_shape");
    HU_RUN_TEST(test_shape_short_message_passes);
    HU_RUN_TEST(test_shape_single_sentence_long_message_passes);
    HU_RUN_TEST(test_shape_empty_returns_send);
    HU_RUN_TEST(test_shape_null_content_returns_send);
    HU_RUN_TEST(test_shape_over_200_chars_regenerates);
    HU_RUN_TEST(test_shape_corpus_cross_contact_bleed_regenerates);
    HU_RUN_TEST(test_shape_corpus_safety_block_regenerates);
    HU_RUN_TEST(test_shape_short_multi_sentence_passes);
    HU_RUN_TEST(test_shape_long_with_url_passes);
    HU_RUN_TEST(test_shape_ellipsis_counts_as_one);
    HU_RUN_TEST(test_shape_question_exclam_combo_counts_as_one);
    HU_RUN_TEST(test_shape_corpus_pass_cases_send);
}
