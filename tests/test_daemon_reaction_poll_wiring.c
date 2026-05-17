/* tests/test_daemon_reaction_poll_wiring.c
 *
 * Pins the production-callable iMessage reaction poll tick that the
 * daemon's main loop invokes once every ~30s (CF-3 closure).
 *
 * The original two tests (test_daemon_*_tick_for_test) exercise the
 * legacy `_for_test` entry that strictly returns INVALID_ARGUMENT on
 * NULL cfg. The new tests exercise `hu_daemon_reaction_poll_tick`, the
 * production entry the daemon actually calls, which treats NULL cfg
 * as feature-disabled (no-op).
 */

#include "test_framework.h"
#include "human/agent/reaction_handler.h"
#include "human/daemon_reaction_poll.h"
#include "human/config.h"

static void test_daemon_does_not_call_poll_when_feature_flag_off(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.reaction_collection.enabled = false;
    int poll_call_count = 0;
    hu_daemon_set_poll_call_counter_for_test(&poll_call_count);
    HU_ASSERT_EQ(hu_daemon_tick_for_test(&cfg), HU_OK);
    HU_ASSERT_EQ(poll_call_count, 0);
    hu_daemon_set_poll_call_counter_for_test(NULL);
}

static void test_daemon_calls_poll_when_feature_flag_on(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.reaction_collection.enabled = true;
    snprintf(cfg.reaction_collection.channels[0],
             sizeof(cfg.reaction_collection.channels[0]), "imessage");
    cfg.reaction_collection.channel_count = 1;
    setenv("HU_CHATDB", "/tmp/nonexistent-chatdb.sqlite", 1);
    int poll_call_count = 0;
    hu_daemon_set_poll_call_counter_for_test(&poll_call_count);
    HU_ASSERT_EQ(hu_daemon_tick_for_test(&cfg), HU_OK);
    HU_ASSERT_TRUE(poll_call_count >= 1);
    hu_daemon_set_poll_call_counter_for_test(NULL);
}

/* CF-3 wiring tests against the production entry. */

static void test_production_tick_treats_null_cfg_as_noop(void) {
    int poll_call_count = 0;
    hu_daemon_set_poll_call_counter_for_test(&poll_call_count);
    size_t ingested = 999;
    HU_ASSERT_EQ(hu_daemon_reaction_poll_tick(NULL, 0, &ingested), HU_OK);
    HU_ASSERT_EQ(ingested, 0u);
    HU_ASSERT_EQ(poll_call_count, 0);
    hu_daemon_set_poll_call_counter_for_test(NULL);
}

static void test_production_tick_treats_disabled_feature_as_noop(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.reaction_collection.enabled = false;
    int poll_call_count = 0;
    hu_daemon_set_poll_call_counter_for_test(&poll_call_count);
    HU_ASSERT_EQ(hu_daemon_reaction_poll_tick(&cfg, 0, NULL), HU_OK);
    HU_ASSERT_EQ(poll_call_count, 0);
    hu_daemon_set_poll_call_counter_for_test(NULL);
}

static void test_production_tick_treats_missing_env_as_noop(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.reaction_collection.enabled = true;
    snprintf(cfg.reaction_collection.channels[0],
             sizeof(cfg.reaction_collection.channels[0]), "imessage");
    cfg.reaction_collection.channel_count = 1;
    unsetenv("HU_CHATDB");
    int poll_call_count = 0;
    hu_daemon_set_poll_call_counter_for_test(&poll_call_count);
    size_t ingested = 999;
    HU_ASSERT_EQ(hu_daemon_reaction_poll_tick(&cfg, 0, &ingested), HU_OK);
    HU_ASSERT_EQ(ingested, 0u);
    HU_ASSERT_EQ(poll_call_count, 0);
    hu_daemon_set_poll_call_counter_for_test(NULL);
}

static void test_production_tick_calls_poll_when_enabled(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.reaction_collection.enabled = true;
    snprintf(cfg.reaction_collection.channels[0],
             sizeof(cfg.reaction_collection.channels[0]), "imessage");
    cfg.reaction_collection.channel_count = 1;
    setenv("HU_CHATDB", "/tmp/cf3-nonexistent-chatdb.sqlite", 1);
    int poll_call_count = 0;
    hu_daemon_set_poll_call_counter_for_test(&poll_call_count);
    size_t ingested = 999;
    /* Under HU_IS_TEST, hu_imessage_poll_reactions returns
     * HU_ERR_NOT_SUPPORTED — the production tick treats that as
     * "nothing to poll" (HU_OK + 0 ingested), so the daemon's main
     * loop never reports the test-only stub as a fatal error. */
    HU_ASSERT_EQ(hu_daemon_reaction_poll_tick(&cfg, 0, &ingested), HU_OK);
    HU_ASSERT_TRUE(poll_call_count >= 1);
    HU_ASSERT_EQ(ingested, 0u);
    hu_daemon_set_poll_call_counter_for_test(NULL);
    unsetenv("HU_CHATDB");
}

static void test_production_tick_accepts_channel_count_zero_as_imessage(void) {
    /* channel_count==0 means "all default channels" (matches the
     * existing reaction_collection_wants_imessage helper). */
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.reaction_collection.enabled = true;
    cfg.reaction_collection.channel_count = 0;
    setenv("HU_CHATDB", "/tmp/cf3-nonexistent-chatdb.sqlite", 1);
    int poll_call_count = 0;
    hu_daemon_set_poll_call_counter_for_test(&poll_call_count);
    HU_ASSERT_EQ(hu_daemon_reaction_poll_tick(&cfg, 0, NULL), HU_OK);
    HU_ASSERT_TRUE(poll_call_count >= 1);
    hu_daemon_set_poll_call_counter_for_test(NULL);
    unsetenv("HU_CHATDB");
}

static void test_production_tick_skips_when_imessage_not_in_channel_list(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.reaction_collection.enabled = true;
    snprintf(cfg.reaction_collection.channels[0],
             sizeof(cfg.reaction_collection.channels[0]), "slack");
    cfg.reaction_collection.channel_count = 1;
    setenv("HU_CHATDB", "/tmp/cf3-nonexistent-chatdb.sqlite", 1);
    int poll_call_count = 0;
    hu_daemon_set_poll_call_counter_for_test(&poll_call_count);
    HU_ASSERT_EQ(hu_daemon_reaction_poll_tick(&cfg, 0, NULL), HU_OK);
    HU_ASSERT_EQ(poll_call_count, 0);
    hu_daemon_set_poll_call_counter_for_test(NULL);
    unsetenv("HU_CHATDB");
}

void run_daemon_reaction_poll_tests(void) {
    HU_TEST_SUITE("daemon-reaction-poll");
    HU_RUN_TEST(test_daemon_does_not_call_poll_when_feature_flag_off);
    HU_RUN_TEST(test_daemon_calls_poll_when_feature_flag_on);
    HU_RUN_TEST(test_production_tick_treats_null_cfg_as_noop);
    HU_RUN_TEST(test_production_tick_treats_disabled_feature_as_noop);
    HU_RUN_TEST(test_production_tick_treats_missing_env_as_noop);
    HU_RUN_TEST(test_production_tick_calls_poll_when_enabled);
    HU_RUN_TEST(test_production_tick_accepts_channel_count_zero_as_imessage);
    HU_RUN_TEST(test_production_tick_skips_when_imessage_not_in_channel_list);
}
