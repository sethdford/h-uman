#include "human/behavior/rel_dynamics.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static void rel_compute_velocity_all_positive(void) {
    hu_rel_signals_t s = {
        .frequency_delta = 0.5,
        .initiation_delta = 0.5,
        .response_time_delta = 0.5,
        .msg_length_delta = 0.5,
        .vulnerability_delta = 0.5,
        .plan_completion_rate = 0.5,
        .sentiment_delta = 0.5,
        .topic_diversity_delta = 0.5,
    };
    double v = hu_rel_compute_velocity(&s);
    HU_ASSERT_TRUE(v > 0.0);
}

static void rel_compute_velocity_all_negative(void) {
    hu_rel_signals_t s = {
        .frequency_delta = -0.5,
        .initiation_delta = -0.5,
        .response_time_delta = -0.5,
        .msg_length_delta = -0.5,
        .vulnerability_delta = -0.5,
        .plan_completion_rate = 0.0,
        .sentiment_delta = -0.5,
        .topic_diversity_delta = -0.5,
    };
    double v = hu_rel_compute_velocity(&s);
    HU_ASSERT_TRUE(v < 0.0);
}

static void rel_compute_velocity_mixed(void) {
    hu_rel_signals_t s = {
        .frequency_delta = 0.8,
        .initiation_delta = -0.5,
        .response_time_delta = 0.2,
        .msg_length_delta = -0.3,
        .vulnerability_delta = 0.6,
        .plan_completion_rate = 0.9,
        .sentiment_delta = -0.2,
        .topic_diversity_delta = 0.1,
    };
    double v = hu_rel_compute_velocity(&s);
    HU_ASSERT_TRUE(v > -1.0 && v < 1.0);
}

static void rel_compute_velocity_zeroes(void) {
    hu_rel_signals_t s = {0};
    double v = hu_rel_compute_velocity(&s);
    HU_ASSERT_FLOAT_EQ(v, 0.0, 0.001);
}

static void rel_classify_mode_deepening(void) {
    hu_rel_mode_t m = hu_rel_classify_mode(0.2, 0.0, HU_REL_NORMAL);
    HU_ASSERT_EQ(m, HU_REL_DEEPENING);
}

static void rel_classify_mode_drifting_consecutive(void) {
    hu_rel_mode_t m = hu_rel_classify_mode(-0.2, -0.15, HU_REL_NORMAL);
    HU_ASSERT_EQ(m, HU_REL_DRIFTING);
}

static void rel_classify_mode_single_negative_stays_normal(void) {
    hu_rel_mode_t m = hu_rel_classify_mode(-0.15, 0.1, HU_REL_NORMAL);
    HU_ASSERT_EQ(m, HU_REL_NORMAL);
}

static void rel_classify_mode_clear_drift(void) {
    hu_rel_mode_t m = hu_rel_classify_mode(-0.4, 0.5, HU_REL_NORMAL);
    HU_ASSERT_EQ(m, HU_REL_DRIFTING);
}

static void rel_classify_mode_reconnecting(void) {
    hu_rel_mode_t m = hu_rel_classify_mode(0.1, -0.2, HU_REL_DRIFTING);
    HU_ASSERT_EQ(m, HU_REL_RECONNECTING);
}

static void rel_classify_mode_repair_stays(void) {
    hu_rel_mode_t m = hu_rel_classify_mode(0.2, 0.1, HU_REL_REPAIR);
    HU_ASSERT_EQ(m, HU_REL_REPAIR);
}

static void rel_compute_state_updates_closeness(void) {
    hu_rel_signals_t s = {
        .frequency_delta = 0.5,
        .initiation_delta = 0.5,
        .response_time_delta = 0.5,
        .msg_length_delta = 0.5,
        .vulnerability_delta = 0.5,
        .plan_completion_rate = 0.5,
        .sentiment_delta = 0.5,
        .topic_diversity_delta = 0.5,
    };
    hu_rel_state_t out;
    memset(&out, 0, sizeof(out));
    hu_error_t err = hu_rel_compute_state(&s, 0.5, HU_REL_NORMAL, 0.0, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out.closeness > 0.5);
}

static void rel_compute_state_clamps_closeness(void) {
    hu_rel_signals_t s = {
        .frequency_delta = 1.0,
        .initiation_delta = 1.0,
        .response_time_delta = 1.0,
        .msg_length_delta = 1.0,
        .vulnerability_delta = 1.0,
        .plan_completion_rate = 1.0,
        .sentiment_delta = 1.0,
        .topic_diversity_delta = 1.0,
    };
    hu_rel_state_t out;
    memset(&out, 0, sizeof(out));
    hu_error_t err = hu_rel_compute_state(&s, 0.95, HU_REL_NORMAL, 0.0, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out.closeness <= 1.0);
}

static void rel_budget_multiplier_normal(void) {
    hu_rel_state_t state = {.mode = HU_REL_NORMAL};
    hu_rel_config_t config = {0};
    double m = hu_rel_budget_multiplier(&state, &config);
    HU_ASSERT_FLOAT_EQ(m, 1.0, 0.001);
}

static void rel_budget_multiplier_drifting(void) {
    hu_rel_state_t state = {.mode = HU_REL_DRIFTING};
    hu_rel_config_t config = {.drift_budget_multiplier = 0.5};
    double m = hu_rel_budget_multiplier(&state, &config);
    HU_ASSERT_FLOAT_EQ(m, 0.5, 0.001);
}

static void rel_budget_multiplier_repair(void) {
    hu_rel_state_t state = {.mode = HU_REL_REPAIR};
    hu_rel_config_t config = {0};
    double m = hu_rel_budget_multiplier(&state, &config);
    HU_ASSERT_FLOAT_EQ(m, 0.3, 0.001);
}

static void rel_should_exit_repair_true(void) {
    hu_rel_state_t state = {
        .mode = HU_REL_REPAIR,
        .velocity = 0.1,
        .mode_entered_at = 0,
    };
    hu_rel_config_t config = {.repair_exit_days = 3};
    uint64_t now_ms = 4ULL * 86400000ULL;
    HU_ASSERT_TRUE(hu_rel_should_exit_repair(&state, &config, now_ms));
}

static void rel_should_exit_repair_false_too_early(void) {
    hu_rel_state_t state = {
        .mode = HU_REL_REPAIR,
        .velocity = 0.1,
        .mode_entered_at = 86400000ULL,
    };
    hu_rel_config_t config = {.repair_exit_days = 3};
    uint64_t now_ms = 86400000ULL + 8640000ULL;
    HU_ASSERT_FALSE(hu_rel_should_exit_repair(&state, &config, now_ms));
}

static void rel_create_table_sql_valid(void) {
    char buf[1024];
    size_t len = 0;
    hu_error_t err = hu_rel_create_table_sql(buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "CREATE TABLE"));
    HU_ASSERT_NOT_NULL(strstr(buf, "relationship_state"));
    HU_ASSERT_NOT_NULL(strstr(buf, "contact_id"));
    HU_ASSERT_NOT_NULL(strstr(buf, "closeness"));
    HU_ASSERT_NOT_NULL(strstr(buf, "velocity"));
    HU_ASSERT_NOT_NULL(strstr(buf, "mode"));
}

static void rel_mode_str_roundtrip(void) {
    hu_rel_mode_t modes[] = {
        HU_REL_NORMAL, HU_REL_DEEPENING, HU_REL_DRIFTING, HU_REL_REPAIR, HU_REL_RECONNECTING,
    };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        const char *str = hu_rel_mode_str(modes[i]);
        HU_ASSERT_NOT_NULL(str);
        hu_rel_mode_t out;
        HU_ASSERT_TRUE(hu_rel_mode_from_str(str, &out));
        HU_ASSERT_EQ(out, modes[i]);
    }
}

static void rel_build_prompt_contains_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rel_state_t state = {
        .contact_id = NULL,
        .contact_id_len = 0,
        .closeness = 0.5,
        .velocity = -0.2,
        .vulnerability_depth = 0.4,
        .reciprocity = -0.1,
        .last_interaction = 0,
        .last_vulnerability_moment = 0,
        .mode = HU_REL_DRIFTING,
        .mode_entered_at = 0,
        .measured_at = 0,
    };
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_rel_build_prompt(&alloc, &state, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_NOT_NULL(strstr(out, "drifting"));
    hu_str_free(&alloc, out);
}

static void rel_state_deinit_frees_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rel_state_t state;
    memset(&state, 0, sizeof(state));
    state.contact_id = hu_strndup(&alloc, "test_contact", 11);
    state.contact_id_len = 11;
    HU_ASSERT_NOT_NULL(state.contact_id);
    hu_rel_state_deinit(&alloc, &state);
    HU_ASSERT_NULL(state.contact_id);
    HU_ASSERT_EQ(state.contact_id_len, 0);
}

void run_rel_dynamics_tests(void) {
    HU_TEST_SUITE("rel_dynamics");
    HU_RUN_TEST(rel_compute_velocity_all_positive);
    HU_RUN_TEST(rel_compute_velocity_all_negative);
    HU_RUN_TEST(rel_compute_velocity_mixed);
    HU_RUN_TEST(rel_compute_velocity_zeroes);
    HU_RUN_TEST(rel_classify_mode_deepening);
    HU_RUN_TEST(rel_classify_mode_drifting_consecutive);
    HU_RUN_TEST(rel_classify_mode_single_negative_stays_normal);
    HU_RUN_TEST(rel_classify_mode_clear_drift);
    HU_RUN_TEST(rel_classify_mode_reconnecting);
    HU_RUN_TEST(rel_classify_mode_repair_stays);
    HU_RUN_TEST(rel_compute_state_updates_closeness);
    HU_RUN_TEST(rel_compute_state_clamps_closeness);
    HU_RUN_TEST(rel_budget_multiplier_normal);
    HU_RUN_TEST(rel_budget_multiplier_drifting);
    HU_RUN_TEST(rel_budget_multiplier_repair);
    HU_RUN_TEST(rel_should_exit_repair_true);
    HU_RUN_TEST(rel_should_exit_repair_false_too_early);
    HU_RUN_TEST(rel_create_table_sql_valid);
    HU_RUN_TEST(rel_mode_str_roundtrip);
    HU_RUN_TEST(rel_build_prompt_contains_mode);
    HU_RUN_TEST(rel_state_deinit_frees_memory);
}
