#include "human/eval/longmemeval.h"
#include "test_framework.h"

#include <stdio.h>
#include <string.h>

#ifndef HU_EVAL_SUITES_DIR
#error "HU_EVAL_SUITES_DIR must be defined when building human_tests"
#endif

static void lme_keyword_recall_full_match(void) {
    static const char *KW[] = {"trek", "domane"};
    hu_longmemeval_score_t s = {0};
    HU_ASSERT_EQ(hu_longmemeval_score_item("single_hop",
                                            "They ride a Trek Domane SL7.", 28, KW, 2, &s),
                 HU_OK);
    HU_ASSERT_EQ(s.score, 100);
    HU_ASSERT_EQ((long long)s.keywords_seen, (long long)s.keywords_total);
    HU_ASSERT_FALSE(s.abstained);
}

static void lme_keyword_recall_partial(void) {
    static const char *KW[] = {"trek", "domane", "carbon"};
    hu_longmemeval_score_t s = {0};
    HU_ASSERT_EQ(hu_longmemeval_score_item("single_hop",
                                            "They ride a Trek Domane.", 24, KW, 3, &s),
                 HU_OK);
    /* 2/3 ≈ 66 */
    HU_ASSERT_TRUE(s.score >= 60 && s.score <= 70);
}

static void lme_abstention_correct_response(void) {
    hu_longmemeval_score_t s = {0};
    HU_ASSERT_EQ(hu_longmemeval_score_item("abstention", "I don't know.", 13, NULL, 0, &s),
                 HU_OK);
    HU_ASSERT_TRUE(s.abstained);
    HU_ASSERT_EQ(s.score, 100);
}

static void lme_abstention_wrong_response_zero(void) {
    hu_longmemeval_score_t s = {0};
    HU_ASSERT_EQ(hu_longmemeval_score_item("abstention",
                                            "It's 12345-67890 probably.", 26, NULL, 0, &s),
                 HU_OK);
    HU_ASSERT_FALSE(s.abstained);
    HU_ASSERT_EQ(s.score, 0);
}

static void lme_no_keywords_grounded_response_default(void) {
    hu_longmemeval_score_t s = {0};
    HU_ASSERT_EQ(hu_longmemeval_score_item("temporal", "It was a Tuesday.", 17, NULL, 0, &s),
                 HU_OK);
    HU_ASSERT_EQ(s.score, 50);
}

static void lme_pack_self_test_meets_threshold(void) {
    char path[768];
    int n = snprintf(path, sizeof(path), "%s/longmemeval/longmemeval.json", HU_EVAL_SUITES_DIR);
    HU_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(path));
    hu_allocator_t alloc = hu_system_allocator();
    unsigned total = 0, passed = 0;
    int mean = 0;
    HU_ASSERT_EQ(hu_longmemeval_run_pack_self_test(&alloc, path, &total, &passed, &mean), HU_OK);
    HU_ASSERT_TRUE(total >= 4u);
    /* Self-test on the golden candidate answers must be near-perfect. */
    HU_ASSERT_TRUE(mean >= 80);
    HU_ASSERT_TRUE(passed * 4u >= total * 3u); /* ≥75 % items pass */
}

static void lme_null_args_return_invalid(void) {
    HU_ASSERT_EQ(hu_longmemeval_score_item("temporal", NULL, 0, NULL, 0, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    hu_longmemeval_score_t s = {0};
    HU_ASSERT_EQ(hu_longmemeval_score_item(NULL, "", 0, NULL, 0, &s), HU_OK);
}

void run_longmemeval_tests(void);

void run_longmemeval_tests(void) {
    HU_TEST_SUITE("longmemeval");
    HU_RUN_TEST(lme_keyword_recall_full_match);
    HU_RUN_TEST(lme_keyword_recall_partial);
    HU_RUN_TEST(lme_abstention_correct_response);
    HU_RUN_TEST(lme_abstention_wrong_response_zero);
    HU_RUN_TEST(lme_no_keywords_grounded_response_default);
    HU_RUN_TEST(lme_pack_self_test_meets_threshold);
    HU_RUN_TEST(lme_null_args_return_invalid);
}
