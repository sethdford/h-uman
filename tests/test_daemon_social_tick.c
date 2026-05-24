/* tests/test_daemon_social_tick.c
 *
 * Sprint A.6 wiring tests. The underlying scanners
 * (hu_imessage_scan_stale_contacts / hu_contact_signature_top_n /
 * hu_drift_scan_top_contacts) all return HU_ERR_NOT_SUPPORTED under
 * HU_IS_TEST, so we can only test that the orchestrator + JSON
 * emitter does the right thing on empty inputs. Production
 * verification happens by running the daemon against the user's
 * chat.db and inspecting ~/.human/social_state.json. */

#include "human/daemon_social_tick.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void unlink_if_exists(const char *path) {
    struct stat sb;
    if (stat(path, &sb) == 0)
        unlink(path);
}

static void test_social_tick_run_writes_empty_skeleton_in_test_mode(void) {
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/hu_social_tick_e2e_%d.json", (int)getpid());
    unlink_if_exists(tmp_path);

    /* All three scanners stub to HU_ERR_NOT_SUPPORTED in test mode → we
     * expect an empty skeleton (zero arrays) but a valid JSON file. */
    hu_error_t err =
        hu_daemon_social_tick_run(NULL, "/tmp/nonexistent.db", tmp_path, 1730000000, 16);
    HU_ASSERT_EQ((int)err, (int)HU_OK);

    FILE *f = fopen(tmp_path, "r");
    HU_ASSERT_NOT_NULL(f);
    char buf[4096] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    HU_ASSERT_TRUE(n > 0);

    /* Stable JSON shape — downstream consumers (PWA, persona prompt,
     * `cat ~/.human/social_state.json`) rely on these field names. */
    HU_ASSERT_TRUE(strstr(buf, "\"generated_at_unix\":") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"stale_contacts\":") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"signatures\":") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "\"drift_alerts\":") != NULL);

    unlink(tmp_path);
}

static void test_social_tick_interval_gates_subsequent_runs(void) {
    /* The interval-gated wrapper: subsequent calls within 6 hours of
     * the last run should NOT trigger another scan. We verify by
     * leaving last_run_unix_inout at a recent value. */
    int64_t last_run = 1730000000;
    int64_t now_close = 1730000000 + 60; /* 60s later */
    hu_error_t err = hu_daemon_social_tick(NULL, now_close, &last_run);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    /* last_run should NOT have advanced — the gate held */
    HU_ASSERT_EQ((int)last_run, 1730000000);
}

static void test_social_tick_interval_allows_after_six_hours(void) {
    int64_t last_run = 1730000000;
    int64_t now_after = 1730000000 + (6 * 60 * 60) + 60; /* 6h+1m later */
    hu_error_t err = hu_daemon_social_tick(NULL, now_after, &last_run);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    /* last_run should have advanced to `now` */
    HU_ASSERT_EQ((long long)last_run, (long long)now_after);
}

static void test_social_tick_rejects_null_last_run_pointer(void) {
    hu_error_t err = hu_daemon_social_tick(NULL, 1730000000, NULL);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_INVALID_ARGUMENT);
}

void run_daemon_social_tick_tests(void) {
    HU_TEST_SUITE("daemon_social_tick");
    HU_RUN_TEST(test_social_tick_run_writes_empty_skeleton_in_test_mode);
    HU_RUN_TEST(test_social_tick_interval_gates_subsequent_runs);
    HU_RUN_TEST(test_social_tick_interval_allows_after_six_hours);
    HU_RUN_TEST(test_social_tick_rejects_null_last_run_pointer);
}
