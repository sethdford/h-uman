#include "human/agent/prompt.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/fact_extract.h"
#include "human/memory/personal_model.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

static void personal_model_init_sets_defaults(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    HU_ASSERT_EQ((long)m.version, 1L);
    HU_ASSERT_EQ((long)m.created_at, 0L);
    HU_ASSERT_EQ((long)m.fact_count, 0L);
    HU_ASSERT_EQ((long)m.topic_count, 0L);
    HU_ASSERT_EQ((long)m.goal_count, 0L);
    HU_ASSERT_EQ((unsigned)m.style.sample_count, 0U);
}

static void personal_model_ingest_extracts_facts(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    const char *text = "I like hiking, I live in Portland";
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, text, strlen(text), true, 1700000000LL), HU_OK);
    HU_ASSERT_TRUE(m.fact_count >= 2U);
}

static void personal_model_merge_facts_deduplicates(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    m.updated_at = 1;

    hu_fact_extract_result_t batch;
    memset(&batch, 0, sizeof(batch));
    strncpy(batch.facts[0].subject, "user", sizeof(batch.facts[0].subject) - 1);
    strncpy(batch.facts[0].predicate, "i like", sizeof(batch.facts[0].predicate) - 1);
    strncpy(batch.facts[0].object, "tea", sizeof(batch.facts[0].object) - 1);
    batch.facts[0].type = HU_KNOWLEDGE_PROPOSITIONAL;
    batch.facts[0].confidence = 0.7f;
    batch.fact_count = 1;

    HU_ASSERT_EQ(hu_personal_model_merge_facts(&m, &batch), HU_OK);
    HU_ASSERT_EQ((long)m.fact_count, 1L);
    HU_ASSERT_EQ(hu_personal_model_merge_facts(&m, &batch), HU_OK);
    HU_ASSERT_EQ((long)m.fact_count, 1L);
}

static void personal_model_build_prompt_non_empty(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    char buf[2048];
    size_t n = hu_personal_model_build_prompt(&m, buf, sizeof(buf));
    HU_ASSERT_GT((long)n, 0L);
    HU_ASSERT_TRUE(strstr(buf, "[Personal Context]") != NULL);
}

static void personal_model_query_preference_finds_match(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    const char *text = "I prefer dark mode for coding";
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, text, strlen(text), true, 0), HU_OK);
    const hu_heuristic_fact_t *f = hu_personal_model_query_preference(&m, "dark", 4);
    HU_ASSERT_NOT_NULL(f);
}

static void personal_model_ingest_updates_style_metrics(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    const char *text = "Hello there";
    size_t len = strlen(text);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, text, len, true, 0), HU_OK);
    HU_ASSERT_EQ((unsigned)m.style.sample_count, 1U);
    HU_ASSERT_EQ((unsigned)m.style.avg_message_length, (unsigned)len);
}

static void personal_model_has_content_false_when_fresh(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    HU_ASSERT_FALSE(hu_personal_model_has_content(&m));
    HU_ASSERT_FALSE(hu_personal_model_has_content(NULL));
}

static void personal_model_has_content_true_after_fact(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    const char *text = "I like hiking";
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, text, strlen(text), true, 1700000000LL), HU_OK);
    HU_ASSERT_TRUE(hu_personal_model_has_content(&m));
}

static void personal_model_has_content_true_after_style_observation(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    const char *text = "ok";
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, text, strlen(text), true, 1700000000LL), HU_OK);
    HU_ASSERT_TRUE(hu_personal_model_has_content(&m));
}

/* Integration: prove that when an agent's personal model has content and
 * is wired into the prompt config, the rendered system prompt actually
 * contains the user's facts. This is the regression test that closes the
 * "personal model is ingested but never injected" gap. */
static void personal_model_reaches_system_prompt_via_config(void) {
    hu_personal_model_t m;
    hu_personal_model_init(&m);
    strncpy(m.core.user_name, "Sethford", sizeof(m.core.user_name) - 1);
    const char *text1 = "I love rock climbing on weekends";
    const char *text2 = "I prefer dark roast coffee in the morning";
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, text1, strlen(text1), true, 1700000000LL), HU_OK);
    HU_ASSERT_EQ(hu_personal_model_ingest(&m, text2, strlen(text2), true, 1700000060LL), HU_OK);
    HU_ASSERT_TRUE(hu_personal_model_has_content(&m));

    char pm_buf[8192];
    size_t pm_len = hu_personal_model_build_prompt(&m, pm_buf, sizeof(pm_buf));
    HU_ASSERT_GT((long)pm_len, 0L);

    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg = {
        .provider_name = "test",
        .provider_name_len = 4,
        .model_name = "test-model",
        .model_name_len = 10,
        .autonomy_level = 1,
        .personal_model_context = pm_buf,
        .personal_model_context_len = pm_len,
    };

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "[Personal Context]") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Sethford") != NULL);
    HU_ASSERT_TRUE(strstr(out, "climbing") != NULL || strstr(out, "coffee") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

/* Adversarial: when no personal model context is set, the prompt should
 * still render cleanly with no [Personal Context] block leaking through. */
static void personal_model_absent_does_not_leak_into_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg = {
        .provider_name = "test",
        .provider_name_len = 4,
        .model_name = "test-model",
        .model_name_len = 10,
        .autonomy_level = 1,
    };
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "[Personal Context]") == NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

void run_personal_model_tests(void) {
    HU_TEST_SUITE("PersonalModel");
    HU_RUN_TEST(personal_model_init_sets_defaults);
    HU_RUN_TEST(personal_model_ingest_extracts_facts);
    HU_RUN_TEST(personal_model_merge_facts_deduplicates);
    HU_RUN_TEST(personal_model_build_prompt_non_empty);
    HU_RUN_TEST(personal_model_query_preference_finds_match);
    HU_RUN_TEST(personal_model_ingest_updates_style_metrics);
    HU_RUN_TEST(personal_model_has_content_false_when_fresh);
    HU_RUN_TEST(personal_model_has_content_true_after_fact);
    HU_RUN_TEST(personal_model_has_content_true_after_style_observation);
    HU_RUN_TEST(personal_model_reaches_system_prompt_via_config);
    HU_RUN_TEST(personal_model_absent_does_not_leak_into_prompt);
}
