/* tests/test_init_proposer.c — covers AC-1 / AC-6 / AC-7 of T1.
 *
 * The init_proposer tick function is a pure predicate over (config, ar_cfg,
 * budget, recency, now). All four T1 scenarios are exercised without any
 * real network or daemon spin-up. Per
 * .claude/rules/security-predicate-extraction.md — the tick is structured
 * as a pure decision so the truth table can be locked here. */

#include "human/agent/governor.h"
#include "human/agent/init_proposer.h"
#include "human/autoresponder.h"
#include "human/config.h"
#include "test_framework.h"
#include <string.h>

/* T1 default config: disabled (AC-7 kill switch is off by default). */
static void make_default_cfg(hu_initiative_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = false;
    cfg->tick_interval_sec = 1800;
    cfg->confidence_threshold = 0.85;
    cfg->per_contact_min_seconds = 600;
    /* propose_model intentionally left NULL — tick handles that. */
}

static void make_enabled_cfg(hu_initiative_config_t *cfg) {
    make_default_cfg(cfg);
    cfg->enabled = true;
}

static void test_disabled_config_returns_skip_no_state_change(void) {
    hu_init_proposer_reset_warn_guards_for_test();
    hu_initiative_config_t cfg;
    make_default_cfg(&cfg);
    int64_t last_tick = 0;
    uint64_t tick_id = 0;
    hu_init_proposer_result_t result = HU_INIT_RESULT_FIRED; /* sentinel */
    HU_ASSERT_EQ(
        hu_init_proposer_tick(&cfg, NULL, 0, NULL, 0, 1779700000, &last_tick, &tick_id, &result),
        HU_OK);
    HU_ASSERT_EQ((int)result, (int)HU_INIT_RESULT_SKIP);
    /* Disabled path MUST NOT advance the watermark — operators rely on
     * last_tick being stale to detect a flipped-off subsystem. */
    HU_ASSERT_EQ(last_tick, (int64_t)0);
    HU_ASSERT_EQ(tick_id, (uint64_t)0);
}

static void test_enabled_all_clear_returns_skip_advances_state(void) {
    hu_init_proposer_reset_warn_guards_for_test();
    hu_initiative_config_t cfg;
    make_enabled_cfg(&cfg);
    int64_t last_tick = 0;
    uint64_t tick_id = 0;
    hu_init_proposer_result_t result = HU_INIT_RESULT_FIRED;
    int64_t now = 1779700000;
    HU_ASSERT_EQ(hu_init_proposer_tick(&cfg, NULL, 0, NULL, 0, now, &last_tick, &tick_id, &result),
                 HU_OK);
    HU_ASSERT_EQ((int)result, (int)HU_INIT_RESULT_SKIP);
    HU_ASSERT_EQ(last_tick, now);
    HU_ASSERT_EQ(tick_id, (uint64_t)1);
}

static void test_interval_gate_blocks_back_to_back_ticks(void) {
    hu_init_proposer_reset_warn_guards_for_test();
    hu_initiative_config_t cfg;
    make_enabled_cfg(&cfg);
    int64_t last_tick = 1779700000;
    uint64_t tick_id = 5;
    hu_init_proposer_result_t result = HU_INIT_RESULT_FIRED;
    /* 60s later — well inside the 1800s interval. */
    HU_ASSERT_EQ(
        hu_init_proposer_tick(&cfg, NULL, 0, NULL, 0, 1779700060, &last_tick, &tick_id, &result),
        HU_OK);
    HU_ASSERT_EQ((int)result, (int)HU_INIT_RESULT_GATED_INTERVAL);
    /* tick_id MUST NOT advance when interval-gated — otherwise every poll
     * inflates the counter and SKIP-rate metrics become meaningless. */
    HU_ASSERT_EQ(tick_id, (uint64_t)5);
    HU_ASSERT_EQ(last_tick, (int64_t)1779700000);
}

static void test_per_contact_recency_gates_when_seth_texted_recently(void) {
    hu_init_proposer_reset_warn_guards_for_test();
    hu_initiative_config_t cfg;
    make_enabled_cfg(&cfg);
    int64_t now = 1779700000;
    int64_t last_inbound = now - 60; /* Seth texted 60s ago, floor is 600s */
    int64_t last_tick = 0;
    uint64_t tick_id = 0;
    hu_init_proposer_result_t result = HU_INIT_RESULT_FIRED;
    HU_ASSERT_EQ(hu_init_proposer_tick(&cfg, NULL, 0, NULL, last_inbound, now, &last_tick, &tick_id,
                                       &result),
                 HU_OK);
    HU_ASSERT_EQ((int)result, (int)HU_INIT_RESULT_GATED_RECENCY);
    /* Recency-gated ticks DO advance the watermark — otherwise a
     * fast-talking Seth would starve the proposer indefinitely. */
    HU_ASSERT_EQ(last_tick, now);
    HU_ASSERT_EQ(tick_id, (uint64_t)1);
}

static void test_null_args_return_invalid(void) {
    hu_initiative_config_t cfg;
    make_enabled_cfg(&cfg);
    int64_t last_tick = 0;
    uint64_t tick_id = 0;
    hu_init_proposer_result_t result = HU_INIT_RESULT_FIRED;
    /* cfg NULL */
    HU_ASSERT_EQ(hu_init_proposer_tick(NULL, NULL, 0, NULL, 0, 0, &last_tick, &tick_id, &result),
                 HU_ERR_INVALID_ARGUMENT);
    /* watermark NULL */
    HU_ASSERT_EQ(hu_init_proposer_tick(&cfg, NULL, 0, NULL, 0, 0, NULL, &tick_id, &result),
                 HU_ERR_INVALID_ARGUMENT);
    /* tick_id NULL */
    HU_ASSERT_EQ(hu_init_proposer_tick(&cfg, NULL, 0, NULL, 0, 0, &last_tick, NULL, &result),
                 HU_ERR_INVALID_ARGUMENT);
    /* out_result NULL */
    HU_ASSERT_EQ(hu_init_proposer_tick(&cfg, NULL, 0, NULL, 0, 0, &last_tick, &tick_id, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

void run_init_proposer_tests(void);
void run_init_proposer_tests(void) {
    HU_TEST_SUITE("init_proposer");
    HU_RUN_TEST(test_disabled_config_returns_skip_no_state_change);
    HU_RUN_TEST(test_enabled_all_clear_returns_skip_advances_state);
    HU_RUN_TEST(test_interval_gate_blocks_back_to_back_ticks);
    HU_RUN_TEST(test_per_contact_recency_gates_when_seth_texted_recently);
    HU_RUN_TEST(test_null_args_return_invalid);
}
