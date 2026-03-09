#include "seaclaw/agent/ab_response.h"
#include "seaclaw/context/conversation.h"
#include "seaclaw/core/allocator.h"
#include "test_framework.h"
#include <string.h>

/* ── sc_ab_evaluate tests ───────────────────────────────────────────── */

static void ab_evaluate_picks_best(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ab_result_t result;
    memset(&result, 0, sizeof(result));

    /* Three candidates with different quality; highest scorer wins */
    const char *a = "Certainly! I'd be happy to help you with that.";
    const char *b = "yeah that's wild lol";
    const char *c = "that sounds good; I'll check it out";

    result.candidates[0].response = (char *)alloc.alloc(alloc.ctx, strlen(a) + 1);
    SC_ASSERT_NOT_NULL(result.candidates[0].response);
    memcpy(result.candidates[0].response, a, strlen(a) + 1);
    result.candidates[0].response_len = strlen(a);

    result.candidates[1].response = (char *)alloc.alloc(alloc.ctx, strlen(b) + 1);
    SC_ASSERT_NOT_NULL(result.candidates[1].response);
    memcpy(result.candidates[1].response, b, strlen(b) + 1);
    result.candidates[1].response_len = strlen(b);

    result.candidates[2].response = (char *)alloc.alloc(alloc.ctx, strlen(c) + 1);
    SC_ASSERT_NOT_NULL(result.candidates[2].response);
    memcpy(result.candidates[2].response, c, strlen(c) + 1);
    result.candidates[2].response_len = strlen(c);

    result.candidate_count = 3;

    sc_error_t err = sc_ab_evaluate(&alloc, &result, NULL, 0, 300);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(result.best_idx < 3u);
    int best = result.candidates[result.best_idx].quality_score;
    for (size_t i = 0; i < 3; i++)
        SC_ASSERT_TRUE(result.candidates[i].quality_score <= best);
    /* Natural-sounding response (a) should score highest and be selected */
    size_t best_idx = 0;
    for (size_t i = 1; i < 3; i++)
        if (result.candidates[i].quality_score > result.candidates[best_idx].quality_score)
            best_idx = i;
    SC_ASSERT_EQ(result.best_idx, best_idx);

    sc_ab_result_deinit(&result, &alloc);
}

static void ab_evaluate_single_candidate(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ab_result_t result;
    memset(&result, 0, sizeof(result));

    const char *txt = "sounds good";
    result.candidates[0].response = (char *)alloc.alloc(alloc.ctx, strlen(txt) + 1);
    SC_ASSERT_NOT_NULL(result.candidates[0].response);
    memcpy(result.candidates[0].response, txt, strlen(txt) + 1);
    result.candidates[0].response_len = strlen(txt);
    result.candidate_count = 1;

    sc_error_t err = sc_ab_evaluate(&alloc, &result, NULL, 0, 300);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(result.best_idx, 0u);

    sc_ab_result_deinit(&result, &alloc);
}

static void ab_evaluate_null_args(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ab_result_t result;
    memset(&result, 0, sizeof(result));
    result.candidate_count = 1;

    sc_error_t err = sc_ab_evaluate(&alloc, NULL, NULL, 0, 300);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void ab_result_deinit_frees(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ab_result_t result;
    memset(&result, 0, sizeof(result));

    const char *txt = "test response";
    result.candidates[0].response = (char *)alloc.alloc(alloc.ctx, strlen(txt) + 1);
    SC_ASSERT_NOT_NULL(result.candidates[0].response);
    memcpy(result.candidates[0].response, txt, strlen(txt) + 1);
    result.candidates[0].response_len = strlen(txt);
    result.candidate_count = 1;

    sc_ab_result_deinit(&result, &alloc);
    SC_ASSERT_NULL(result.candidates[0].response);
    SC_ASSERT_EQ(result.candidate_count, 0u);
}

static void ab_evaluate_zero_candidates(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ab_result_t result;
    memset(&result, 0, sizeof(result));
    result.candidate_count = 0;

    sc_error_t err = sc_ab_evaluate(&alloc, &result, NULL, 0, 300);
    SC_ASSERT_EQ(err, SC_OK);
}

static void ab_evaluate_max_chars_zero(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ab_result_t result;
    memset(&result, 0, sizeof(result));

    const char *txt = "short reply";
    result.candidates[0].response = (char *)alloc.alloc(alloc.ctx, strlen(txt) + 1);
    memcpy(result.candidates[0].response, txt, strlen(txt) + 1);
    result.candidates[0].response_len = strlen(txt);
    result.candidate_count = 1;

    sc_error_t err = sc_ab_evaluate(&alloc, &result, NULL, 0, 0);
    SC_ASSERT_EQ(err, SC_OK);
    sc_ab_result_deinit(&result, &alloc);
}

static void ab_result_deinit_zeroed_safe(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ab_result_t result;
    memset(&result, 0, sizeof(result));
    sc_ab_result_deinit(&result, &alloc);
    SC_ASSERT_EQ(result.candidate_count, 0u);
}

static void ab_evaluate_null_alloc(void) {
    sc_ab_result_t result;
    memset(&result, 0, sizeof(result));
    sc_error_t err = sc_ab_evaluate(NULL, &result, NULL, 0, 300);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

void run_ab_response_tests(void) {
    SC_TEST_SUITE("ab_response");
    SC_RUN_TEST(ab_evaluate_picks_best);
    SC_RUN_TEST(ab_evaluate_single_candidate);
    SC_RUN_TEST(ab_evaluate_null_args);
    SC_RUN_TEST(ab_result_deinit_frees);
    SC_RUN_TEST(ab_evaluate_zero_candidates);
    SC_RUN_TEST(ab_evaluate_max_chars_zero);
    SC_RUN_TEST(ab_result_deinit_zeroed_safe);
    SC_RUN_TEST(ab_evaluate_null_alloc);
}
