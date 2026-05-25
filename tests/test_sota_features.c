#include "human/agent/compaction.h"
#include "human/agent/constitutional.h"
#include "human/agent/dag.h"
#include "human/agent/dag_executor.h"
#include "human/agent/prompt.h"
#include "human/cognition/trust.h"
#include "human/context/conversation.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/eval/consistency.h"
#include "human/memory/fact_extract.h"
#include "human/memory/lifecycle/semantic_cache.h"
#include "human/persona/somatic.h"
#include "test_framework.h"
#include <string.h>

/* ── Hierarchical Compaction Tests ──────────────────────────────────────── */

static hu_owned_message_t make_msg(hu_allocator_t *alloc, hu_role_t role, const char *text) {
    hu_owned_message_t m;
    memset(&m, 0, sizeof(m));
    m.role = role;
    m.content = hu_strndup(alloc, text, strlen(text));
    m.content_len = strlen(text);
    return m;
}

static void hierarchical_compact_below_threshold_noop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_compaction_config_t cfg;
    hu_compaction_config_default(&cfg);
    cfg.keep_recent = 5;
    cfg.max_history_messages = 50;

    hu_owned_message_t history[3];
    history[0] = make_msg(&alloc, HU_ROLE_SYSTEM, "system prompt");
    history[1] = make_msg(&alloc, HU_ROLE_USER, "hello");
    history[2] = make_msg(&alloc, HU_ROLE_ASSISTANT, "hi");

    size_t count = 3;
    size_t cap = 3;
    hu_owned_message_t *hist = history;
    hu_error_t err = hu_compact_history_hierarchical(&alloc, &hist, &count, &cap,
                                                      &cfg, NULL, 5, 2);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 3u);

    for (size_t i = 0; i < count; i++) {
        if (history[i].content)
            alloc.free(alloc.ctx, history[i].content, history[i].content_len + 1);
    }
}

static void hierarchical_compact_produces_summary(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_compaction_config_t cfg;
    hu_compaction_config_default(&cfg);
    cfg.keep_recent = 2;
    cfg.max_history_messages = 10;

    size_t total = 30;
    hu_owned_message_t history[30];
    history[0] = make_msg(&alloc, HU_ROLE_SYSTEM, "system prompt");
    for (size_t i = 1; i < total; i++) {
        if (i % 2 == 1)
            history[i] = make_msg(&alloc, HU_ROLE_USER, "What is the weather today?");
        else
            history[i] = make_msg(&alloc, HU_ROLE_ASSISTANT, "The weather is sunny.");
    }

    size_t count = total;
    size_t cap = total;
    hu_owned_message_t *hist = history;
    hu_error_t err = hu_compact_history_hierarchical(&alloc, &hist, &count, &cap,
                                                      &cfg, NULL, 5, 3);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count < total);
    HU_ASSERT_TRUE(count >= 3); /* system + summary + at least 2 recent */

    /* First non-system message should be the hierarchical summary */
    HU_ASSERT_NOT_NULL(history[1].content);
    HU_ASSERT_TRUE(strstr(history[1].content, "[Hierarchical compaction") != NULL);

    for (size_t i = 0; i < count; i++) {
        if (history[i].content)
            alloc.free(alloc.ctx, history[i].content, history[i].content_len + 1);
    }
}

static void hierarchical_compact_null_args_returns_error(void) {
    hu_error_t err = hu_compact_history_hierarchical(NULL, NULL, NULL, NULL, NULL, NULL, 0, 0);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

/* ── Constitutional AI Tests ──────────────────────────────────────────── */

static void constitutional_default_has_three_principles(void) {
    hu_constitutional_config_t cfg = hu_constitutional_config_default();
    HU_ASSERT_EQ(cfg.principle_count, 3u);
    HU_ASSERT_STR_EQ(cfg.principles[0].name, "helpful");
    HU_ASSERT_STR_EQ(cfg.principles[1].name, "harmless");
    HU_ASSERT_STR_EQ(cfg.principles[2].name, "honest");
    HU_ASSERT_TRUE(cfg.enabled);
    HU_ASSERT_TRUE(cfg.rewrite_enabled);
}

static void constitutional_critique_passes_under_test(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_constitutional_config_t cfg = hu_constitutional_config_default();
    hu_critique_result_t result;
    memset(&result, 0, sizeof(result));

    hu_error_t err = hu_constitutional_critique(&alloc, NULL, "test", 4,
                                                "hi", 2, "hello", 5, &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.verdict, HU_CRITIQUE_PASS);
    hu_critique_result_free(&alloc, &result);
}

static void constitutional_disabled_passes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_constitutional_config_t cfg = hu_constitutional_config_default();
    cfg.enabled = false;
    hu_critique_result_t result;
    memset(&result, 0, sizeof(result));

    hu_error_t err = hu_constitutional_critique(&alloc, NULL, "test", 4,
                                                "hi", 2, "hello", 5, &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.verdict, HU_CRITIQUE_PASS);
}

/* ── System Prompt with Constitutional Principles ────────────────────── */

static void prompt_includes_constitutional_principles(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.constitutional_principles = "- Be helpful\n- Be harmless\n- Be honest\n";
    cfg.constitutional_principles_len = strlen(cfg.constitutional_principles);

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Core Principles") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Be helpful") != NULL);
    hu_str_free(&alloc, out);
}

static void prompt_without_principles_has_no_section(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Core Principles") == NULL);
    hu_str_free(&alloc, out);
}

/* ── DAG Executor Integration Tests ─────────────────────────────────── */

static void dag_batch_execution_with_var_resolution(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    hu_dag_add_node(&dag, "t1", "search", "{\"q\":\"weather\"}", NULL, 0);
    hu_dag_add_node(&dag, "t2", "summarize", "{\"text\":\"$t1\"}", (const char *[]){"t1"}, 1);

    /* Batch 1: root nodes */
    hu_dag_batch_t batch;
    hu_error_t err = hu_dag_next_batch(&dag, &batch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(batch.count, 1u);
    HU_ASSERT_STR_EQ(batch.nodes[0]->id, "t1");

    /* Simulate t1 completion */
    batch.nodes[0]->status = HU_DAG_DONE;
    batch.nodes[0]->result = hu_strdup(&alloc, "sunny 75F");
    batch.nodes[0]->result_len = 9;

    /* Batch 2: dependent node with $t1 resolution */
    err = hu_dag_next_batch(&dag, &batch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(batch.count, 1u);
    HU_ASSERT_STR_EQ(batch.nodes[0]->id, "t2");

    /* Resolve vars for t2 */
    char *resolved = NULL;
    size_t resolved_len = 0;
    err = hu_dag_resolve_vars(&alloc, &dag, batch.nodes[0]->args_json,
                              strlen(batch.nodes[0]->args_json), &resolved, &resolved_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(resolved);
    HU_ASSERT_TRUE(strstr(resolved, "sunny 75F") != NULL);
    HU_ASSERT_TRUE(strstr(resolved, "$t1") == NULL);
    hu_str_free(&alloc, resolved);

    hu_dag_deinit(&dag);
}

static void dag_parallel_batch_returns_independent_nodes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dag_t dag;
    hu_dag_init(&dag, alloc);

    hu_dag_add_node(&dag, "t1", "search", "{}", NULL, 0);
    hu_dag_add_node(&dag, "t2", "fetch", "{}", NULL, 0);
    hu_dag_add_node(&dag, "t3", "lookup", "{}", NULL, 0);
    hu_dag_add_node(&dag, "t4", "merge", "{}", (const char *[]){"t1", "t2", "t3"}, 3);

    hu_dag_batch_t batch;
    hu_error_t err = hu_dag_next_batch(&dag, &batch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(batch.count, 3u);

    /* Complete all roots */
    for (size_t i = 0; i < batch.count; i++)
        batch.nodes[i]->status = HU_DAG_DONE;

    /* Now t4 should be ready */
    err = hu_dag_next_batch(&dag, &batch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(batch.count, 1u);
    HU_ASSERT_STR_EQ(batch.nodes[0]->id, "t4");

    hu_dag_deinit(&dag);
}

/* ── Semantic Cache Tests ──────────────────────────────────────────── */

static void semantic_cache_exact_match(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_semantic_cache_t *cache = hu_semantic_cache_create(&alloc, 60, 100, 0.85f, NULL);
    HU_ASSERT_NOT_NULL(cache);

    hu_error_t err = hu_semantic_cache_put(cache, &alloc, "key1", 4, "model", 5,
                                            "response1", 9, 10, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_semantic_cache_hit_t hit;
    memset(&hit, 0, sizeof(hit));
    err = hu_semantic_cache_get(cache, &alloc, "key1", 4, NULL, 0, &hit);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(hit.response);
    HU_ASSERT_STR_EQ(hit.response, "response1");
    HU_ASSERT_EQ(hit.semantic, 0);
    hu_semantic_cache_hit_free(&alloc, &hit);

    hu_semantic_cache_destroy(&alloc, cache);
}

static void semantic_cache_miss(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_semantic_cache_t *cache = hu_semantic_cache_create(&alloc, 60, 100, 0.85f, NULL);
    HU_ASSERT_NOT_NULL(cache);

    hu_semantic_cache_hit_t hit;
    memset(&hit, 0, sizeof(hit));
    hu_error_t err = hu_semantic_cache_get(cache, &alloc, "nonexistent", 11, NULL, 0, &hit);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
    HU_ASSERT_NULL(hit.response);

    hu_semantic_cache_destroy(&alloc, cache);
}

static void semantic_cache_overwrite(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_semantic_cache_t *cache = hu_semantic_cache_create(&alloc, 60, 100, 0.85f, NULL);

    hu_semantic_cache_put(cache, &alloc, "k", 1, NULL, 0, "old", 3, 0, NULL, 0);
    hu_semantic_cache_put(cache, &alloc, "k", 1, NULL, 0, "new", 3, 0, NULL, 0);

    hu_semantic_cache_hit_t hit;
    memset(&hit, 0, sizeof(hit));
    hu_error_t err = hu_semantic_cache_get(cache, &alloc, "k", 1, NULL, 0, &hit);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(hit.response, "new");
    hu_semantic_cache_hit_free(&alloc, &hit);

    hu_semantic_cache_destroy(&alloc, cache);
}

/* ── Emotion Detection Tests ──────────────────────────────────────────── */

static hu_channel_history_entry_t make_entry(const char *text, bool from_me) {
    hu_channel_history_entry_t e;
    memset(&e, 0, sizeof(e));
    size_t len = strlen(text);
    if (len >= sizeof(e.text)) len = sizeof(e.text) - 1;
    memcpy(e.text, text, len);
    e.text[len] = '\0';
    e.from_me = from_me;
    return e;
}

static void emotion_neutral_for_empty(void) {
    hu_emotional_state_t s = hu_conversation_detect_emotion(NULL, 0);
    HU_ASSERT_STR_EQ(s.dominant_emotion, "neutral");
    HU_ASSERT_FLOAT_EQ(s.valence, 0.0f, 0.01f);
}

static void emotion_positive_for_joy_words(void) {
    hu_channel_history_entry_t entries[] = {
        make_entry("I'm so happy and excited today!", false),
    };
    hu_emotional_state_t s = hu_conversation_detect_emotion(entries, 1);
    HU_ASSERT_TRUE(s.valence > 0.0f);
    HU_ASSERT_TRUE(s.intensity > 0.0f);
}

static void emotion_negative_for_sadness(void) {
    hu_channel_history_entry_t entries[] = {
        make_entry("I'm really sad and depressed", false),
    };
    hu_emotional_state_t s = hu_conversation_detect_emotion(entries, 1);
    HU_ASSERT_TRUE(s.valence < 0.0f);
    HU_ASSERT_TRUE(s.intensity > 0.0f);
}

static void emotion_negation_flips_valence(void) {
    hu_channel_history_entry_t entries[] = {
        make_entry("I'm not happy at all", false),
    };
    hu_emotional_state_t s = hu_conversation_detect_emotion(entries, 1);
    HU_ASSERT_TRUE(s.valence < 0.0f);
}

static void emotion_intensifier_increases_score(void) {
    hu_channel_history_entry_t base_entries[] = {
        make_entry("I'm sad", false),
    };
    hu_channel_history_entry_t intense_entries[] = {
        make_entry("I'm extremely sad", false),
    };
    hu_emotional_state_t base = hu_conversation_detect_emotion(base_entries, 1);
    hu_emotional_state_t intense = hu_conversation_detect_emotion(intense_entries, 1);
    HU_ASSERT_TRUE(intense.intensity > base.intensity);
}

static void emotion_anger_detected(void) {
    hu_channel_history_entry_t entries[] = {
        make_entry("I'm so angry and furious right now", false),
    };
    hu_emotional_state_t s = hu_conversation_detect_emotion(entries, 1);
    HU_ASSERT_TRUE(s.valence < 0.0f);
    HU_ASSERT_TRUE(strcmp(s.dominant_emotion, "furious") == 0 ||
                    strcmp(s.dominant_emotion, "frustrated") == 0 ||
                    strcmp(s.dominant_emotion, "upset") == 0);
}

static void emotion_anxiety_detected(void) {
    hu_channel_history_entry_t entries[] = {
        make_entry("I'm really anxious and scared about tomorrow", false),
    };
    hu_emotional_state_t s = hu_conversation_detect_emotion(entries, 1);
    HU_ASSERT_TRUE(s.valence < 0.0f);
}

static void emotion_concerning_high_negative(void) {
    hu_channel_history_entry_t entries[] = {
        make_entry("I'm so depressed and crying all the time, I feel broken", false),
    };
    hu_emotional_state_t s = hu_conversation_detect_emotion(entries, 1);
    HU_ASSERT_TRUE(s.concerning);
    HU_ASSERT_TRUE(s.valence < -0.5f);
}

static void emotion_mixed_signals(void) {
    hu_channel_history_entry_t entries[] = {
        make_entry("I love this but I'm also stressed about work", false),
    };
    hu_emotional_state_t s = hu_conversation_detect_emotion(entries, 1);
    HU_ASSERT_TRUE(s.intensity > 0.0f);
}

static void emotion_skips_own_messages(void) {
    hu_channel_history_entry_t entries[] = {
        make_entry("I'm very depressed and crying", true),
    };
    hu_emotional_state_t s = hu_conversation_detect_emotion(entries, 1);
    HU_ASSERT_STR_EQ(s.dominant_emotion, "neutral");
}

/* ===== Frontier integration tests ===== */

static void trust_update_modifies_composite(void) {
    hu_tcal_state_t state;
    memset(&state, 0, sizeof(state));
    hu_tcal_init(&state);
    float before = state.composite;
    HU_ASSERT_TRUE(before > 0.0f);
    hu_tcal_update(&state, 0.9f, 0.8f, 0.7f);
    HU_ASSERT_TRUE(state.dimensions.competence > before);
    HU_ASSERT_TRUE(state.dimensions.benevolence > before);
    HU_ASSERT_TRUE(state.dimensions.integrity > before);
    HU_ASSERT_TRUE(state.composite != before);
    HU_ASSERT_TRUE(state.composite > before);
}

static void somatic_low_energy_below_threshold(void) {
    hu_somatic_state_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.energy = 0.2f;
    ss.social_battery = 0.6f;
    ss.focus = 0.4f;
    HU_ASSERT_TRUE(ss.energy < 0.3f);
    hu_allocator_t alloc = hu_system_allocator();
    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_somatic_build_context(&alloc, &ss, &ctx, &ctx_len), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(ctx_len > 0);
    HU_ASSERT_NOT_NULL(strstr(ctx, "Energy"));
    alloc.free(alloc.ctx, ctx, ctx_len + 1);
}

static void consistency_score_similar_text_high(void) {
    const char *a = "I love hiking in the mountains and being outdoors";
    const char *b = "I enjoy hiking and spending time in the mountains outdoors";
    float score = 0.0f;
    HU_ASSERT_EQ(hu_consistency_score_line(a, strlen(a), b, strlen(b), &score), HU_OK);
    HU_ASSERT_TRUE(score > 0.3f);
}

static void fact_extract_basic_statement(void) {
    const char *text = "My name is Alice and I live in Boston.";
    hu_fact_extract_result_t res;
    memset(&res, 0, sizeof(res));
    HU_ASSERT_EQ(hu_fact_extract(text, strlen(text), &res), HU_OK);
    HU_ASSERT_TRUE(res.fact_count > 0);
    bool found_name = false, found_location = false;
    for (size_t i = 0; i < res.fact_count; i++) {
        if (strstr(res.facts[i].subject, "lice") || strstr(res.facts[i].object, "lice"))
            found_name = true;
        if (strstr(res.facts[i].object, "oston") || strstr(res.facts[i].predicate, "live"))
            found_location = true;
    }
    HU_ASSERT_TRUE(found_name || found_location);
}

static void fact_dedup_removes_duplicates(void) {
    hu_heuristic_fact_t existing[1] = {{
        .subject = "alice",
        .predicate = "lives in",
        .object = "boston",
        .confidence = 0.9f,
    }};
    hu_fact_extract_result_t res;
    memset(&res, 0, sizeof(res));
    res.fact_count = 1;
    res.facts[0] = existing[0];
    size_t novel = hu_fact_dedup(&res, existing, 1);
    HU_ASSERT_EQ(novel, (size_t)0);

    hu_heuristic_fact_t different = {
        .subject = "bob", .predicate = "works at", .object = "acme",
        .confidence = 0.8f,
    };
    res.fact_count = 1;
    res.facts[0] = different;
    novel = hu_fact_dedup(&res, existing, 1);
    HU_ASSERT_EQ(novel, (size_t)1);
}

static void prompt_includes_conv_goals_section(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.provider_name = "test";
    cfg.provider_name_len = 4;
    cfg.model_name = "test-model";
    cfg.model_name_len = 10;
    cfg.conv_goals_context = "- Goal: check in about mood";
    cfg.conv_goals_context_len = 27;
    char *prompt = NULL;
    size_t prompt_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, NULL, &prompt, &prompt_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_NOT_NULL(strstr(prompt, "Conversation Goals"));
    HU_ASSERT_NOT_NULL(strstr(prompt, "check in about mood"));
    alloc.free(alloc.ctx, prompt, prompt_len + 1);
}

void run_sota_features_tests(void) {
    HU_TEST_SUITE("sota_features");

    /* Hierarchical compaction */
    HU_RUN_TEST(hierarchical_compact_below_threshold_noop);
    HU_RUN_TEST(hierarchical_compact_produces_summary);
    HU_RUN_TEST(hierarchical_compact_null_args_returns_error);

    /* Constitutional AI */
    HU_RUN_TEST(constitutional_default_has_three_principles);
    HU_RUN_TEST(constitutional_critique_passes_under_test);
    HU_RUN_TEST(constitutional_disabled_passes);

    /* System prompt with principles */
    HU_RUN_TEST(prompt_includes_constitutional_principles);
    HU_RUN_TEST(prompt_without_principles_has_no_section);

    /* DAG executor integration */
    HU_RUN_TEST(dag_batch_execution_with_var_resolution);
    HU_RUN_TEST(dag_parallel_batch_returns_independent_nodes);

    /* Semantic cache */
    HU_RUN_TEST(semantic_cache_exact_match);
    HU_RUN_TEST(semantic_cache_miss);
    HU_RUN_TEST(semantic_cache_overwrite);

    /* Emotion detection */
    HU_RUN_TEST(emotion_neutral_for_empty);
    HU_RUN_TEST(emotion_positive_for_joy_words);
    HU_RUN_TEST(emotion_negative_for_sadness);
    HU_RUN_TEST(emotion_negation_flips_valence);
    HU_RUN_TEST(emotion_intensifier_increases_score);
    HU_RUN_TEST(emotion_anger_detected);
    HU_RUN_TEST(emotion_anxiety_detected);
    HU_RUN_TEST(emotion_concerning_high_negative);
    HU_RUN_TEST(emotion_mixed_signals);
    HU_RUN_TEST(emotion_skips_own_messages);

    HU_TEST_SUITE("frontier_integration");
    HU_RUN_TEST(trust_update_modifies_composite);
    HU_RUN_TEST(somatic_low_energy_below_threshold);
    HU_RUN_TEST(consistency_score_similar_text_high);
    HU_RUN_TEST(fact_extract_basic_statement);
    HU_RUN_TEST(fact_dedup_removes_duplicates);
    HU_RUN_TEST(prompt_includes_conv_goals_section);
}
