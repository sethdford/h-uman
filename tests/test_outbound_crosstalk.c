/* test_outbound_crosstalk.c — Sprint 59 crosstalk stage contract.
 *
 * Covers:
 *   - Pure Jaccard predicate (hu_outbound_crosstalk_jaccard_5gram)
 *   - Metadata-pattern check (corpus #4: "(last: 1774705881)")
 *   - Cross-contact bleed via injected lookup callback (corpus #1-3)
 *   - Pass cases (#19-24) must not trigger either check
 *   - Graceful degradation when no lookup callback is registered
 */

#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

#include "human/agent/outbound_pipeline.h"
#include "human/core/allocator.h"

extern hu_outbound_pipeline_stage_t hu_outbound_pipeline_stage_crosstalk;

static hu_allocator_t *test_alloc(void) {
    static hu_allocator_t a;
    static int init = 0;
    if (!init) {
        a = hu_system_allocator();
        init = 1;
    }
    return &a;
}

/* ----------------------------------------------------------------- */
/* Fake corpus lookup for cross-contact tests                        */
/* ----------------------------------------------------------------- */

typedef struct {
    const char *const *texts;
    size_t count;
} fake_corpus_t;

static int fake_lookup(void *userdata, hu_allocator_t *alloc, const char *exclude_id,
                       size_t exclude_id_len, char ***out_texts, size_t *out_count) {
    (void)exclude_id;
    (void)exclude_id_len;
    fake_corpus_t *fc = (fake_corpus_t *)userdata;
    *out_texts = NULL;
    *out_count = 0;
    if (fc->count == 0)
        return 0;
    char **arr = (char **)alloc->alloc(alloc->ctx, fc->count * sizeof(char *));
    if (!arr)
        return -1;
    for (size_t i = 0; i < fc->count; i++) {
        size_t n = strlen(fc->texts[i]);
        char *buf = (char *)alloc->alloc(alloc->ctx, n + 1);
        if (!buf) {
            for (size_t k = 0; k < i; k++)
                alloc->free(alloc->ctx, arr[k], strlen(arr[k]) + 1);
            alloc->free(alloc->ctx, arr, fc->count * sizeof(char *));
            return -1;
        }
        memcpy(buf, fc->texts[i], n + 1);
        arr[i] = buf;
    }
    *out_texts = arr;
    *out_count = fc->count;
    return 0;
}

static hu_outbound_verdict_t run_crosstalk(const char *content, fake_corpus_t *fc) {
    if (fc) {
        hu_outbound_crosstalk_set_lookup(fake_lookup, fc);
    } else {
        hu_outbound_crosstalk_set_lookup(NULL, NULL);
    }
    hu_outbound_message_t msg = {0};
    msg.content = (char *)content;
    msg.content_len = strlen(content);

    hu_outbound_context_t ctx = {0};
    ctx.alloc = test_alloc();
    ctx.path = HU_OUTBOUND_PATH_PROACTIVE;
    ctx.regenerate_budget = 1;
    ctx.recipient_contact_id = "+18018285260";
    ctx.recipient_contact_id_len = strlen("+18018285260");

    hu_outbound_verdict_t v =
        hu_outbound_pipeline_stage_crosstalk.run(&hu_outbound_pipeline_stage_crosstalk, &msg, &ctx);

    /* Clean up — leave lookup unregistered for next test. */
    hu_outbound_crosstalk_set_lookup(NULL, NULL);
    return v;
}

/* ----------------------------------------------------------------- */
/* Pure-predicate tests                                              */
/* ----------------------------------------------------------------- */

static void test_jaccard_identical_strings_returns_one(void) {
    const char *s = "but boy I am just more lonely now than ever";
    double score = hu_outbound_crosstalk_jaccard_5gram(test_alloc(), s, strlen(s), s, strlen(s));
    HU_ASSERT_TRUE(score > 0.99);
}

static void test_jaccard_disjoint_strings_returns_near_zero(void) {
    double score = hu_outbound_crosstalk_jaccard_5gram(
        test_alloc(), "how's the garden", strlen("how's the garden"), "what time is the meeting",
        strlen("what time is the meeting"));
    HU_ASSERT_TRUE(score < 0.2);
}

static void test_jaccard_substring_match_returns_high(void) {
    const char *short_msg = "but boy I am just more lonely now";
    const char *long_corpus = "I told her but boy I am just more lonely now than ever before";
    double score = hu_outbound_crosstalk_jaccard_5gram(test_alloc(), short_msg, strlen(short_msg),
                                                       long_corpus, strlen(long_corpus));
    HU_ASSERT_TRUE(score >= 0.4);
}

static void test_jaccard_zero_length_returns_zero(void) {
    HU_ASSERT_EQ(hu_outbound_crosstalk_jaccard_5gram(test_alloc(), "", 0, "abc", 3), 0.0);
    HU_ASSERT_EQ(hu_outbound_crosstalk_jaccard_5gram(test_alloc(), "abc", 3, "", 0), 0.0);
}

/* ----------------------------------------------------------------- */
/* Metadata-pattern tests                                            */
/* ----------------------------------------------------------------- */

/* Corpus #4: "(last: 1774705881)" — timestamp metadata leak. */
static void test_metadata_pattern_unix_timestamp_rejects(void) {
    hu_outbound_verdict_t v = run_crosstalk("(last: 1774705881)", NULL);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REJECT);
    HU_ASSERT_STR_EQ(v.reason, "crosstalk_metadata_pattern");
}

static void test_metadata_pattern_embedded_in_text_rejects(void) {
    hu_outbound_verdict_t v =
        run_crosstalk("hey just checking in (last: 1774705881) how are you", NULL);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REJECT);
}

static void test_metadata_pattern_short_number_passes(void) {
    /* A 3-digit number is just a phone area code or count — not a
     * timestamp leak. */
    hu_outbound_verdict_t v = run_crosstalk("call me back (ext: 123) please", NULL);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

static void test_metadata_pattern_no_keyword_passes(void) {
    /* "(1234567890)" — no keyword: prefix. Not a metadata leak. */
    hu_outbound_verdict_t v = run_crosstalk("call me at (1234567890) tomorrow", NULL);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

/* ----------------------------------------------------------------- */
/* Cross-contact bleed tests                                         */
/* ----------------------------------------------------------------- */

/* Corpus #1, #2, #3: the verbatim "but boy I am just more lonely" string. */
static void test_crosstalk_corpus_verbatim_bleed_rejects(void) {
    const char *other_contact_text = "but boy I am just more lonely now than ever. I am skeptical";
    const char *texts[] = {other_contact_text};
    fake_corpus_t fc = {texts, 1};
    hu_outbound_verdict_t v =
        run_crosstalk("but boy I am just more lonely now than ever. I am skeptical ?", &fc);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REJECT);
    HU_ASSERT_STR_EQ(v.reason, "crosstalk_other_contact");
}

/* If no other contact has matching content, no rejection. */
static void test_crosstalk_legitimate_with_corpus_sends(void) {
    const char *texts[] = {"how was the loan meeting today", "the garden is doing great",
                           "saw your dad yesterday"};
    fake_corpus_t fc = {texts, 3};
    hu_outbound_verdict_t v = run_crosstalk("how are those funny looking dogs doing?", &fc);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

/* No lookup callback → degraded, but metadata check still runs. */
static void test_crosstalk_no_callback_runs_metadata_check(void) {
    hu_outbound_crosstalk_set_lookup(NULL, NULL);
    hu_outbound_verdict_t v = run_crosstalk("hey (last: 1774705881)", NULL);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_REJECT);
    HU_ASSERT_STR_EQ(v.reason, "crosstalk_metadata_pattern");
}

/* No lookup callback + clean message → SEND (degraded path). */
static void test_crosstalk_no_callback_clean_message_sends(void) {
    hu_outbound_verdict_t v = run_crosstalk("how are you", NULL);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

/* Empty corpus → SEND. */
static void test_crosstalk_empty_corpus_sends(void) {
    fake_corpus_t fc = {NULL, 0};
    hu_outbound_verdict_t v = run_crosstalk("how are you doing today", &fc);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

/* PASS-case corpus regression. */
static void test_crosstalk_corpus_pass_cases_send(void) {
    const char *other_contact_text = "but boy I am just more lonely now than ever. I am skeptical";
    const char *texts[] = {other_contact_text};
    fake_corpus_t fc = {texts, 1};
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
        hu_outbound_verdict_t v = run_crosstalk(pass_cases[i], &fc);
        HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
    }
}

/* Short message — skip Jaccard (cheap optimization). */
static void test_crosstalk_short_message_sends_without_jaccard(void) {
    const char *texts[] = {"hi"};
    fake_corpus_t fc = {texts, 1};
    hu_outbound_verdict_t v = run_crosstalk("hi", &fc);
    HU_ASSERT_EQ(v.kind, HU_OUTBOUND_SEND);
}

void run_outbound_crosstalk_tests(void) {
    HU_TEST_SUITE("outbound_crosstalk");
    HU_RUN_TEST(test_jaccard_identical_strings_returns_one);
    HU_RUN_TEST(test_jaccard_disjoint_strings_returns_near_zero);
    HU_RUN_TEST(test_jaccard_substring_match_returns_high);
    HU_RUN_TEST(test_jaccard_zero_length_returns_zero);
    HU_RUN_TEST(test_metadata_pattern_unix_timestamp_rejects);
    HU_RUN_TEST(test_metadata_pattern_embedded_in_text_rejects);
    HU_RUN_TEST(test_metadata_pattern_short_number_passes);
    HU_RUN_TEST(test_metadata_pattern_no_keyword_passes);
    HU_RUN_TEST(test_crosstalk_corpus_verbatim_bleed_rejects);
    HU_RUN_TEST(test_crosstalk_legitimate_with_corpus_sends);
    HU_RUN_TEST(test_crosstalk_no_callback_runs_metadata_check);
    HU_RUN_TEST(test_crosstalk_no_callback_clean_message_sends);
    HU_RUN_TEST(test_crosstalk_empty_corpus_sends);
    HU_RUN_TEST(test_crosstalk_corpus_pass_cases_send);
    HU_RUN_TEST(test_crosstalk_short_message_sends_without_jaccard);
}
