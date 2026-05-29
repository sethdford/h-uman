/* tests/test_daemon_follow_up_watcher.c
 *
 * Unit tests for the follow-up watcher daemon tick
 * (src/daemon_follow_up_watcher.c). The tick polls for read-but-unreplied
 * iMessage threads and schedules circadian-aware follow-ups; it is
 * config-gated via hu_follow_up_watcher_config_t.enabled.
 *
 * These tests exercise the tick's deterministic control-flow contract
 * directly — no daemon, no chat.db, no network, no spawning. We pass
 * NULL for the agent/config/channels/throttle arguments because the
 * current implementation is a stub that ignores them (it logs + advances
 * the watermark). The interval gate and the disabled/enabled one-shot
 * log guards ARE exercised.
 *
 * Contract under test:
 *   - NULL cfg / last_poll / watermark -> HU_ERR_INVALID_ARGUMENT
 *   - enabled=false                    -> HU_OK, no watermark advance
 *   - enabled=true, first poll         -> HU_OK, watermark advances to now
 *   - enabled=true, within interval    -> HU_OK, watermark NOT re-advanced
 */

#include "test_framework.h"

#include "human/config.h"
#include "human/core/error.h"
#include "human/daemon.h"

/* The reset helper is declared only under #if HU_IS_TEST in the source
 * with no public header; tests link against the test build (HU_IS_TEST=1)
 * so the symbol resolves. Declared extern here to call it. */
extern void hu_daemon_follow_up_watcher_reset_warn_guards_for_test(void);

/* NULL cfg is rejected with HU_ERR_INVALID_ARGUMENT and never crashes. */
static void test_follow_up_watcher_null_cfg_returns_invalid_argument(void) {
    int64_t last_poll = 0;
    int64_t watermark = 0;
    hu_error_t err = hu_daemon_tick_follow_up_watcher(NULL, 1000, &last_poll, &watermark, NULL,
                                                      NULL, NULL, 0, NULL);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INVALID_ARGUMENT);
}

/* NULL in/out pointers are rejected too. */
static void test_follow_up_watcher_null_outptrs_returns_invalid_argument(void) {
    hu_follow_up_watcher_config_t cfg = {.enabled = true, .interval_seconds = 300};
    int64_t watermark = 0;
    HU_ASSERT_EQ((int)hu_daemon_tick_follow_up_watcher(&cfg, 1000, NULL, &watermark, NULL, NULL,
                                                       NULL, 0, NULL),
                 (int)HU_ERR_INVALID_ARGUMENT);
    int64_t last_poll = 0;
    HU_ASSERT_EQ((int)hu_daemon_tick_follow_up_watcher(&cfg, 1000, &last_poll, NULL, NULL, NULL,
                                                       NULL, 0, NULL),
                 (int)HU_ERR_INVALID_ARGUMENT);
}

/* Disabled config is a clean no-op: HU_OK, watermark untouched. */
static void test_follow_up_watcher_disabled_is_noop(void) {
    hu_daemon_follow_up_watcher_reset_warn_guards_for_test();
    hu_follow_up_watcher_config_t cfg = {.enabled = false, .interval_seconds = 300};
    int64_t last_poll = 0;
    int64_t watermark = 0;
    hu_error_t err = hu_daemon_tick_follow_up_watcher(&cfg, 1000, &last_poll, &watermark, NULL,
                                                      NULL, NULL, 0, NULL);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ((int)watermark, 0); /* disabled path must not advance */
    HU_ASSERT_EQ((int)last_poll, 0);
}

/* Enabled + first poll (last_poll==0) does the work: advances watermark
 * and last_poll to now_unix. */
static void test_follow_up_watcher_enabled_first_poll_advances_watermark(void) {
    hu_daemon_follow_up_watcher_reset_warn_guards_for_test();
    hu_follow_up_watcher_config_t cfg = {.enabled = true, .interval_seconds = 300};
    int64_t last_poll = 0;
    int64_t watermark = 0;
    hu_error_t err = hu_daemon_tick_follow_up_watcher(&cfg, 5000, &last_poll, &watermark, NULL,
                                                      NULL, NULL, 0, NULL);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ((int)watermark, 5000);
    HU_ASSERT_EQ((int)last_poll, 5000);
}

/* Enabled but called again within the interval: the interval gate
 * short-circuits, so the watermark is NOT re-advanced. */
static void test_follow_up_watcher_within_interval_does_not_readvance(void) {
    hu_daemon_follow_up_watcher_reset_warn_guards_for_test();
    hu_follow_up_watcher_config_t cfg = {.enabled = true, .interval_seconds = 300};
    int64_t last_poll = 5000;
    int64_t watermark = 5000;
    /* now is only 100s after last_poll (< 300s interval). */
    hu_error_t err = hu_daemon_tick_follow_up_watcher(&cfg, 5100, &last_poll, &watermark, NULL,
                                                      NULL, NULL, 0, NULL);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ((int)watermark, 5000); /* unchanged */
    HU_ASSERT_EQ((int)last_poll, 5000); /* unchanged */
}

void run_daemon_follow_up_watcher_tests(void) {
    HU_TEST_SUITE("daemon_follow_up_watcher");
    HU_RUN_TEST(test_follow_up_watcher_null_cfg_returns_invalid_argument);
    HU_RUN_TEST(test_follow_up_watcher_null_outptrs_returns_invalid_argument);
    HU_RUN_TEST(test_follow_up_watcher_disabled_is_noop);
    HU_RUN_TEST(test_follow_up_watcher_enabled_first_poll_advances_watermark);
    HU_RUN_TEST(test_follow_up_watcher_within_interval_does_not_readvance);
}
