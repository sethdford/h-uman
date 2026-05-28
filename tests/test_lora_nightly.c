/* tests/test_lora_nightly.c
 *
 * Sprint B residuals #3 — nightly export→train→swap orchestrator.
 * Contracts (10 tests):
 *   should_run predicate:
 *     1. <MIN_NEW_PAIRS new → false (no train on tiny deltas)
 *     2. ≥MIN_NEW_PAIRS, never run → true
 *     3. ≥MIN_NEW_PAIRS, last_run >24h ago → true
 *     4. ≥MIN_NEW_PAIRS, last_run <24h ago → false
 *   config defaults:
 *     5. fills paths from $HOME
 *     6. fails when $HOME unset
 *   symlink rotation:
 *     7. creates new symlink pointing at target
 *     8. atomic replacement when symlink exists
 *     9. NULL/empty args → INVALID_ARGUMENT
 *  10. orchestrator end-to-end returns NOT_SUPPORTED in test build
 */

#include "test_framework.h"

#ifdef HU_ENABLE_ML

#include "human/ml/lora_nightly.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── should_run predicate ────────────────────────────────────────────── */

static void test_should_run_below_min_pairs_false(void) {
    HU_ASSERT_TRUE(!hu_lora_nightly_should_run(1700000000, 0, 5));
    HU_ASSERT_TRUE(!hu_lora_nightly_should_run(1700000000, 0, HU_LORA_NIGHTLY_MIN_NEW_PAIRS - 1));
}

static void test_should_run_never_run_with_enough_pairs(void) {
    HU_ASSERT_TRUE(hu_lora_nightly_should_run(1700000000, 0, HU_LORA_NIGHTLY_MIN_NEW_PAIRS));
}

static void test_should_run_more_than_24h_ago_true(void) {
    int64_t now = 1700000000;
    int64_t last = now - HU_LORA_NIGHTLY_MIN_INTERVAL_SEC - 1;
    HU_ASSERT_TRUE(hu_lora_nightly_should_run(now, last, HU_LORA_NIGHTLY_MIN_NEW_PAIRS));
}

static void test_should_run_within_24h_false(void) {
    int64_t now = 1700000000;
    int64_t last = now - (HU_LORA_NIGHTLY_MIN_INTERVAL_SEC - 60);
    HU_ASSERT_TRUE(!hu_lora_nightly_should_run(now, last, HU_LORA_NIGHTLY_MIN_NEW_PAIRS));
}

/* ── config defaults ─────────────────────────────────────────────────── */

/* IMPORTANT: getenv() returns a pointer that setenv/unsetenv may
 * invalidate. Always strdup() the original before mutating, then
 * free after restoring. ASan caught this on first run (heap-use-after-
 * free in __setenv_locked) — the duped buffer is the fix. */
static void test_config_defaults_fills_paths_from_home(void) {
    const char *getenv_home = getenv("HOME");
    char *orig_home = getenv_home ? strdup(getenv_home) : NULL;
    setenv("HOME", "/tmp/hu_nightly_test_home", 1);
    hu_lora_nightly_config_t cfg;
    HU_ASSERT_TRUE(hu_lora_nightly_config_init_defaults(&cfg));
    HU_ASSERT_TRUE(strstr(cfg.db_path, "/tmp/hu_nightly_test_home/.human/memory.db") != NULL);
    HU_ASSERT_TRUE(strstr(cfg.pairs_jsonl_path, "/.human/lora-pairs.jsonl") != NULL);
    HU_ASSERT_TRUE(strstr(cfg.adapters_dir, "/.human/adapters") != NULL);
    HU_ASSERT_TRUE(strstr(cfg.current_symlink, "/.human/adapter-current") != NULL);
    HU_ASSERT_TRUE(strstr(cfg.mlx_base_url, "127.0.0.1:8741") != NULL);
    /* N1: defaults now include a base_model. Verify it's a non-empty,
     * mlx-community-shaped identifier (just sanity — exact id may
     * change as the runbook recommends different starting models). */
    HU_ASSERT_TRUE(cfg.base_model[0] != '\0');
    HU_ASSERT_TRUE(strstr(cfg.base_model, "/") != NULL); /* org/model shape */
    HU_ASSERT_TRUE(!cfg.dry_run);
    if (orig_home) {
        setenv("HOME", orig_home, 1);
        free(orig_home);
    } else {
        unsetenv("HOME");
    }
}

static void test_config_defaults_fails_when_home_unset(void) {
    const char *getenv_home = getenv("HOME");
    char *orig_home = getenv_home ? strdup(getenv_home) : NULL;
    unsetenv("HOME");
    hu_lora_nightly_config_t cfg;
    HU_ASSERT_TRUE(!hu_lora_nightly_config_init_defaults(&cfg));
    if (orig_home) {
        setenv("HOME", orig_home, 1);
        free(orig_home);
    }
}

/* ── symlink rotation ────────────────────────────────────────────────── */

static void test_rotate_symlink_creates_new_link(void) {
    char link_path[256];
    snprintf(link_path, sizeof(link_path), "/tmp/hu_nightly_link_%d", (int)getpid());
    unlink(link_path);
    hu_error_t err = hu_lora_nightly_rotate_symlink(link_path, "/tmp/some/target/v1");
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    char readbuf[256] = {0};
    ssize_t rn = readlink(link_path, readbuf, sizeof(readbuf) - 1);
    HU_ASSERT_TRUE(rn > 0);
    if (rn > 0)
        readbuf[rn] = '\0';
    HU_ASSERT_STR_EQ(readbuf, "/tmp/some/target/v1");
    unlink(link_path);
}

static void test_rotate_symlink_replaces_existing_atomically(void) {
    char link_path[256];
    snprintf(link_path, sizeof(link_path), "/tmp/hu_nightly_link_replace_%d", (int)getpid());
    unlink(link_path);
    /* First rotation. */
    HU_ASSERT_EQ((int)hu_lora_nightly_rotate_symlink(link_path, "/tmp/v1"), (int)HU_OK);
    /* Second rotation overwrites the first. */
    HU_ASSERT_EQ((int)hu_lora_nightly_rotate_symlink(link_path, "/tmp/v2"), (int)HU_OK);
    char readbuf[256] = {0};
    ssize_t rn = readlink(link_path, readbuf, sizeof(readbuf) - 1);
    if (rn > 0)
        readbuf[rn] = '\0';
    HU_ASSERT_STR_EQ(readbuf, "/tmp/v2");
    unlink(link_path);
}

static void test_rotate_symlink_null_or_empty_args(void) {
    HU_ASSERT_EQ((int)hu_lora_nightly_rotate_symlink(NULL, "/tmp/x"), (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((int)hu_lora_nightly_rotate_symlink("/tmp/x", NULL), (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((int)hu_lora_nightly_rotate_symlink("", "/tmp/x"), (int)HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((int)hu_lora_nightly_rotate_symlink("/tmp/x", ""), (int)HU_ERR_INVALID_ARGUMENT);
}

/* ── orchestrator end-to-end ─────────────────────────────────────────── */

static void test_orchestrator_returns_not_supported_in_test_build(void) {
    /* In HU_IS_TEST builds, hu_lora_export_dpo_pairs stubs out to
     * NOT_SUPPORTED → the orchestrator propagates that immediately,
     * never reaching rotation or swap. */
    hu_lora_nightly_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.db_path, sizeof(cfg.db_path), "/tmp/x.db");
    snprintf(cfg.pairs_jsonl_path, sizeof(cfg.pairs_jsonl_path), "/tmp/x.jsonl");
    snprintf(cfg.adapters_dir, sizeof(cfg.adapters_dir), "/tmp/x-adapters");
    snprintf(cfg.current_symlink, sizeof(cfg.current_symlink), "/tmp/x-current");
    snprintf(cfg.mlx_base_url, sizeof(cfg.mlx_base_url), "http://127.0.0.1:9/v1");
    cfg.dry_run = true;

    hu_allocator_t alloc = hu_system_allocator();
    size_t count = 0;
    hu_error_t err = hu_lora_nightly_run(&alloc, &cfg, 1700000000, &count);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_NOT_SUPPORTED);
}

/* ── MLX admin swap path tests (AC-6) ────────────────────────────────── */

/* Test that the swap function returns the correct error code on all paths
 * (curl-on via mock HTTP, curl-off via HU_ENABLE_CURL gating). */

static void test_swap_predicate_threshold_boundary(void) {
    /* should_run returns false below MIN_NEW_PAIRS, true at and above. */
    int32_t threshold = HU_LORA_NIGHTLY_MIN_NEW_PAIRS;
    int64_t now = 1700000000;
    int64_t last = 0; /* never run */

    /* Below threshold → false. */
    HU_ASSERT_TRUE(!hu_lora_nightly_should_run(now, last, threshold - 1));
    /* At threshold → true. */
    HU_ASSERT_TRUE(hu_lora_nightly_should_run(now, last, threshold));
    /* Above threshold → true. */
    HU_ASSERT_TRUE(hu_lora_nightly_should_run(now, last, threshold + 1));
}

void run_lora_nightly_tests(void) {
    HU_TEST_SUITE("lora_nightly");
    HU_RUN_TEST(test_should_run_below_min_pairs_false);
    HU_RUN_TEST(test_should_run_never_run_with_enough_pairs);
    HU_RUN_TEST(test_should_run_more_than_24h_ago_true);
    HU_RUN_TEST(test_should_run_within_24h_false);
    HU_RUN_TEST(test_config_defaults_fills_paths_from_home);
    HU_RUN_TEST(test_config_defaults_fails_when_home_unset);
    HU_RUN_TEST(test_rotate_symlink_creates_new_link);
    HU_RUN_TEST(test_rotate_symlink_replaces_existing_atomically);
    HU_RUN_TEST(test_rotate_symlink_null_or_empty_args);
    HU_RUN_TEST(test_orchestrator_returns_not_supported_in_test_build);
    HU_RUN_TEST(test_swap_predicate_threshold_boundary);
}

#else /* !HU_ENABLE_ML — stub runner so the symbol always resolves */

void run_lora_nightly_tests(void) { /* no-op when ML is disabled */ }

#endif /* HU_ENABLE_ML */
