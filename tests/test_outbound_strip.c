/* test_outbound_strip.c — Sprint 59 strip stage contract.
 *
 * Strip is the character-normalization stage. It returns REWRITE
 * with stripped content when problematic codepoints are present,
 * SEND otherwise. Never REJECT — it's non-judgmental.
 *
 * Corpus coverage: the visible REJECT cases don't contain U+FFFC
 * directly, but the broader incident log evidence does. The strip
 * stage prevents that class of leak. PASS cases (#19-24) must NOT
 * be stripped — they're plain ASCII.
 */

#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

#include "human/agent/outbound_pipeline.h"
#include "human/core/allocator.h"

/* The stage singleton lives in src/agent/outbound/strip.c. */
extern hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_strip;

static hu_allocator_t *test_alloc(void) {
    static hu_allocator_t a;
    static int init = 0;
    if (!init) {
        a = hu_system_allocator();
        init = 1;
    }
    return &a;
}

/* Build a heap-allocated message buffer the test owns. */
static char *test_dup_n(hu_allocator_t *alloc, const char *s, size_t n) {
    char *buf = (char *)alloc->alloc(alloc->ctx, n + 1);
    HU_ASSERT_NOT_NULL(buf);
    memcpy(buf, s, n);
    buf[n] = '\0';
    return buf;
}

/* Run the strip stage on a string literal; caller is responsible
 * for freeing the verdict and the input. */
static hu_outbound_verdict_t run_strip(const char *content_literal) {
    hu_allocator_t *alloc = test_alloc();
    size_t n = strlen(content_literal);
    hu_outbound_message_t msg = {0};
    msg.content = test_dup_n(alloc, content_literal, n);
    msg.content_len = n;

    hu_outbound_context_t ctx = {0};
    ctx.alloc = alloc;
    ctx.path = HU_OUTBOUND_PATH_PROACTIVE;
    ctx.regenerate_budget = 1;

    hu_outbound_verdict_t v = hu_outbound_pipeline_stage_strip.run(&hu_outbound_pipeline_stage_strip, &msg, &ctx);

    alloc->free(alloc->ctx, msg.content, n + 1);
    return v;
}

/* ----------------------------------------------------------------- */
/* Tests                                                              */
/* ----------------------------------------------------------------- */

static void test_strip_plain_ascii_passes_through(void) {
    hu_outbound_verdict_t v = run_strip("hey how are you");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
    HU_ASSERT_NULL(v.replacement);
    hu_outbound_verdict_clear(&v, test_alloc());
}

static void test_strip_empty_string_returns_send(void) {
    hu_outbound_verdict_t v = run_strip("");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
    hu_outbound_verdict_clear(&v, test_alloc());
}

static void test_strip_null_content_returns_send(void) {
    hu_outbound_message_t msg = {0};
    hu_outbound_context_t ctx = {0};
    ctx.alloc = test_alloc();
    hu_outbound_verdict_t v = hu_outbound_pipeline_stage_strip.run(&hu_outbound_pipeline_stage_strip, &msg, &ctx);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

/* U+FFFC OBJECT REPLACEMENT — iMessage attachment placeholder. */
static void test_strip_u_fffc_rewrites_to_clean_content(void) {
    /* "hi mom\xEF\xBF\xBC look at this" — U+FFFC between "mom" and "look". */
    hu_outbound_verdict_t v = run_strip("hi mom\xEF\xBF\xBC look at this");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REWRITE);
    HU_ASSERT_STR_EQ(v.reason, "strip_codepoints");
    HU_ASSERT_NOT_NULL(v.replacement);
    HU_ASSERT_STR_EQ(v.replacement, "hi mom look at this");
    HU_ASSERT_EQ(v.replacement_len, strlen("hi mom look at this"));
    hu_outbound_verdict_clear(&v, test_alloc());
}

/* U+202E RIGHT-TO-LEFT OVERRIDE. */
static void test_strip_u_202e_rewrites_to_clean_content(void) {
    /* "evil\xE2\x80\xAEgpj.exe" — classic spoofing attack vector. */
    hu_outbound_verdict_t v = run_strip("evil\xE2\x80\xAEgpj.exe");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REWRITE);
    HU_ASSERT_STR_EQ(v.replacement, "evilgpj.exe");
    hu_outbound_verdict_clear(&v, test_alloc());
}

/* U+200B ZERO WIDTH SPACE. */
static void test_strip_u_200b_rewrites_to_clean_content(void) {
    hu_outbound_verdict_t v = run_strip("paypal\xE2\x80\x8B.com");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REWRITE);
    HU_ASSERT_STR_EQ(v.replacement, "paypal.com");
    hu_outbound_verdict_clear(&v, test_alloc());
}

/* Isolated U+200D ZWJ — not between emoji, must be stripped. */
static void test_strip_isolated_u_200d_rewrites(void) {
    /* ZWJ between two ASCII letters — definitely isolated. */
    hu_outbound_verdict_t v = run_strip("hi\xE2\x80\x8Dthere");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REWRITE);
    HU_ASSERT_STR_EQ(v.replacement, "hithere");
    hu_outbound_verdict_clear(&v, test_alloc());
}

/* Family emoji U+1F468 U+200D U+1F469 U+200D U+1F467 (man-woman-girl).
 * The ZWJs sit between 4-byte UTF-8 codepoints → must be PRESERVED. */
static void test_strip_emoji_zwj_sequence_preserved(void) {
    /* 👨‍👩‍👧 = F0 9F 91 A8  E2 80 8D  F0 9F 91 A9  E2 80 8D  F0 9F 91 A7 */
    const char *emoji = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
    hu_outbound_verdict_t v = run_strip(emoji);
    /* Legitimate emoji ZWJ sequence must NOT trigger a rewrite. */
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
    hu_outbound_verdict_clear(&v, test_alloc());
}

/* Multiple problematic codepoints in one message → all stripped at once. */
static void test_strip_multiple_codepoints_all_stripped(void) {
    /* U+FFFC + U+200B + U+202E mashed together with ASCII. */
    hu_outbound_verdict_t v = run_strip("a\xEF\xBF\xBC"
                                        "b\xE2\x80\x8B"
                                        "c\xE2\x80\xAE"
                                        "d");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REWRITE);
    HU_ASSERT_STR_EQ(v.replacement, "abcd");
    hu_outbound_verdict_clear(&v, test_alloc());
}

/* Corpus PASS cases must NOT trigger any rewrite. */
static void test_strip_corpus_pass_cases_send(void) {
    const char *pass_cases[] = {"how'd it go with the loan?",              /* #19 */
                                "you still getting that loan tomorrow?",   /* #20 */
                                "morning! How's the garden doing?",        /* #21 */
                                "how are those funny looking dogs doing?", /* #22 */
                                "see any more funny looking dogs lately?", /* #23 */
                                "how are you",                             /* #24 */
                                NULL};
    for (int i = 0; pass_cases[i]; i++) {
        hu_outbound_verdict_t v = run_strip(pass_cases[i]);
        HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
        hu_outbound_verdict_clear(&v, test_alloc());
    }
}

/* Single U+FFFC alone → empty rewrite. */
static void test_strip_lone_u_fffc_rewrites_to_empty(void) {
    hu_outbound_verdict_t v = run_strip("\xEF\xBF\xBC");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REWRITE);
    HU_ASSERT_NOT_NULL(v.replacement);
    HU_ASSERT_EQ(v.replacement_len, (size_t)0);
    HU_ASSERT_STR_EQ(v.replacement, "");
    hu_outbound_verdict_clear(&v, test_alloc());
}

/* Adjacent problematic codepoints → multi-strip works. */
static void test_strip_adjacent_codepoints_rewrites(void) {
    hu_outbound_verdict_t v = run_strip("x\xEF\xBF\xBC\xEF\xBF\xBC"
                                        "y");
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REWRITE);
    HU_ASSERT_STR_EQ(v.replacement, "xy");
    hu_outbound_verdict_clear(&v, test_alloc());
}

void run_outbound_strip_tests(void) {
    HU_TEST_SUITE("outbound_strip");
    HU_RUN_TEST(test_strip_plain_ascii_passes_through);
    HU_RUN_TEST(test_strip_empty_string_returns_send);
    HU_RUN_TEST(test_strip_null_content_returns_send);
    HU_RUN_TEST(test_strip_u_fffc_rewrites_to_clean_content);
    HU_RUN_TEST(test_strip_u_202e_rewrites_to_clean_content);
    HU_RUN_TEST(test_strip_u_200b_rewrites_to_clean_content);
    HU_RUN_TEST(test_strip_isolated_u_200d_rewrites);
    HU_RUN_TEST(test_strip_emoji_zwj_sequence_preserved);
    HU_RUN_TEST(test_strip_multiple_codepoints_all_stripped);
    HU_RUN_TEST(test_strip_corpus_pass_cases_send);
    HU_RUN_TEST(test_strip_lone_u_fffc_rewrites_to_empty);
    HU_RUN_TEST(test_strip_adjacent_codepoints_rewrites);
}
