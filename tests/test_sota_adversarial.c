#include "human/agent/process_reward.h"
#include "human/memory/adaptive_rag.h"
#include "human/memory/self_rag.h"
#include "human/memory/tiers.h"
#include "human/ml/dpo.h"
#include "test_framework.h"
#include <string.h>

/* ── Adaptive RAG adversarial ────────────────────────────────── */

static void rag_all_whitespace_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_adaptive_rag_t rag;
    hu_adaptive_rag_create(&alloc, NULL, &rag);
    hu_rag_strategy_t s = hu_adaptive_rag_select(&rag, "   \t  \t  ", 9);
    HU_ASSERT(s >= 0 && s < HU_RAG_STRATEGY_COUNT);
    hu_adaptive_rag_deinit(&rag);
}

static void rag_single_char_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_adaptive_rag_t rag;
    hu_adaptive_rag_create(&alloc, NULL, &rag);
    hu_rag_strategy_t s = hu_adaptive_rag_select(&rag, "?", 1);
    HU_ASSERT(s >= 0 && s < HU_RAG_STRATEGY_COUNT);
    hu_adaptive_rag_deinit(&rag);
}

static void rag_binary_garbage_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_adaptive_rag_t rag;
    hu_adaptive_rag_create(&alloc, NULL, &rag);
    char garbage[64];
    for (int i = 0; i < 64; i++)
        garbage[i] = (char)(i * 7 + 13);
    hu_rag_strategy_t s = hu_adaptive_rag_select(&rag, garbage, 64);
    HU_ASSERT(s >= 0 && s < HU_RAG_STRATEGY_COUNT);
    hu_adaptive_rag_deinit(&rag);
}

static void rag_extreme_weight_values(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_adaptive_rag_t rag;
    hu_adaptive_rag_create(&alloc, NULL, &rag);
    for (int i = 0; i < HU_RAG_STRATEGY_COUNT; i++)
        hu_adaptive_rag_record_outcome(&rag, (hu_rag_strategy_t)i, 0.0);
    hu_rag_strategy_t s = hu_adaptive_rag_select(&rag, "what is gravity", 15);
    HU_ASSERT(s >= 0 && s < HU_RAG_STRATEGY_COUNT);
    hu_adaptive_rag_deinit(&rag);
}

static void rag_features_max_length_query(void) {
    char long_query[4096];
    memset(long_query, 'a', sizeof(long_query));
    for (size_t i = 5; i < sizeof(long_query); i += 6)
        long_query[i] = ' ';
    hu_rag_query_features_t f;
    hu_adaptive_rag_extract_features(long_query, sizeof(long_query), &f);
    HU_ASSERT(f.word_count > 0);
}

static void rag_record_outcome_boundary_strategy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_adaptive_rag_t rag;
    hu_adaptive_rag_create(&alloc, NULL, &rag);
    HU_ASSERT(hu_adaptive_rag_record_outcome(&rag, (hu_rag_strategy_t)-1, 0.5) ==
              HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT(hu_adaptive_rag_record_outcome(&rag, (hu_rag_strategy_t)HU_RAG_STRATEGY_COUNT, 0.5) ==
              HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT(hu_adaptive_rag_record_outcome(&rag, (hu_rag_strategy_t)99, 0.5) ==
              HU_ERR_INVALID_ARGUMENT);
    hu_adaptive_rag_deinit(&rag);
}

/* ── Self-RAG adversarial ────────────────────────────────────── */

static void srag_embedded_nulls_in_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_srag_config_t cfg = hu_srag_config_default();
    char query[] = "hello\0world\0test";
    hu_srag_assessment_t out;
    hu_error_t err = hu_srag_should_retrieve(&alloc, &cfg, query, 16, NULL, 0, &out);
    HU_ASSERT(err == HU_OK);
}

static void srag_max_length_relevance(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_srag_config_t cfg = hu_srag_config_default();
    char big_query[8192];
    memset(big_query, 'x', sizeof(big_query));
    for (size_t i = 3; i < sizeof(big_query); i += 4)
        big_query[i] = ' ';
    char big_content[8192];
    memset(big_content, 'y', sizeof(big_content));
    for (size_t i = 3; i < sizeof(big_content); i += 4)
        big_content[i] = ' ';
    double rel = 0.0;
    bool use = false;
    hu_error_t err = hu_srag_verify_relevance(&alloc, &cfg, big_query, sizeof(big_query),
                                              big_content, sizeof(big_content), &rel, &use);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(rel >= 0.0 && rel <= 1.0);
}

static void srag_identical_query_and_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_srag_config_t cfg = hu_srag_config_default();
    const char *text = "the quick brown fox jumps over the lazy dog";
    double rel = 0.0;
    bool use = false;
    hu_error_t err =
        hu_srag_verify_relevance(&alloc, &cfg, text, strlen(text), text, strlen(text), &rel, &use);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(rel > 0.5);
    HU_ASSERT(use == true);
}

static void srag_all_greeting_variants(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_srag_config_t cfg = hu_srag_config_default();
    const char *greetings[] = {"hi",        "hello", "hey",   "thanks", "bye",     "ok",
                               "thank you", "HI",    "HELLO", "Hey!",   "Thanks.", "OK"};
    for (size_t i = 0; i < sizeof(greetings) / sizeof(greetings[0]); i++) {
        hu_srag_assessment_t out;
        hu_srag_should_retrieve(&alloc, &cfg, greetings[i], strlen(greetings[i]), NULL, 0, &out);
        HU_ASSERT(out.decision == HU_SRAG_NO_RETRIEVAL);
    }
}

static void srag_unicode_like_bytes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_srag_config_t cfg = hu_srag_config_default();
    const char query[] = "\xc3\xa9\xc3\xa0\xc3\xbc test query \xf0\x9f\x98\x80";
    hu_srag_assessment_t out;
    hu_error_t err = hu_srag_should_retrieve(&alloc, &cfg, query, sizeof(query) - 1, NULL, 0, &out);
    HU_ASSERT(err == HU_OK);
}

/* ── Memory Tiers adversarial ────────────────────────────────── */

static void tiers_update_core_max_length_value(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tier_manager_t mgr;
    hu_tier_manager_create(&alloc, NULL, &mgr);
    char huge[2048];
    memset(huge, 'Z', sizeof(huge));
    hu_error_t err = hu_tier_manager_update_core(&mgr, "user_name", 9, huge, sizeof(huge));
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(strlen(mgr.core.user_name) == sizeof(mgr.core.user_name) - 1);
    hu_tier_manager_deinit(&mgr);
}

static void tiers_build_prompt_tiny_buffer(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tier_manager_t mgr;
    hu_tier_manager_create(&alloc, NULL, &mgr);
    hu_tier_manager_update_core(&mgr, "user_name", 9, "Test User", 9);
    char tiny[4];
    size_t len = 0;
    hu_error_t err = hu_tier_manager_build_core_prompt(&mgr, tiny, sizeof(tiny), &len);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(len <= sizeof(tiny) - 1);
    HU_ASSERT(tiny[len] == '\0' || len == sizeof(tiny) - 1);
    hu_tier_manager_deinit(&mgr);
}

static void tiers_build_prompt_exact_capacity(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tier_manager_t mgr;
    hu_tier_manager_create(&alloc, NULL, &mgr);
    hu_tier_manager_update_core(&mgr, "user_name", 9, "A", 1);
    char buf[16];
    size_t len = 0;
    hu_error_t err = hu_tier_manager_build_core_prompt(&mgr, buf, sizeof(buf), &len);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(len < sizeof(buf));
    hu_tier_manager_deinit(&mgr);
}

static void tiers_auto_tier_empty_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tier_manager_t mgr;
    hu_tier_manager_create(&alloc, NULL, &mgr);
    hu_memory_tier_t assigned = HU_TIER_ARCHIVAL;
    hu_error_t err = hu_tier_manager_auto_tier(&mgr, "k", 1, "", 0, &assigned);
    /* No DB → store fails with NOT_SUPPORTED, but tier assignment still happened */
    HU_ASSERT(err == HU_ERR_NOT_SUPPORTED);
    HU_ASSERT(assigned == HU_TIER_RECALL);
    hu_tier_manager_deinit(&mgr);
}

static void tiers_update_all_fields_then_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tier_manager_t mgr;
    hu_tier_manager_create(&alloc, NULL, &mgr);
    hu_tier_manager_update_core(&mgr, "user_name", 9, "Alice", 5);
    hu_tier_manager_update_core(&mgr, "user_bio", 8, "Engineer", 8);
    hu_tier_manager_update_core(&mgr, "user_preferences", 16, "Concise answers", 15);
    hu_tier_manager_update_core(&mgr, "relationship_summary", 20, "Close friend", 12);
    hu_tier_manager_update_core(&mgr, "active_goals", 12, "Ship v2", 7);
    char buf[2048];
    size_t len = 0;
    hu_tier_manager_build_core_prompt(&mgr, buf, sizeof(buf), &len);
    HU_ASSERT(len > 0);
    HU_ASSERT(strstr(buf, "Alice") != NULL);
    HU_ASSERT(strstr(buf, "Engineer") != NULL);
    HU_ASSERT(strstr(buf, "Ship v2") != NULL);
    hu_tier_manager_deinit(&mgr);
}

/* ── PRM adversarial ─────────────────────────────────────────── */

static void prm_single_byte_input(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prm_config_t cfg = hu_prm_config_default();
    hu_prm_result_t result;
    hu_error_t err = hu_prm_score_chain(&alloc, &cfg, "x", 1, &result);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(result.step_count == 1);
    hu_prm_result_free(&alloc, &result);
}

static void prm_only_newlines(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prm_config_t cfg = hu_prm_config_default();
    hu_prm_result_t result;
    hu_error_t err = hu_prm_score_chain(&alloc, &cfg, "\n\n\n\n\n\n\n\n\n\n", 10, &result);
    HU_ASSERT(err == HU_OK);
    hu_prm_result_free(&alloc, &result);
}

static void prm_65_steps_capped_at_64(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prm_config_t cfg = hu_prm_config_default();
    char big[4096];
    size_t pos = 0;
    for (int i = 0; i < 70 && pos + 20 < sizeof(big); i++) {
        int n = snprintf(big + pos, sizeof(big) - pos, "Step %d reasoning\n\n", i);
        if (n > 0)
            pos += (size_t)n;
    }
    hu_prm_result_t result;
    hu_error_t err = hu_prm_score_chain(&alloc, &cfg, big, pos, &result);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(result.step_count <= 64);
    hu_prm_result_free(&alloc, &result);
}

static void prm_all_hedge_words(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prm_config_t cfg = hu_prm_config_default();
    const char *text = "maybe perhaps i think not sure might be possible";
    hu_prm_result_t result;
    hu_error_t err = hu_prm_score_chain(&alloc, &cfg, text, strlen(text), &result);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(result.aggregate_score < 0.3);
    hu_prm_result_free(&alloc, &result);
}

static void prm_mixed_split_formats(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prm_config_t cfg = hu_prm_config_default();
    const char *text = "First step.\n\nSecond step.\n1. Third step\n2. Fourth step";
    hu_prm_result_t result;
    hu_error_t err = hu_prm_score_chain(&alloc, &cfg, text, strlen(text), &result);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(result.step_count >= 2);
    hu_prm_result_free(&alloc, &result);
}

static void prm_threshold_zero_all_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prm_config_t cfg = hu_prm_config_default();
    cfg.correctness_threshold = 0.0;
    const char *text = "maybe perhaps not sure";
    hu_prm_result_t result;
    hu_error_t err = hu_prm_score_chain(&alloc, &cfg, text, strlen(text), &result);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(result.chain_valid == true);
    hu_prm_result_free(&alloc, &result);
}

/* ── DPO adversarial ─────────────────────────────────────────── */

static void dpo_feedback_with_zero_length_response(void) {
    /* Asserts the corrected contract: empty or trivially-short responses
     * are REJECTED with HU_ERR_INVALID_ARGUMENT, not silently written as
     * single-sided rows.
     *
     * Prior to 2026-05-19 this test asserted err == HU_OK — codifying the
     * exact bug that caused 41.5% of dpo_pairs to be inverted (empty
     * chosen + "SKIP"/"GOOD" in rejected). Per
     * ~/.claude/rules/tests-that-pin-bugs.md the test name claims a
     * security/correctness behavior ("zero length response"); the
     * assertion must MATCH that intent. See
     * docs/plans/2026-05-19-dpo-corpus-inverted.md. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    hu_dpo_collector_create(&alloc, NULL, 100, &col);
    hu_error_t err = hu_dpo_record_from_feedback(&col, "prompt", 6, "", 0, true);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
    size_t count = 999;
    hu_dpo_pair_count(&col, &count);
    HU_ASSERT(count == 0);
    hu_dpo_collector_deinit(&col);
}

static void dpo_feedback_trivially_short_response_rejected(void) {
    /* Positive contract: explicitly verify the 4-byte minimum. A 3-char
     * response like "GO" or "k" is the exact length of the labels that
     * poisoned the corpus on 2026-05-19. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    hu_dpo_collector_create(&alloc, NULL, 100, &col);
    HU_ASSERT(hu_dpo_record_from_feedback(&col, "p", 1, "k", 1, true) == HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT(hu_dpo_record_from_feedback(&col, "p", 1, "GO", 2, true) == HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT(hu_dpo_record_from_feedback(&col, "p", 1, "SKIP", 4, true) == HU_OK);
    size_t count = 0;
    hu_dpo_pair_count(&col, &count);
    HU_ASSERT(count == 1);
    hu_dpo_collector_deinit(&col);
}

static void dpo_feedback_max_length_truncation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    hu_dpo_collector_create(&alloc, NULL, 100, &col);
    char huge[8192];
    memset(huge, 'A', sizeof(huge));
    hu_error_t err =
        hu_dpo_record_from_feedback(&col, huge, sizeof(huge), huge, sizeof(huge), true);
    HU_ASSERT(err == HU_OK);
    size_t count = 0;
    hu_dpo_pair_count(&col, &count);
    HU_ASSERT(count == 1);
    hu_dpo_collector_deinit(&col);
}

static void dpo_retry_with_identical_chosen_rejected(void) {
    /* Asserts the corrected contract: identical chosen == rejected has
     * NO learning signal and must be refused. Prior to 2026-05-19 this
     * test asserted HU_OK — codifying the bug. Audit row id=28 had this
     * exact shape ("I apologize for the mistake" in both columns).
     * Per ~/.claude/rules/tests-that-pin-bugs.md, test name claims a
     * security/correctness property; assertion must match. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    hu_dpo_collector_create(&alloc, NULL, 100, &col);
    hu_error_t err =
        hu_dpo_record_from_retry(&col, "prompt", 6, "same response", 13, "same response", 13);
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT);
    size_t count = 999;
    hu_dpo_pair_count(&col, &count);
    HU_ASSERT(count == 0);
    hu_dpo_collector_deinit(&col);
}

static void dpo_retry_refuses_critique_as_chosen(void) {
    /* Positive contract: the LLM's retry attempt occasionally echoes
     * the reflection critique back as its response, leading to
     * chosen="NEEDS_RETRY. The response 'GOOD' is irrelevant..." +
     * rejected="GOOD". 9/10 reflection_retry rows in the audited
     * corpus had this shape. Refuse the write. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    hu_dpo_collector_create(&alloc, NULL, 100, &col);
    const char *critique = "NEEDS_RETRY. The response 'GOOD' is irrelevant to the prompt.";
    /* Case-sensitive prefix */
    HU_ASSERT(hu_dpo_record_from_retry(&col, "prompt", 6, "GOOD", 4, critique, strlen(critique)) ==
              HU_ERR_INVALID_ARGUMENT);
    /* Lower-case also caught */
    HU_ASSERT(hu_dpo_record_from_retry(&col, "prompt", 6, "GOOD", 4, "needs_retry: too short",
                                       22) == HU_ERR_INVALID_ARGUMENT);
    /* A legitimate corrected response that happens to MENTION needs_retry
     * elsewhere in its body is fine — only PREFIX blocks the row. */
    const char *ok = "well, the answer is 42. The retry pass evaluated the prior NEEDS_RETRY case.";
    HU_ASSERT(hu_dpo_record_from_retry(&col, "prompt", 6, "GOOD", 4, ok, strlen(ok)) == HU_OK);
    size_t count = 0;
    hu_dpo_pair_count(&col, &count);
    HU_ASSERT(count == 1);
    hu_dpo_collector_deinit(&col);
}

static void dpo_rapid_fire_many_records(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    hu_dpo_collector_create(&alloc, NULL, 50, &col);
    for (int i = 0; i < 200; i++) {
        hu_dpo_record_from_feedback(&col, "p", 1, "resp", 4, i % 2 == 0);
    }
    size_t count = 0;
    hu_dpo_pair_count(&col, &count);
    HU_ASSERT(count == 200);
    hu_dpo_collector_deinit(&col);
}

static void dpo_create_with_zero_max_pairs(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    hu_error_t err = hu_dpo_collector_create(&alloc, NULL, 0, &col);
    HU_ASSERT(err == HU_OK);
    HU_ASSERT(col.max_pairs == 10000);
    hu_dpo_collector_deinit(&col);
}

static void dpo_clear_then_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dpo_collector_t col;
    hu_dpo_collector_create(&alloc, NULL, 100, &col);
    hu_dpo_record_from_feedback(&col, "p", 1, "resp", 4, true);
    hu_dpo_record_from_feedback(&col, "p", 1, "resp", 4, false);
    hu_dpo_clear(&col);
    size_t count = 99;
    hu_dpo_pair_count(&col, &count);
    HU_ASSERT(count == 0);
    hu_dpo_collector_deinit(&col);
}

/* ── Cross-system adversarial ────────────────────────────────── */

static void cross_srag_rag_disagree_on_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_srag_config_t srag_cfg = hu_srag_config_default();
    hu_srag_assessment_t assessment;
    hu_srag_should_retrieve(&alloc, &srag_cfg, "", 0, NULL, 0, &assessment);
    HU_ASSERT(assessment.decision == HU_SRAG_NO_RETRIEVAL);

    hu_adaptive_rag_t rag;
    hu_adaptive_rag_create(&alloc, NULL, &rag);
    hu_rag_strategy_t s = hu_adaptive_rag_select(&rag, "", 0);
    HU_ASSERT(s == HU_RAG_KEYWORD);
    hu_adaptive_rag_deinit(&rag);
}

static void cross_prm_feeds_dpo_correctly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prm_config_t prm_cfg = hu_prm_config_default();
    hu_dpo_collector_t col;
    hu_dpo_collector_create(&alloc, NULL, 100, &col);

    const char *good = "Therefore, the answer is 42 because the calculation shows x=42.";
    const char *bad = "Maybe the answer is something, I think, not sure, perhaps 42?";

    hu_prm_result_t good_res, bad_res;
    hu_prm_score_chain(&alloc, &prm_cfg, good, strlen(good), &good_res);
    hu_prm_score_chain(&alloc, &prm_cfg, bad, strlen(bad), &bad_res);

    HU_ASSERT(good_res.aggregate_score > bad_res.aggregate_score);

    hu_dpo_record_from_retry(&col, "What is the answer?", 19, bad, strlen(bad), good, strlen(good));
    size_t count = 0;
    hu_dpo_pair_count(&col, &count);
    HU_ASSERT(count == 1);

    hu_prm_result_free(&alloc, &good_res);
    hu_prm_result_free(&alloc, &bad_res);
    hu_dpo_collector_deinit(&col);
}

void run_sota_adversarial_tests(void) {
    HU_TEST_SUITE("SOTA Adversarial");

    /* Adaptive RAG */
    HU_RUN_TEST(rag_all_whitespace_query);
    HU_RUN_TEST(rag_single_char_query);
    HU_RUN_TEST(rag_binary_garbage_query);
    HU_RUN_TEST(rag_extreme_weight_values);
    HU_RUN_TEST(rag_features_max_length_query);
    HU_RUN_TEST(rag_record_outcome_boundary_strategy);

    /* Self-RAG */
    HU_RUN_TEST(srag_embedded_nulls_in_query);
    HU_RUN_TEST(srag_max_length_relevance);
    HU_RUN_TEST(srag_identical_query_and_content);
    HU_RUN_TEST(srag_all_greeting_variants);
    HU_RUN_TEST(srag_unicode_like_bytes);

    /* Memory Tiers */
    HU_RUN_TEST(tiers_update_core_max_length_value);
    HU_RUN_TEST(tiers_build_prompt_tiny_buffer);
    HU_RUN_TEST(tiers_build_prompt_exact_capacity);
    HU_RUN_TEST(tiers_auto_tier_empty_content);
    HU_RUN_TEST(tiers_update_all_fields_then_prompt);

    /* PRM */
    HU_RUN_TEST(prm_single_byte_input);
    HU_RUN_TEST(prm_only_newlines);
    HU_RUN_TEST(prm_65_steps_capped_at_64);
    HU_RUN_TEST(prm_all_hedge_words);
    HU_RUN_TEST(prm_mixed_split_formats);
    HU_RUN_TEST(prm_threshold_zero_all_valid);

    /* DPO */
    HU_RUN_TEST(dpo_feedback_with_zero_length_response);
    HU_RUN_TEST(dpo_feedback_trivially_short_response_rejected);
    HU_RUN_TEST(dpo_feedback_max_length_truncation);
    HU_RUN_TEST(dpo_retry_with_identical_chosen_rejected);
    HU_RUN_TEST(dpo_retry_refuses_critique_as_chosen);
    HU_RUN_TEST(dpo_rapid_fire_many_records);
    HU_RUN_TEST(dpo_create_with_zero_max_pairs);
    HU_RUN_TEST(dpo_clear_then_count);

    /* Cross-system */
    HU_RUN_TEST(cross_srag_rag_disagree_on_empty);
    HU_RUN_TEST(cross_prm_feeds_dpo_correctly);
}
