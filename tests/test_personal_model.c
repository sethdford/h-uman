#include "human/agent/prompt.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/fact_extract.h"
#include "human/memory/personal_model.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* M2 P1 — save/load round trip. The model is the only place we
 * accumulate user-specific signal across daemon restarts; without
 * persistence M2 is functionally still RAM-only. This test proves the
 * binary format survives a full round trip: ingest signals → save →
 * fresh struct → load → equality on the fields that carry value. */
static void personal_model_save_load_round_trips(void) {
    hu_personal_model_t a;
    hu_personal_model_init(&a);
    /* Seed signal: a couple of fact-shaped utterances + style observations. */
    hu_personal_model_ingest(&a, "i love climbing in the morning", 30, true,
                             1700000000LL);
    hu_personal_model_ingest(&a, "i never drink coffee", 21, true, 1700000100LL);
    hu_personal_model_ingest(&a, "lol that's cool 😎", 18, true, 1700000200LL);
    HU_ASSERT_TRUE(hu_personal_model_has_content(&a));

    /* Round-trip through a unique tmp path. */
    char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_pm_test_%d.bin", (int)getpid());
    HU_ASSERT_EQ(hu_personal_model_save(&a, path), HU_OK);

    hu_personal_model_t b;
    HU_ASSERT_EQ(hu_personal_model_load(&b, path), HU_OK);

    /* Key invariants: facts and interaction count survived. We don't
     * memcmp the whole struct because the build prompt's style metrics
     * are floats and a strict byte-equality would be brittle on platforms
     * with different float representations. Instead we assert the
     * meaningful fields. */
    HU_ASSERT_EQ(b.fact_count, a.fact_count);
    HU_ASSERT_EQ(b.interaction_count, a.interaction_count);
    HU_ASSERT_EQ(b.version, a.version);
    HU_ASSERT_TRUE(hu_personal_model_has_content(&b));

    /* Cleanup. */
    remove(path);
}

/* Bad magic must fail closed and leave `out` initialized to defaults so
 * the daemon can keep going without crashing. */
static void personal_model_load_rejects_bad_magic(void) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_pm_bad_%d.bin", (int)getpid());
    FILE *fp = fopen(path, "wb");
    HU_ASSERT_NOT_NULL(fp);
    const char garbage[64] = "not a personal model file at all";
    fwrite(garbage, sizeof(garbage), 1, fp);
    fclose(fp);

    hu_personal_model_t out;
    HU_ASSERT_EQ(hu_personal_model_load(&out, path), HU_ERR_PARSE);
    /* Initialized to defaults — version is the only field set by init. */
    HU_ASSERT_EQ(out.version, 1U);
    HU_ASSERT_EQ(out.fact_count, (size_t)0);
    HU_ASSERT_FALSE(hu_personal_model_has_content(&out));

    remove(path);
}

/* Missing file is a benign HU_ERR_NOT_FOUND. */
static void personal_model_load_missing_file_returns_not_found(void) {
    hu_personal_model_t out;
    HU_ASSERT_EQ(hu_personal_model_load(&out, "/tmp/no_such_pm_file_xyz_123.bin"),
                 HU_ERR_NOT_FOUND);
}

/* Resolution favours the env override when set. */
static void personal_model_resolve_default_path_honors_env_override(void) {
    char buf[128];
    char override[64];
    snprintf(override, sizeof(override), "/tmp/hu_pm_override_%d.bin", (int)getpid());
    setenv("HUMAN_PERSONAL_MODEL_PATH", override, 1);
    const char *got = hu_personal_model_resolve_default_path(buf, sizeof(buf));
    HU_ASSERT_NOT_NULL(got);
    HU_ASSERT_STR_EQ(got, override);
    unsetenv("HUMAN_PERSONAL_MODEL_PATH");
}

/* Without the override, the path lives under $HOME/.human/. */
static void personal_model_resolve_default_path_uses_home(void) {
    char tmp_home[64];
    snprintf(tmp_home, sizeof(tmp_home), "/tmp/hu_pm_home_%d", (int)getpid());
    unsetenv("HUMAN_PERSONAL_MODEL_PATH");
    setenv("HOME", tmp_home, 1);

    char buf[256];
    const char *got = hu_personal_model_resolve_default_path(buf, sizeof(buf));
    HU_ASSERT_NOT_NULL(got);
    char expected[256];
    snprintf(expected, sizeof(expected), "%s/.human/personal_model.bin", tmp_home);
    HU_ASSERT_STR_EQ(got, expected);
}

/* Without HOME or override, resolution returns NULL (no crash). */
static void personal_model_resolve_default_path_no_home_returns_null(void) {
    unsetenv("HUMAN_PERSONAL_MODEL_PATH");
    const char *prior_home = getenv("HOME");
    char saved_home[1024] = {0};
    if (prior_home) {
        size_t n = strlen(prior_home);
        if (n < sizeof(saved_home)) {
            memcpy(saved_home, prior_home, n + 1);
        }
    }
    unsetenv("HOME");
    char buf[64];
    HU_ASSERT_NULL(hu_personal_model_resolve_default_path(buf, sizeof(buf)));
    if (saved_home[0]) {
        setenv("HOME", saved_home, 1);
    }
}

/* Save creates the parent directory tree on first run so users don't have
 * to pre-create `~/.human/`. */
static void personal_model_save_creates_parent_directory(void) {
    char tmp_dir[128];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/hu_pm_mkdir_%d/nested/leaf", (int)getpid());
    char path[256];
    snprintf(path, sizeof(path), "%s/personal_model.bin", tmp_dir);

    /* Make sure the directory does NOT exist yet — fresh-state assertion. */
    char rm_cmd[512];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf /tmp/hu_pm_mkdir_%d 2>/dev/null", (int)getpid());
    (void)system(rm_cmd);

    hu_personal_model_t a;
    hu_personal_model_init(&a);
    hu_personal_model_ingest(&a, "i prefer dark mode", 18, true, 1700000300LL);
    HU_ASSERT_EQ(hu_personal_model_save(&a, path), HU_OK);

    hu_personal_model_t b;
    HU_ASSERT_EQ(hu_personal_model_load(&b, path), HU_OK);
    HU_ASSERT_TRUE(hu_personal_model_has_content(&b));

    (void)system(rm_cmd);
}

/* Round-trip via the resolver: save then load using the same default path
 * (driven by the env override) round-trips fact data. Documents the agent
 * lifecycle wiring contract. */
static void personal_model_default_path_round_trip(void) {
    char override[64];
    snprintf(override, sizeof(override), "/tmp/hu_pm_resolver_%d.bin", (int)getpid());
    setenv("HUMAN_PERSONAL_MODEL_PATH", override, 1);

    char path[256];
    HU_ASSERT_NOT_NULL(hu_personal_model_resolve_default_path(path, sizeof(path)));

    hu_personal_model_t a;
    hu_personal_model_init(&a);
    hu_personal_model_ingest(&a, "i never drink coffee", 21, true, 1700000400LL);
    HU_ASSERT_EQ(hu_personal_model_save(&a, path), HU_OK);

    char path2[256];
    HU_ASSERT_NOT_NULL(hu_personal_model_resolve_default_path(path2, sizeof(path2)));
    HU_ASSERT_STR_EQ(path, path2);
    hu_personal_model_t b;
    HU_ASSERT_EQ(hu_personal_model_load(&b, path2), HU_OK);
    HU_ASSERT_EQ(b.fact_count, a.fact_count);
    HU_ASSERT_EQ(b.interaction_count, a.interaction_count);

    remove(override);
    unsetenv("HUMAN_PERSONAL_MODEL_PATH");
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
    HU_RUN_TEST(personal_model_save_load_round_trips);
    HU_RUN_TEST(personal_model_load_rejects_bad_magic);
    HU_RUN_TEST(personal_model_load_missing_file_returns_not_found);
    HU_RUN_TEST(personal_model_resolve_default_path_honors_env_override);
    HU_RUN_TEST(personal_model_resolve_default_path_uses_home);
    HU_RUN_TEST(personal_model_resolve_default_path_no_home_returns_null);
    HU_RUN_TEST(personal_model_save_creates_parent_directory);
    HU_RUN_TEST(personal_model_default_path_round_trip);
}
