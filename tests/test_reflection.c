/* Reflection module tests — quality evaluation + critique prompt */
#include "human/agent/reflection.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <string.h>

static void test_reflection_null_response(void) {
    HU_ASSERT_EQ(hu_reflection_evaluate("q", 1, NULL, 0, NULL), HU_QUALITY_NEEDS_RETRY);
}

static void test_reflection_empty_response(void) {
    HU_ASSERT_EQ(hu_reflection_evaluate("q", 1, "", 0, NULL), HU_QUALITY_NEEDS_RETRY);
}

static void test_reflection_trivial_short_response(void) {
    HU_ASSERT_EQ(hu_reflection_evaluate("q", 1, "ok", 2, NULL), HU_QUALITY_NEEDS_RETRY);
    HU_ASSERT_EQ(hu_reflection_evaluate("q", 1, "12345678", 8, NULL), HU_QUALITY_NEEDS_RETRY);
}

static void test_reflection_refusal_i_cannot(void) {
    const char *resp = "I cannot help with that request due to policy restrictions.";
    HU_ASSERT_EQ(hu_reflection_evaluate("help me", 7, resp, strlen(resp), NULL),
                 HU_QUALITY_ACCEPTABLE);
}

static void test_reflection_refusal_i_cant(void) {
    const char *resp = "I can't assist with this kind of task.";
    HU_ASSERT_EQ(hu_reflection_evaluate("help me", 7, resp, strlen(resp), NULL),
                 HU_QUALITY_ACCEPTABLE);
}

static void test_reflection_refusal_unable(void) {
    const char *resp = "I'm unable to perform that operation right now.";
    HU_ASSERT_EQ(hu_reflection_evaluate("help me", 7, resp, strlen(resp), NULL),
                 HU_QUALITY_ACCEPTABLE);
}

static void test_reflection_refusal_as_an_ai(void) {
    const char *resp = "As an AI language model, I don't have access to your files.";
    HU_ASSERT_EQ(hu_reflection_evaluate("help me", 7, resp, strlen(resp), NULL),
                 HU_QUALITY_ACCEPTABLE);
}

static void test_reflection_question_short_answer(void) {
    const char *resp = "Yes, that's correct.";
    HU_ASSERT_EQ(hu_reflection_evaluate("Is C faster than Python?", 24, resp, strlen(resp), NULL),
                 HU_QUALITY_ACCEPTABLE);
}

static void test_reflection_question_good_answer(void) {
    const char *resp =
        "C is generally faster than Python because it compiles to native machine code "
        "while Python is interpreted. However, Python with C extensions or NumPy can be "
        "competitive.";
    HU_ASSERT_EQ(hu_reflection_evaluate("Is C faster than Python?", 24, resp, strlen(resp), NULL),
                 HU_QUALITY_GOOD);
}

static void test_reflection_no_question_good(void) {
    const char *resp = "Here is the implementation for the sorting algorithm you requested.";
    HU_ASSERT_EQ(hu_reflection_evaluate("sort this array", 15, resp, strlen(resp), NULL),
                 HU_QUALITY_GOOD);
}

static void test_reflection_min_tokens_below(void) {
    hu_reflection_config_t cfg = {.enabled = true, .min_response_tokens = 20, .max_retries = 1};
    const char *resp = "Short reply here.";
    HU_ASSERT_EQ(hu_reflection_evaluate("tell me more", 12, resp, strlen(resp), &cfg),
                 HU_QUALITY_ACCEPTABLE);
}

static void test_reflection_min_tokens_above(void) {
    hu_reflection_config_t cfg = {.enabled = true, .min_response_tokens = 5, .max_retries = 1};
    const char *resp =
        "This is a longer response that contains more than five words and should pass.";
    HU_ASSERT_EQ(hu_reflection_evaluate("tell me more", 12, resp, strlen(resp), &cfg),
                 HU_QUALITY_GOOD);
}

static void test_reflection_case_insensitive_refusal(void) {
    const char *resp = "I CANNOT fulfill that request under current policy.";
    HU_ASSERT_EQ(hu_reflection_evaluate("help", 4, resp, strlen(resp), NULL),
                 HU_QUALITY_ACCEPTABLE);
}

static void test_reflection_critique_prompt_null(void) {
    HU_ASSERT_EQ(hu_reflection_build_critique_prompt(NULL, "q", 1, "r", 1, NULL, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_reflection_critique_prompt_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *prompt = NULL;
    size_t prompt_len = 0;

    hu_error_t err = hu_reflection_build_critique_prompt(
        &alloc, "What is C?", 10, "C is a language.", 16, &prompt, &prompt_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(prompt_len > 0);
    HU_ASSERT_TRUE(strstr(prompt, "What is C?") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "C is a language.") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "Evaluate") != NULL);
    alloc.free(alloc.ctx, prompt, prompt_len + 1);
}

static void test_reflection_critique_prompt_empty_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *prompt = NULL;
    size_t prompt_len = 0;

    hu_error_t err =
        hu_reflection_build_critique_prompt(&alloc, NULL, 0, "response", 8, &prompt, &prompt_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(strstr(prompt, "response") != NULL);
    alloc.free(alloc.ctx, prompt, prompt_len + 1);
}

static void test_reflection_result_free_null_safe(void) {
    hu_reflection_result_free(NULL, NULL);
    hu_allocator_t alloc = hu_system_allocator();
    hu_reflection_result_free(&alloc, NULL);
    hu_reflection_result_t r = {.quality = HU_QUALITY_GOOD, .feedback = NULL, .feedback_len = 0};
    hu_reflection_result_free(&alloc, &r);
}

static void test_reflection_result_free_with_feedback(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *fb = (char *)alloc.alloc(alloc.ctx, 16);
    memcpy(fb, "needs more info", 15);
    fb[15] = '\0';

    hu_reflection_result_t r = {
        .quality = HU_QUALITY_ACCEPTABLE, .feedback = fb, .feedback_len = 15};
    hu_reflection_result_free(&alloc, &r);
    HU_ASSERT_NULL(r.feedback);
    HU_ASSERT_EQ(r.feedback_len, 0);
}

void run_reflection_tests(void) {
    HU_TEST_SUITE("Reflection");
    HU_RUN_TEST(test_reflection_null_response);
    HU_RUN_TEST(test_reflection_empty_response);
    HU_RUN_TEST(test_reflection_trivial_short_response);
    HU_RUN_TEST(test_reflection_refusal_i_cannot);
    HU_RUN_TEST(test_reflection_refusal_i_cant);
    HU_RUN_TEST(test_reflection_refusal_unable);
    HU_RUN_TEST(test_reflection_refusal_as_an_ai);
    HU_RUN_TEST(test_reflection_question_short_answer);
    HU_RUN_TEST(test_reflection_question_good_answer);
    HU_RUN_TEST(test_reflection_no_question_good);
    HU_RUN_TEST(test_reflection_min_tokens_below);
    HU_RUN_TEST(test_reflection_min_tokens_above);
    HU_RUN_TEST(test_reflection_case_insensitive_refusal);
    HU_RUN_TEST(test_reflection_critique_prompt_null);
    HU_RUN_TEST(test_reflection_critique_prompt_basic);
    HU_RUN_TEST(test_reflection_critique_prompt_empty_query);
    HU_RUN_TEST(test_reflection_result_free_null_safe);
    HU_RUN_TEST(test_reflection_result_free_with_feedback);
}
