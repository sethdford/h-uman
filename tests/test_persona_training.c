#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/persona/training.h"
#include "test_framework.h"
#include <math.h>
#include <string.h>

/* --- Style Drift (F163) --- */

static void style_drift_score_identical_returns_zero(void) {
    hu_style_snapshot_t base = {50.0, 0.1, 0.05, 0.2, 0.3, 1000};
    hu_style_snapshot_t cur = {50.0, 0.1, 0.05, 0.2, 0.3, 2000};

    double score = hu_style_drift_score(&base, &cur);
    HU_ASSERT_FLOAT_EQ(score, 0.0, 0.001);
}

static void style_drift_score_different_returns_nonzero(void) {
    hu_style_snapshot_t base = {50.0, 0.1, 0.05, 0.2, 0.3, 1000};
    hu_style_snapshot_t cur = {100.0, 0.5, 0.2, 0.5, 0.8, 2000};

    double score = hu_style_drift_score(&base, &cur);
    HU_ASSERT_TRUE(score > 0.0);
    HU_ASSERT_TRUE(score <= 1.0);
}

static void style_drift_score_null_returns_zero(void) {
    hu_style_snapshot_t base = {50.0, 0.1, 0.05, 0.2, 0.3, 1000};
    HU_ASSERT_FLOAT_EQ(hu_style_drift_score(&base, NULL), 0.0, 0.001);
    HU_ASSERT_FLOAT_EQ(hu_style_drift_score(NULL, &base), 0.0, 0.001);
}

static void style_drift_significant_above_threshold(void) {
    HU_ASSERT_TRUE(hu_style_drift_significant(0.5, 0.3));
    HU_ASSERT_TRUE(hu_style_drift_significant(0.31, 0.3));
}

static void style_drift_significant_below_threshold(void) {
    HU_ASSERT_FALSE(hu_style_drift_significant(0.2, 0.3));
    HU_ASSERT_FALSE(hu_style_drift_significant(0.3, 0.3));
}

static void style_drift_create_table_sql_produces_valid_sql(void) {
    char buf[1024];
    size_t out_len = 0;

    HU_ASSERT_EQ(hu_style_drift_create_table_sql(buf, sizeof(buf), &out_len), HU_OK);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(buf, "style_snapshots") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "avg_msg_length") != NULL);
}

static void style_drift_insert_sql_produces_valid_sql(void) {
    hu_style_snapshot_t snap = {42.5, 0.15, 0.08, 0.25, 0.4, 12345};
    char buf[512];
    size_t out_len = 0;

    HU_ASSERT_EQ(hu_style_drift_insert_sql(&snap, buf, sizeof(buf), &out_len), HU_OK);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(buf, "INSERT INTO style_snapshots") != NULL);
}

/* --- Behavioral Cloning (F165-F166) --- */

static void clone_calibrate_computes_delta(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *names[] = {"formality", "emoji_freq", "msg_length"};
    size_t lens[] = {9, 10, 10};
    double targets[] = {0.8, 0.2, 100.0};
    double currents[] = {0.6, 0.35, 80.0};
    hu_clone_calibration_t out[3];
    memset(out, 0, sizeof(out));

    HU_ASSERT_EQ(hu_clone_calibrate(&alloc, names, lens, targets, currents, 3, out),
                 HU_OK);
    HU_ASSERT_FLOAT_EQ(out[0].delta, 0.2, 0.001);
    HU_ASSERT_FLOAT_EQ(out[1].delta, -0.15, 0.001);
    HU_ASSERT_FLOAT_EQ(out[2].delta, 20.0, 0.001);

    for (size_t i = 0; i < 3; i++)
        hu_clone_calibration_deinit(&alloc, &out[i]);
}

static void clone_calibrate_with_default_names(void) {
    hu_allocator_t alloc = hu_system_allocator();
    double targets[] = {0.5};
    double currents[] = {0.3};
    hu_clone_calibration_t out[1];
    memset(out, 0, sizeof(out));

    HU_ASSERT_EQ(hu_clone_calibrate(&alloc, NULL, NULL, targets, currents, 1, out),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out[0].parameter);
    HU_ASSERT_TRUE(strstr(out[0].parameter, "formality") != NULL);

    hu_clone_calibration_deinit(&alloc, &out[0]);
}

static void clone_build_adjustment_prompt_format(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_clone_calibration_t cals[2];
    memset(cals, 0, sizeof(cals));
    cals[0].parameter = hu_strndup(&alloc, "formality", 9);
    cals[0].parameter_len = 9;
    cals[0].delta = 0.2;
    cals[1].parameter = hu_strndup(&alloc, "emoji", 5);
    cals[1].parameter_len = 5;
    cals[1].delta = -0.15;

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_clone_build_adjustment_prompt(&alloc, cals, 2, &out, &out_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(out, "[CALIBRATION NEEDED]") != NULL);
    HU_ASSERT_TRUE(strstr(out, "formality") != NULL);
    HU_ASSERT_TRUE(strstr(out, "too low") != NULL);
    HU_ASSERT_TRUE(strstr(out, "emoji") != NULL);
    HU_ASSERT_TRUE(strstr(out, "too high") != NULL);

    hu_str_free(&alloc, out);
    hu_clone_calibration_deinit(&alloc, &cals[0]);
    hu_clone_calibration_deinit(&alloc, &cals[1]);
}

static void clone_calibration_deinit_frees_parameter(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_clone_calibration_t cal;
    memset(&cal, 0, sizeof(cal));
    cal.parameter = hu_strndup(&alloc, "formality", 9);
    cal.parameter_len = 9;

    hu_clone_calibration_deinit(&alloc, &cal);
    HU_ASSERT_NULL(cal.parameter);
    HU_ASSERT_EQ(cal.parameter_len, 0u);
}

/* --- Persona LoRA (F144-F146) --- */

static void lora_default_config_has_expected_values(void) {
    hu_lora_config_t c = hu_lora_default_config();

    HU_ASSERT_EQ(c.min_training_messages, 500u);
    HU_ASSERT_EQ(c.epochs, 3u);
    HU_ASSERT_EQ(c.rank, 16u);
    HU_ASSERT_TRUE(c.learning_rate > 0.0);
}

static void lora_check_readiness_not_started(void) {
    HU_ASSERT_EQ(hu_lora_check_readiness(0, 500), HU_LORA_NOT_STARTED);
}

static void lora_check_readiness_collecting(void) {
    HU_ASSERT_EQ(hu_lora_check_readiness(250, 500), HU_LORA_COLLECTING);
    HU_ASSERT_EQ(hu_lora_check_readiness(499, 500), HU_LORA_COLLECTING);
}

static void lora_check_readiness_ready(void) {
    HU_ASSERT_EQ(hu_lora_check_readiness(500, 500), HU_LORA_READY);
    HU_ASSERT_EQ(hu_lora_check_readiness(600, 500), HU_LORA_READY);
}

static void lora_create_table_sql_produces_valid_sql(void) {
    char buf[1024];
    size_t out_len = 0;

    HU_ASSERT_EQ(hu_lora_create_table_sql(buf, sizeof(buf), &out_len), HU_OK);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(buf, "lora_training_samples") != NULL);
}

static void lora_insert_training_sample_sql_produces_valid_sql(void) {
    char buf[1024];
    size_t out_len = 0;
    const char *msg = "hello there";
    const char *ctx = "prior context";

    HU_ASSERT_EQ(hu_lora_insert_training_sample_sql(msg, 11, ctx, 12, 99999, buf,
                                                    sizeof(buf), &out_len),
                 HU_OK);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(buf, "INSERT INTO lora_training_samples") != NULL);
}

static void lora_insert_escapes_quotes(void) {
    char buf[1024];
    size_t out_len = 0;
    const char *msg = "O'Brien said hi";

    HU_ASSERT_EQ(hu_lora_insert_training_sample_sql(msg, 15, NULL, 0, 1, buf,
                                                    sizeof(buf), &out_len),
                 HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "''") != NULL);
}

static void lora_query_status_sql_produces_valid_sql(void) {
    char buf[512];
    size_t out_len = 0;

    HU_ASSERT_EQ(hu_lora_query_status_sql(buf, sizeof(buf), &out_len), HU_OK);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(buf, "lora_training_samples") != NULL);
}

static void lora_build_prompt_collecting_shows_progress(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_lora_state_t state = {.status = HU_LORA_COLLECTING,
                             .messages_collected = 250,
                             .messages_needed = 500};

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_lora_build_prompt(&alloc, &state, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "250") != NULL);
    HU_ASSERT_TRUE(strstr(out, "500") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Collecting") != NULL);

    hu_str_free(&alloc, out);
}

static void lora_status_str_returns_non_empty(void) {
    HU_ASSERT_NOT_NULL(hu_lora_status_str(HU_LORA_NOT_STARTED));
    HU_ASSERT_TRUE(strlen(hu_lora_status_str(HU_LORA_NOT_STARTED)) > 0);
    HU_ASSERT_STR_EQ(hu_lora_status_str(HU_LORA_COLLECTING), "collecting");
}

static void lora_state_deinit_frees_model_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_lora_state_t state;
    memset(&state, 0, sizeof(state));
    state.model_path = hu_strndup(&alloc, "/path/to/model", 14);
    state.model_path_len = 14;

    hu_lora_state_deinit(&alloc, &state);
    HU_ASSERT_NULL(state.model_path);
}

static void lora_config_deinit_frees_base_model(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_lora_config_t c;
    memset(&c, 0, sizeof(c));
    c.base_model = hu_strndup(&alloc, "llama-3", 7);
    c.base_model_len = 7;

    hu_lora_config_deinit(&alloc, &c);
    HU_ASSERT_NULL(c.base_model);
}

void run_persona_training_tests(void) {
    HU_TEST_SUITE("persona_training");
    HU_RUN_TEST(style_drift_score_identical_returns_zero);
    HU_RUN_TEST(style_drift_score_different_returns_nonzero);
    HU_RUN_TEST(style_drift_score_null_returns_zero);
    HU_RUN_TEST(style_drift_significant_above_threshold);
    HU_RUN_TEST(style_drift_significant_below_threshold);
    HU_RUN_TEST(style_drift_create_table_sql_produces_valid_sql);
    HU_RUN_TEST(style_drift_insert_sql_produces_valid_sql);
    HU_RUN_TEST(clone_calibrate_computes_delta);
    HU_RUN_TEST(clone_calibrate_with_default_names);
    HU_RUN_TEST(clone_build_adjustment_prompt_format);
    HU_RUN_TEST(clone_calibration_deinit_frees_parameter);
    HU_RUN_TEST(lora_default_config_has_expected_values);
    HU_RUN_TEST(lora_check_readiness_not_started);
    HU_RUN_TEST(lora_check_readiness_collecting);
    HU_RUN_TEST(lora_check_readiness_ready);
    HU_RUN_TEST(lora_create_table_sql_produces_valid_sql);
    HU_RUN_TEST(lora_insert_training_sample_sql_produces_valid_sql);
    HU_RUN_TEST(lora_insert_escapes_quotes);
    HU_RUN_TEST(lora_query_status_sql_produces_valid_sql);
    HU_RUN_TEST(lora_build_prompt_collecting_shows_progress);
    HU_RUN_TEST(lora_status_str_returns_non_empty);
    HU_RUN_TEST(lora_state_deinit_frees_model_path);
    HU_RUN_TEST(lora_config_deinit_frees_base_model);
}
