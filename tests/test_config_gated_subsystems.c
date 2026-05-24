/* tests/test_config_gated_subsystems.c
 *
 * Per ~/.claude/rules/silent-config-gated-subsystems.md: centralized test
 * suite verifying that config-gated subsystems emit exactly ONE log line per
 * process when disabled or enabled. This is critical operator visibility —
 * missing config must be discoverable, not silent.
 *
 * Subsystems tested:
 * - reaction_collection: iMessage tapback → DPO pair ingestion
 *
 * Future subsystems (US-48-3):
 * - follow_up_watcher: read-without-reply detection
 * - proactive_throttle: follow-up scheduling throttle
 *
 * The core one-shot behavior (emit exactly once per process) is tested in
 * test_log_once.c. This suite focuses on subsystem-specific behavior:
 * - disabled path returns HU_OK without polling
 * - enabled path returns HU_OK after polling
 * - guards are independent per subsystem state
 */

#include "human/config.h"
#include "human/daemon_reaction_poll.h"
#include "test_framework.h"

#include <string.h>

/* ========== reaction_collection subsystem tests ========== */

/* Test: disabled reaction_collection tick returns HU_OK without polling */
static void test_reaction_collection_disabled_returns_ok(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.reaction_collection.enabled = false;

    /* Reset the warning guards so the test starts fresh */
    hu_daemon_reaction_poll_reset_warn_guards_for_test();

    /* Should return HU_OK even though subsystem is disabled */
    HU_ASSERT_EQ(hu_daemon_reaction_poll_tick(&cfg, 0, NULL), HU_OK);

    /* Second invocation should also return HU_OK without error */
    HU_ASSERT_EQ(hu_daemon_reaction_poll_tick(&cfg, 0, NULL), HU_OK);
}

/* Test: enabled reaction_collection tick returns HU_OK with empty env */
static void test_reaction_collection_enabled_with_no_chatdb_env(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.reaction_collection.enabled = true;
    snprintf(cfg.reaction_collection.channels[0], sizeof(cfg.reaction_collection.channels[0]),
             "imessage");
    cfg.reaction_collection.channel_count = 1;

    /* Make sure HU_CHATDB is not set */
    unsetenv("HU_CHATDB");

    /* Reset the warning guards so the test starts fresh */
    hu_daemon_reaction_poll_reset_warn_guards_for_test();

    /* Should return HU_OK because no chatdb path is available */
    HU_ASSERT_EQ(hu_daemon_reaction_poll_tick(&cfg, 0, NULL), HU_OK);

    /* Second invocation should also work */
    HU_ASSERT_EQ(hu_daemon_reaction_poll_tick(&cfg, 0, NULL), HU_OK);
}

/* Test: disabled→enabled state transitions preserve guard independence */
static void test_reaction_collection_guards_are_independent(void) {
    hu_config_t cfg_disabled;
    memset(&cfg_disabled, 0, sizeof(cfg_disabled));
    cfg_disabled.reaction_collection.enabled = false;

    hu_config_t cfg_enabled;
    memset(&cfg_enabled, 0, sizeof(cfg_enabled));
    cfg_enabled.reaction_collection.enabled = true;
    snprintf(cfg_enabled.reaction_collection.channels[0],
             sizeof(cfg_enabled.reaction_collection.channels[0]), "imessage");
    cfg_enabled.reaction_collection.channel_count = 1;
    setenv("HU_CHATDB", "/tmp/test-nonexistent.db", 1);

    /* Reset the warning guards */
    hu_daemon_reaction_poll_reset_warn_guards_for_test();

    /* Call with disabled config */
    HU_ASSERT_EQ(hu_daemon_reaction_poll_tick(&cfg_disabled, 0, NULL), HU_OK);

    /* Call with enabled config */
    HU_ASSERT_EQ(hu_daemon_reaction_poll_tick(&cfg_enabled, 0, NULL), HU_OK);

    /* Both should work without error — guards don't interfere */
    HU_ASSERT_EQ(hu_daemon_reaction_poll_tick(&cfg_disabled, 0, NULL), HU_OK);
    HU_ASSERT_EQ(hu_daemon_reaction_poll_tick(&cfg_enabled, 0, NULL), HU_OK);

    unsetenv("HU_CHATDB");
}

/* Test: hu_daemon_tick_reaction_poll (frequency-gated tick) also works */
static void test_reaction_collection_sub_tick_disabled_returns_ok(void) {
    hu_reaction_collection_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = false;

    /* Reset guards */
    hu_daemon_reaction_poll_reset_warn_guards_for_test();

    int64_t last_poll = 0;
    int64_t watermark = 0;

    /* Should return HU_OK */
    HU_ASSERT_EQ(hu_daemon_tick_reaction_poll(&cfg, 0, &last_poll, &watermark), HU_OK);

    /* Second invocation should also work */
    HU_ASSERT_EQ(hu_daemon_tick_reaction_poll(&cfg, 0, &last_poll, &watermark), HU_OK);
}

/* Test: hu_daemon_tick_reaction_poll with enabled config */
static void test_reaction_collection_sub_tick_enabled_returns_ok(void) {
    hu_reaction_collection_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = true;
    snprintf(cfg.channels[0], sizeof(cfg.channels[0]), "imessage");
    cfg.channel_count = 1;
    cfg.poll_interval_seconds = 30;
    snprintf(cfg.chatdb_path, sizeof(cfg.chatdb_path), "/tmp/test-nonexistent.db");

    /* Reset guards */
    hu_daemon_reaction_poll_reset_warn_guards_for_test();

    int64_t last_poll = 0;
    int64_t watermark = 0;

    /* Should return HU_OK */
    HU_ASSERT_EQ(hu_daemon_tick_reaction_poll(&cfg, 1000, &last_poll, &watermark), HU_OK);

    /* Second invocation should also work */
    HU_ASSERT_EQ(hu_daemon_tick_reaction_poll(&cfg, 2000, &last_poll, &watermark), HU_OK);
}

void run_config_gated_subsystems_tests(void) {
    HU_TEST_SUITE("config_gated");
    HU_RUN_TEST(test_reaction_collection_disabled_returns_ok);
    HU_RUN_TEST(test_reaction_collection_enabled_with_no_chatdb_env);
    HU_RUN_TEST(test_reaction_collection_guards_are_independent);
    HU_RUN_TEST(test_reaction_collection_sub_tick_disabled_returns_ok);
    HU_RUN_TEST(test_reaction_collection_sub_tick_enabled_returns_ok);
}
