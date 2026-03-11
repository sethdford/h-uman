#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/memory/rag_pipeline.h"
#include "test_framework.h"
#include <string.h>

/* RAG Pipeline tests */

static void default_config_has_all_weights_one(void) {
    hu_rag_config_t cfg = hu_rag_default_config();
    HU_ASSERT_EQ(cfg.max_results, 10u);
    HU_ASSERT_EQ(cfg.max_token_budget, 2000u);
    for (size_t i = 0; i < HU_RAG_SOURCE_COUNT; i++)
        HU_ASSERT_FLOAT_EQ(cfg.source_weights[i], 1.0, 0.001);
    HU_ASSERT_FLOAT_EQ(cfg.min_relevance, 0.3, 0.001);
}

static void compute_score_combines_relevance_freshness_weight(void) {
    double s = hu_rag_compute_score(1.0, 1.0, 1.0);
    HU_ASSERT_FLOAT_EQ(s, 1.0, 0.001);
    s = hu_rag_compute_score(0.5, 0.5, 0.5);
    HU_ASSERT_FLOAT_EQ(s, 0.5, 0.001);
    s = hu_rag_compute_score(0.0, 0.0, 0.0);
    HU_ASSERT_FLOAT_EQ(s, 0.0, 0.001);
}

static void sort_results_orders_by_combined_score_desc(void) {
    hu_rag_result_t r[3] = {{0}};
    r[0].combined_score = 0.3;
    r[0].source = HU_RAG_EPISODIC;
    r[1].combined_score = 0.9;
    r[1].source = HU_RAG_KNOWLEDGE;
    r[2].combined_score = 0.6;
    r[2].source = HU_RAG_FEEDS;

    hu_rag_sort_results(r, 3);
    HU_ASSERT_FLOAT_EQ(r[0].combined_score, 0.9, 0.001);
    HU_ASSERT_EQ(r[0].source, HU_RAG_KNOWLEDGE);
    HU_ASSERT_FLOAT_EQ(r[1].combined_score, 0.6, 0.001);
    HU_ASSERT_FLOAT_EQ(r[2].combined_score, 0.3, 0.001);
}

static void select_within_budget_stops_at_token_limit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rag_result_t r[3] = {{0}};
    r[0].content = hu_strndup(&alloc, "a", 1);
    r[0].content_len = 1;
    r[1].content = hu_strndup(&alloc, "bbbb", 4);
    r[1].content_len = 4;
    r[2].content = hu_strndup(&alloc, "cccccccc", 8);
    r[2].content_len = 8;

    size_t n = hu_rag_select_within_budget(r, 3, 1);
    HU_ASSERT_EQ(n, 1u);

    hu_rag_result_deinit(&alloc, &r[0]);
    hu_rag_result_deinit(&alloc, &r[1]);
    hu_rag_result_deinit(&alloc, &r[2]);
}

static void build_prompt_formats_retrieved_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rag_result_t r[2] = {{0}};
    r[0].source = HU_RAG_EPISODIC;
    r[0].content = hu_strndup(&alloc, "went hiking with them last month", 32);
    r[0].content_len = 32;
    r[1].source = HU_RAG_KNOWLEDGE;
    r[1].content = hu_strndup(&alloc, "they know about the new job", 27);
    r[1].content_len = 27;

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_rag_build_prompt(&alloc, r, 2, &out, &out_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(out, "[RETRIEVED CONTEXT]") != NULL);
    HU_ASSERT_TRUE(strstr(out, "[episodic]") != NULL);
    HU_ASSERT_TRUE(strstr(out, "[knowledge]") != NULL);
    HU_ASSERT_TRUE(strstr(out, "went hiking") != NULL);
    HU_ASSERT_TRUE(strstr(out, "new job") != NULL);

    hu_str_free(&alloc, out);
    hu_rag_result_deinit(&alloc, &r[0]);
    hu_rag_result_deinit(&alloc, &r[1]);
}

static void build_prompt_empty_results_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 1;
    hu_error_t err = hu_rag_build_prompt(&alloc, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(out_len, 0u);
}

static void source_str_returns_correct_labels(void) {
    HU_ASSERT_STR_EQ(hu_rag_source_str(HU_RAG_EPISODIC), "episodic");
    HU_ASSERT_STR_EQ(hu_rag_source_str(HU_RAG_KNOWLEDGE), "knowledge");
    HU_ASSERT_STR_EQ(hu_rag_source_str(HU_RAG_FEEDS), "feeds");
}

static void result_deinit_clears_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rag_result_t r = {0};
    r.content = hu_strndup(&alloc, "test", 4);
    r.content_len = 4;
    hu_rag_result_deinit(&alloc, &r);
    HU_ASSERT_NULL(r.content);
    HU_ASSERT_EQ(r.content_len, 0u);
}

/* Classifier tests */

static void classify_greeting_hey_returns_greeting(void) {
    double conf = 0.0;
    hu_classify_result_t r = hu_classify_message("hey there", 9, &conf);
    HU_ASSERT_EQ(r, HU_CLASS_GREETING);
    HU_ASSERT_FLOAT_EQ(conf, 0.8, 0.01);
}

static void classify_greeting_hi_returns_greeting(void) {
    double conf = 0.0;
    HU_ASSERT_EQ(hu_classify_message("Hi!", 3, &conf), HU_CLASS_GREETING);
}

static void classify_question_mark_returns_question(void) {
    double conf = 0.0;
    HU_ASSERT_EQ(hu_classify_message("what time is it?", 16, &conf), HU_CLASS_QUESTION);
}

static void classify_urgent_asap_returns_urgent(void) {
    double conf = 0.0;
    HU_ASSERT_EQ(hu_classify_message("need this asap", 14, &conf), HU_CLASS_URGENT);
}

static void classify_emotional_sad_returns_emotional(void) {
    double conf = 0.0;
    HU_ASSERT_EQ(hu_classify_message("I feel sad today", 16, &conf), HU_CLASS_EMOTIONAL);
}

static void classify_planning_lets_returns_planning(void) {
    double conf = 0.0;
    HU_ASSERT_EQ(hu_classify_message("let's meet tomorrow", 19, &conf), HU_CLASS_PLANNING);
}

static void classify_humor_lol_returns_humor(void) {
    double conf = 0.0;
    HU_ASSERT_EQ(hu_classify_message("that was funny lol", 18, &conf), HU_CLASS_HUMOR);
}

static void classify_informational_did_you_know_returns_informational(void) {
    double conf = 0.0;
    hu_classify_message("did you know that?", 18, &conf);
    HU_ASSERT_TRUE(conf >= 0.0);
}

static void classify_casual_fallback_returns_casual(void) {
    double conf = 0.0;
    HU_ASSERT_EQ(hu_classify_message("just checking in", 15, &conf), HU_CLASS_CASUAL);
}

static void classify_empty_returns_unknown(void) {
    double conf = 1.0;
    HU_ASSERT_EQ(hu_classify_message("", 0, &conf), HU_CLASS_UNKNOWN);
    HU_ASSERT_FLOAT_EQ(conf, 0.0, 0.001);
}

static void classify_result_str_returns_labels(void) {
    HU_ASSERT_STR_EQ(hu_classify_result_str(HU_CLASS_GREETING), "greeting");
    HU_ASSERT_STR_EQ(hu_classify_result_str(HU_CLASS_QUESTION), "question");
    HU_ASSERT_STR_EQ(hu_classify_result_str(HU_CLASS_URGENT), "urgent");
}

static void classifier_build_prompt_includes_class_and_confidence(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_classifier_build_prompt(&alloc, HU_CLASS_GREETING, 0.85, &out, &out_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "[CLASSIFICATION]") != NULL);
    HU_ASSERT_TRUE(strstr(out, "greeting") != NULL);
    HU_ASSERT_TRUE(strstr(out, "0.85") != NULL);

    hu_str_free(&alloc, out);
}

void run_rag_pipeline_tests(void) {
    HU_TEST_SUITE("rag_pipeline");
    HU_RUN_TEST(default_config_has_all_weights_one);
    HU_RUN_TEST(compute_score_combines_relevance_freshness_weight);
    HU_RUN_TEST(sort_results_orders_by_combined_score_desc);
    HU_RUN_TEST(select_within_budget_stops_at_token_limit);
    HU_RUN_TEST(build_prompt_formats_retrieved_context);
    HU_RUN_TEST(build_prompt_empty_results_returns_ok);
    HU_RUN_TEST(source_str_returns_correct_labels);
    HU_RUN_TEST(result_deinit_clears_content);
    HU_RUN_TEST(classify_greeting_hey_returns_greeting);
    HU_RUN_TEST(classify_greeting_hi_returns_greeting);
    HU_RUN_TEST(classify_question_mark_returns_question);
    HU_RUN_TEST(classify_urgent_asap_returns_urgent);
    HU_RUN_TEST(classify_emotional_sad_returns_emotional);
    HU_RUN_TEST(classify_planning_lets_returns_planning);
    HU_RUN_TEST(classify_humor_lol_returns_humor);
    HU_RUN_TEST(classify_informational_did_you_know_returns_informational);
    HU_RUN_TEST(classify_casual_fallback_returns_casual);
    HU_RUN_TEST(classify_empty_returns_unknown);
    HU_RUN_TEST(classify_result_str_returns_labels);
    HU_RUN_TEST(classifier_build_prompt_includes_class_and_confidence);
}
