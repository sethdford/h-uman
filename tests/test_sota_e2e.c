#include "test_framework.h"
#include "human/memory/adaptive_rag.h"
#include "human/memory/self_rag.h"
#include "human/memory/tiers.h"
#include "human/agent/process_reward.h"
#include "human/ml/dpo.h"
#include <string.h>
#include <math.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

/*
 * End-to-end integration tests proving all 5 SOTA systems work together
 * in the agent turn pipeline.
 */

static void e2e_full_pipeline_personal_query(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* 1. Self-RAG gate: personal query → RETRIEVE */
    hu_srag_config_t srag_cfg = hu_srag_config_default();
    hu_srag_assessment_t assessment;
    const char *query = "remind me of my project goals";
    HU_ASSERT_EQ(hu_srag_should_retrieve(&alloc, &srag_cfg, query, strlen(query),
                                          NULL, 0, &assessment), HU_OK);
    HU_ASSERT_EQ((int)assessment.decision, (int)HU_SRAG_RETRIEVE);
    HU_ASSERT(assessment.is_personal_query);

    /* 2. Adaptive RAG: select strategy for retrieval */
    hu_adaptive_rag_t arag;
    HU_ASSERT_EQ(hu_adaptive_rag_create(&alloc, NULL, &arag), HU_OK);
    hu_rag_strategy_t strategy = hu_adaptive_rag_select(&arag, query, strlen(query));
    HU_ASSERT(strategy != HU_RAG_NONE);

    /* 3. Simulate retrieval result, verify relevance */
    const char *retrieved = "User's project goals include shipping v2 and improving performance.";
    double relevance = 0.0;
    bool should_use = false;
    HU_ASSERT_EQ(hu_srag_verify_relevance(&alloc, &srag_cfg, query, strlen(query),
                                           retrieved, strlen(retrieved),
                                           &relevance, &should_use), HU_OK);
    HU_ASSERT(should_use);

    /* 4. Record retrieval outcome for learning */
    HU_ASSERT_EQ(hu_adaptive_rag_record_outcome(&arag, strategy, relevance), HU_OK);

    hu_adaptive_rag_deinit(&arag);
}

static void e2e_full_pipeline_creative_query(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* 1. Self-RAG gate: creative query → NO_RETRIEVAL (skip memory) */
    hu_srag_config_t srag_cfg = hu_srag_config_default();
    hu_srag_assessment_t assessment;
    const char *query = "write me a haiku about programming";
    HU_ASSERT_EQ(hu_srag_should_retrieve(&alloc, &srag_cfg, query, strlen(query),
                                          NULL, 0, &assessment), HU_OK);
    HU_ASSERT_EQ((int)assessment.decision, (int)HU_SRAG_NO_RETRIEVAL);
    HU_ASSERT(assessment.is_creative_query);

    /* 2. PRM: score the LLM response reasoning */
    hu_prm_config_t prm_cfg = hu_prm_config_default();
    hu_prm_result_t prm_result;
    const char *response = "Here's a haiku:\n\nCode flows like water\nBugs emerge from the darkness\nTests catch them at dawn";
    HU_ASSERT_EQ(hu_prm_score_chain(&alloc, &prm_cfg, response, strlen(response), &prm_result), HU_OK);
    HU_ASSERT(prm_result.step_count > 0);
    HU_ASSERT(prm_result.aggregate_score > 0.0);
    hu_prm_result_free(&alloc, &prm_result);
}

static void e2e_full_pipeline_factual_with_retry(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* 1. Self-RAG: factual → RETRIEVE_AND_VERIFY */
    hu_srag_config_t srag_cfg = hu_srag_config_default();
    hu_srag_assessment_t assessment;
    const char *query = "what is the current deployment status";
    HU_ASSERT_EQ(hu_srag_should_retrieve(&alloc, &srag_cfg, query, strlen(query),
                                          NULL, 0, &assessment), HU_OK);
    HU_ASSERT_EQ((int)assessment.decision, (int)HU_SRAG_RETRIEVE_AND_VERIFY);

    /* 2. Adaptive RAG: corrective strategy for factual */
    hu_adaptive_rag_t arag;
    HU_ASSERT_EQ(hu_adaptive_rag_create(&alloc, NULL, &arag), HU_OK);
    hu_rag_strategy_t strategy = hu_adaptive_rag_select(&arag, query, strlen(query));
    HU_ASSERT(strategy == HU_RAG_CORRECTIVE || strategy == HU_RAG_HYBRID ||
              strategy == HU_RAG_SEMANTIC);

    /* 3. PRM: first response has hedging → low score */
    hu_prm_config_t prm_cfg = hu_prm_config_default();
    hu_prm_result_t prm_result1;
    const char *bad_response = "Maybe the deployment is done perhaps. I think it might be running.";
    HU_ASSERT_EQ(hu_prm_score_chain(&alloc, &prm_cfg, bad_response, strlen(bad_response), &prm_result1), HU_OK);
    HU_ASSERT(prm_result1.aggregate_score < 0.5);

    /* 4. Retry produces better response */
    hu_prm_result_t prm_result2;
    const char *good_response = "The deployment completed at 14:32 UTC because all 47 tests passed.";
    HU_ASSERT_EQ(hu_prm_score_chain(&alloc, &prm_cfg, good_response, strlen(good_response), &prm_result2), HU_OK);
    HU_ASSERT(prm_result2.aggregate_score > prm_result1.aggregate_score);

    /* 5. DPO: record preference pair from the retry */
    hu_dpo_collector_t dpo;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, NULL, 0, &dpo), HU_OK);
    HU_ASSERT_EQ(hu_dpo_record_from_retry(&dpo, query, strlen(query),
                                           bad_response, strlen(bad_response),
                                           good_response, strlen(good_response)), HU_OK);
    size_t pair_count = 0;
    hu_dpo_pair_count(&dpo, &pair_count);
    HU_ASSERT_EQ((int)pair_count, 1);

    hu_prm_result_free(&alloc, &prm_result1);
    hu_prm_result_free(&alloc, &prm_result2);
    hu_dpo_collector_deinit(&dpo);
    hu_adaptive_rag_deinit(&arag);
}

#ifdef HU_ENABLE_SQLITE
static void e2e_memory_tiers_with_retrieval(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);

    /* 1. Setup tier manager */
    hu_tier_manager_t tiers;
    HU_ASSERT_EQ(hu_tier_manager_create(&alloc, db, &tiers), HU_OK);
    HU_ASSERT_EQ(hu_tier_manager_init_tables(&tiers), HU_OK);

    /* 2. Store user identity → auto-tiers to CORE */
    hu_memory_tier_t assigned;
    hu_tier_manager_auto_tier(&tiers, "user_intro", 10, "my name is Alice", 16, &assigned);
    HU_ASSERT_EQ((int)assigned, (int)HU_TIER_CORE);

    /* 3. Store conversation fact → auto-tiers to RECALL */
    hu_tier_manager_auto_tier(&tiers, "fact1", 5, "we discussed project deadline", 29, &assigned);
    HU_ASSERT_EQ((int)assigned, (int)HU_TIER_RECALL);

    /* 4. Build core prompt for prompt assembly */
    hu_tier_manager_update_core(&tiers, "user_name", 9, "Alice", 5);
    hu_tier_manager_update_core(&tiers, "active_goals", 12, "ship v2", 7);
    char prompt[2048];
    size_t prompt_len = 0;
    HU_ASSERT_EQ(hu_tier_manager_build_core_prompt(&tiers, prompt, sizeof(prompt), &prompt_len), HU_OK);
    HU_ASSERT(prompt_len > 0);
    HU_ASSERT(strstr(prompt, "Alice") != NULL);
    HU_ASSERT(strstr(prompt, "ship v2") != NULL);

    /* 5. Self-RAG decides to retrieve based on user query */
    hu_srag_config_t srag_cfg = hu_srag_config_default();
    hu_srag_assessment_t assessment;
    HU_ASSERT_EQ(hu_srag_should_retrieve(&alloc, &srag_cfg, "my project status", 17,
                                          NULL, 0, &assessment), HU_OK);
    HU_ASSERT_EQ((int)assessment.decision, (int)HU_SRAG_RETRIEVE);

    /* 6. Adaptive RAG selects strategy */
    hu_adaptive_rag_t arag;
    HU_ASSERT_EQ(hu_adaptive_rag_create(&alloc, NULL, &arag), HU_OK);
    hu_rag_strategy_t strategy = hu_adaptive_rag_select(&arag, "my project status", 17);
    HU_ASSERT(strategy != HU_RAG_NONE);

    hu_adaptive_rag_deinit(&arag);
    hu_tier_manager_deinit(&tiers);
    sqlite3_close(db);
}
#endif

static void e2e_dpo_from_prm_scoring(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* PRM scores two candidate responses to the same prompt */
    hu_prm_config_t prm_cfg = hu_prm_config_default();
    const char *prompt = "explain the memory architecture";

    hu_prm_result_t result_a;
    const char *resp_a = "The memory system uses 3 tiers: core (always in prompt), "
                         "recall (indexed), and archival (compressed). Therefore retrieval "
                         "is efficient because each tier serves a different access pattern.";
    HU_ASSERT_EQ(hu_prm_score_chain(&alloc, &prm_cfg, resp_a, strlen(resp_a), &result_a), HU_OK);

    hu_prm_result_t result_b;
    const char *resp_b = "Maybe the memory works somehow. I think it might store things perhaps.";
    HU_ASSERT_EQ(hu_prm_score_chain(&alloc, &prm_cfg, resp_b, strlen(resp_b), &result_b), HU_OK);

    /* Response A should score higher */
    HU_ASSERT(result_a.aggregate_score > result_b.aggregate_score);

    /* Record DPO pair: A=chosen, B=rejected */
    hu_dpo_collector_t dpo;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, NULL, 0, &dpo), HU_OK);
    HU_ASSERT_EQ(hu_dpo_record_from_retry(&dpo, prompt, strlen(prompt),
                                           resp_b, strlen(resp_b),
                                           resp_a, strlen(resp_a)), HU_OK);
    size_t count = 0;
    hu_dpo_pair_count(&dpo, &count);
    HU_ASSERT_EQ((int)count, 1);

    /* Export succeeds */
    size_t exported = 0;
    HU_ASSERT_EQ(hu_dpo_export_jsonl(&dpo, "prm_pairs.jsonl", 15, &exported), HU_OK);
    HU_ASSERT_EQ((int)exported, 1);

    hu_prm_result_free(&alloc, &result_a);
    hu_prm_result_free(&alloc, &result_b);
    hu_dpo_collector_deinit(&dpo);
}

static void e2e_all_systems_feedback_loop(void) {
    hu_allocator_t alloc = hu_system_allocator();

    const char *query = "what were my goals from last month";

    /* 1. Self-RAG: temporal + personal → RETRIEVE */
    hu_srag_config_t srag_cfg = hu_srag_config_default();
    hu_srag_assessment_t assessment;
    HU_ASSERT_EQ(hu_srag_should_retrieve(&alloc, &srag_cfg, query, strlen(query),
                                          NULL, 0, &assessment), HU_OK);
    HU_ASSERT(assessment.decision == HU_SRAG_RETRIEVE ||
              assessment.decision == HU_SRAG_RETRIEVE_AND_VERIFY);

    /* 2. Adaptive RAG: picks strategy */
    hu_adaptive_rag_t arag;
    HU_ASSERT_EQ(hu_adaptive_rag_create(&alloc, NULL, &arag), HU_OK);
    hu_rag_strategy_t strategy = hu_adaptive_rag_select(&arag, query, strlen(query));

    /* 3. Simulate LLM response, score with PRM */
    hu_prm_config_t prm_cfg = hu_prm_config_default();
    hu_prm_result_t prm_result;
    const char *response = "Based on the records, your goals from last month were:\n\n"
                           "1. Complete the API migration by March 15\n"
                           "2. Review 3 pull requests per week\n"
                           "3. Ship the v2 dashboard";
    HU_ASSERT_EQ(hu_prm_score_chain(&alloc, &prm_cfg, response, strlen(response), &prm_result), HU_OK);
    HU_ASSERT(prm_result.step_count >= 2);
    HU_ASSERT(prm_result.chain_valid);

    /* 4. User gives positive feedback → DPO records */
    hu_dpo_collector_t dpo;
    HU_ASSERT_EQ(hu_dpo_collector_create(&alloc, NULL, 0, &dpo), HU_OK);
    HU_ASSERT_EQ(hu_dpo_record_from_feedback(&dpo, query, strlen(query),
                                              response, strlen(response), true), HU_OK);

    /* 5. Feed retrieval quality back for adaptive RAG learning */
    HU_ASSERT_EQ(hu_adaptive_rag_record_outcome(&arag, strategy, 0.9), HU_OK);

    /* Verify the full feedback loop: strategy weight should increase */
    HU_ASSERT(arag.strategy_weights[strategy] > 0.9);

    /* (2026-05-31) Feedback routes to feedback_signals, NOT dpo_pairs — a +/-
     * reaction has no counterpart, so dpo_pairs stays empty here. The signal is
     * preserved for reward-model / KTO training. */
    size_t pair_count = 99;
    hu_dpo_pair_count(&dpo, &pair_count);
    HU_ASSERT_EQ((int)pair_count, 0);

    hu_prm_result_free(&alloc, &prm_result);
    hu_dpo_collector_deinit(&dpo);
    hu_adaptive_rag_deinit(&arag);
}

static void e2e_strategy_names_all_valid(void) {
    HU_ASSERT_STR_EQ(hu_rag_strategy_str(HU_RAG_NONE), "none");
    HU_ASSERT_STR_EQ(hu_rag_strategy_str(HU_RAG_KEYWORD), "keyword");
    HU_ASSERT_STR_EQ(hu_rag_strategy_str(HU_RAG_SEMANTIC), "semantic");
    HU_ASSERT_STR_EQ(hu_rag_strategy_str(HU_RAG_HYBRID), "hybrid");
    HU_ASSERT_STR_EQ(hu_rag_strategy_str(HU_RAG_GRAPH), "graph");
    HU_ASSERT_STR_EQ(hu_rag_strategy_str(HU_RAG_CORRECTIVE), "corrective");
    HU_ASSERT_STR_EQ(hu_memory_tier_str(HU_TIER_CORE), "core");
    HU_ASSERT_STR_EQ(hu_memory_tier_str(HU_TIER_RECALL), "recall");
    HU_ASSERT_STR_EQ(hu_memory_tier_str(HU_TIER_ARCHIVAL), "archival");
}

void run_sota_e2e_tests(void) {
    HU_TEST_SUITE("SOTA E2E");
    HU_RUN_TEST(e2e_full_pipeline_personal_query);
    HU_RUN_TEST(e2e_full_pipeline_creative_query);
    HU_RUN_TEST(e2e_full_pipeline_factual_with_retry);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(e2e_memory_tiers_with_retrieval);
#endif
    HU_RUN_TEST(e2e_dpo_from_prm_scoring);
    HU_RUN_TEST(e2e_all_systems_feedback_loop);
    HU_RUN_TEST(e2e_strategy_names_all_valid);
}
