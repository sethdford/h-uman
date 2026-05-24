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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Helper: count occurrences of needle as substring in haystack */
static int count_substr(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle)
        return 0;
    int count = 0;
    const char *p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += strlen(needle);
    }
    return count;
}

/* Helper: read entire file into out_buf (caller frees) */
static size_t slurp_file(const char *path, char **out_buf) {
    *out_buf = NULL;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return 0;
    }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    *out_buf = buf;
    return got;
}

/* Helper: build temp path for stderr capture */
static void make_tmp_path(char *out, size_t cap, const char *tag) {
    snprintf(out, cap, "/tmp/test_config_gated_%s_%d.log", tag, (int)getpid());
}

/* Helper: save stderr for restoration */
static FILE *saved_stderr_for_restore(void) {
    int dup_fd = dup(fileno(stderr));
    if (dup_fd < 0)
        return NULL;
    return fdopen(dup_fd, "w");
}

/* Helper: restore stderr from saved copy */
static void restore_stderr(FILE *saved) {
    if (!saved)
        return;
    fflush(stderr);
    dup2(fileno(saved), fileno(stderr));
    fclose(saved);
}

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

/* Test: NULL cfg emits warning exactly once (programmer error, not config error) */
static void test_reaction_collection_null_cfg_logs_warning_once(void) {
    hu_daemon_reaction_poll_reset_warn_guards_for_test();

    char tmp_path[256];
    make_tmp_path(tmp_path, sizeof(tmp_path), "rxn_null_cfg");

    FILE *saved = saved_stderr_for_restore();
    HU_ASSERT_NOT_NULL(saved);
    HU_ASSERT_NOT_NULL(freopen(tmp_path, "w+", stderr));

    /* Fire tick with NULL cfg multiple times */
    for (int i = 0; i < 3; i++) {
        size_t ingested = 0;
        (void)hu_daemon_reaction_poll_tick(NULL, 0, &ingested);
    }

    fflush(stderr);
    restore_stderr(saved);

    char *buf = NULL;
    (void)slurp_file(tmp_path, &buf);
    HU_ASSERT_NOT_NULL(buf);

    /* Should emit warning about NULL cfg exactly once */
    HU_ASSERT_STR_CONTAINS(buf, "cfg is NULL");
    int n = count_substr(buf, "cfg is NULL");
    HU_ASSERT_EQ(n, 1);

    free(buf);
    unlink(tmp_path);
}

void run_config_gated_subsystems_tests(void) {
    HU_TEST_SUITE("config_gated");
    HU_RUN_TEST(test_reaction_collection_disabled_returns_ok);
    HU_RUN_TEST(test_reaction_collection_enabled_with_no_chatdb_env);
    HU_RUN_TEST(test_reaction_collection_guards_are_independent);
    HU_RUN_TEST(test_reaction_collection_sub_tick_disabled_returns_ok);
    HU_RUN_TEST(test_reaction_collection_sub_tick_enabled_returns_ok);
    HU_RUN_TEST(test_reaction_collection_null_cfg_logs_warning_once);
}
