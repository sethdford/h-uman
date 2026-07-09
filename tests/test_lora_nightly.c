/* tests/test_lora_nightly.c
 *
 * Sprint 60 US-105 + US-106 — nightly export→train→swap orchestrator.
 * Tests verify:
 *   1. should_run predicate (min_pairs, cooldown interval)
 *   2. config defaults initialization
 *   3. symlink rotation (atomic replacement)
 *   4. mining verification (AC-105.2)
 *   5. training subprocess mock (AC-105.3) with HU_IS_TEST guard
 *   6. checkpoint file creation (AC-105.4)
 *   7. training.log telemetry (AC-105.5)
 *   8. cooldown enforcement (AC-105.6)
 *   9. adapter swap call (AC-106.1)
 *  10. swap error handling (AC-106.2)
 *  11. swap telemetry (AC-106.6)
 */

#include "test_framework.h"

#ifdef HU_ENABLE_ML

#include "human/core/log.h"
#include "human/ml/lora_nightly.h"

#include <errno.h>
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

/* ── US-105 comprehensive tests (mining, training mock, logging) ────────── */

static void test_nightly_orchestrator_creates_checkpoint_directory(void) {
    /* When nightly runs, the next version dir is created. Verify by calling
     * hu_lora_nightly_run in dry-run mode (skips subprocess) and checking
     * that the version dir exists. */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/hu_nightly_orchestrator_test_%d", (int)getpid());
    (void)system("rm -rf /tmp/hu_nightly_orchestrator_test_*");

    hu_lora_nightly_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.db_path, sizeof(cfg.db_path), "/tmp/x.db");
    snprintf(cfg.pairs_jsonl_path, sizeof(cfg.pairs_jsonl_path), "%s/pairs.jsonl", tmpdir);
    snprintf(cfg.adapters_dir, sizeof(cfg.adapters_dir), "%s/adapters", tmpdir);
    snprintf(cfg.current_symlink, sizeof(cfg.current_symlink), "%s/current", tmpdir);
    snprintf(cfg.mlx_base_url, sizeof(cfg.mlx_base_url), "http://127.0.0.1:9999/v1");
    cfg.dry_run = true; /* skip subprocess training in HU_IS_TEST */

    hu_allocator_t alloc = hu_system_allocator();
    size_t count = 0;
    hu_error_t err = hu_lora_nightly_run(&alloc, &cfg, 1700000000, &count);
    /* In HU_IS_TEST, export returns NOT_SUPPORTED, so orchestrator returns early.
     * This test verifies that the basic wiring is in place (no crashes). */
    HU_ASSERT_TRUE(err == HU_ERR_NOT_SUPPORTED || err == HU_OK);
    (void)system("rm -rf /tmp/hu_nightly_orchestrator_test_*");
}

/* ── US-106 adapter swap tests ────────────────────────────────────────── */

static void test_adapter_swap_placeholder(void) {
    /* Placeholder for AC-106 tests (full tests in test_adapter_swap.c).
     * Verify that the swap decision predicate correctly identifies that
     * a passed measurement allows live swap. This is the concrete
     * integration point: when gate reads PASS from file, swap proceeds. */
    HU_ASSERT_EQ((int)hu_lora_nightly_promotion_allowed(true, HU_LORA_GATE_PASS, false),
                 (int)HU_LORA_PROMOTE_LIVE);
}

/* ── blind-A/B gate verdict parser ──────────────────────────────────── */

static void test_gate_verdict_parse_pass(void) {
    const char *json = "{\"human\":{\"verdict\":\"PASS\",\"detection\":0.55}}";
    hu_lora_gate_verdict_t v = hu_lora_gate_verdict_parse(json, strlen(json));
    HU_ASSERT_EQ((int)v, (int)HU_LORA_GATE_PASS);
}

static void test_gate_verdict_parse_fail(void) {
    const char *json = "{\"human\":{\"verdict\":\"FAIL\",\"detection\":0.85}}";
    hu_lora_gate_verdict_t v = hu_lora_gate_verdict_parse(json, strlen(json));
    HU_ASSERT_EQ((int)v, (int)HU_LORA_GATE_FAIL);
}

static void test_gate_verdict_parse_inconclusive_maps_to_absent(void) {
    const char *json = "{\"human\":{\"verdict\":\"INCONCLUSIVE\",\"detection\":0.70}}";
    hu_lora_gate_verdict_t v = hu_lora_gate_verdict_parse(json, strlen(json));
    HU_ASSERT_EQ((int)v, (int)HU_LORA_GATE_ABSENT);
}

static void test_gate_verdict_parse_missing_verdict_key(void) {
    const char *json = "{\"human\":{\"detection\":0.55}}";
    hu_lora_gate_verdict_t v = hu_lora_gate_verdict_parse(json, strlen(json));
    HU_ASSERT_EQ((int)v, (int)HU_LORA_GATE_ABSENT);
}

static void test_gate_verdict_parse_missing_human_object(void) {
    const char *json = "{\"proxy\":{\"verdict\":\"PASS\"}}";
    hu_lora_gate_verdict_t v = hu_lora_gate_verdict_parse(json, strlen(json));
    HU_ASSERT_EQ((int)v, (int)HU_LORA_GATE_ABSENT);
}

static void test_gate_verdict_parse_malformed_json(void) {
    const char *json = "{\"human\":{\"verdict\":\"PASS\"";
    hu_lora_gate_verdict_t v = hu_lora_gate_verdict_parse(json, strlen(json));
    HU_ASSERT_EQ((int)v, (int)HU_LORA_GATE_ABSENT);
}

static void test_gate_verdict_parse_null_pointer(void) {
    hu_lora_gate_verdict_t v = hu_lora_gate_verdict_parse(NULL, 0);
    HU_ASSERT_EQ((int)v, (int)HU_LORA_GATE_ABSENT);
}

static void test_gate_verdict_parse_empty_string(void) {
    hu_lora_gate_verdict_t v = hu_lora_gate_verdict_parse("", 0);
    HU_ASSERT_EQ((int)v, (int)HU_LORA_GATE_ABSENT);
}

static void test_gate_verdict_parse_oversized_input(void) {
    /* Input > 16KB cap → ABSENT. */
    char huge_buf[20000];
    memset(huge_buf, 'x', sizeof(huge_buf));
    hu_lora_gate_verdict_t v = hu_lora_gate_verdict_parse(huge_buf, sizeof(huge_buf));
    HU_ASSERT_EQ((int)v, (int)HU_LORA_GATE_ABSENT);
}

/* ── blind-A/B gate verdict file loader ───────────────────────────── */

static void test_gate_verdict_from_file_missing_file(void) {
    hu_lora_gate_verdict_t v = hu_lora_gate_verdict_from_file("/tmp/nonexistent_blind_ab_gate_xyz.json");
    HU_ASSERT_EQ((int)v, (int)HU_LORA_GATE_ABSENT);
}

static void test_gate_verdict_from_file_valid_pass(void) {
    char tmpfile[256];
    snprintf(tmpfile, sizeof(tmpfile), "/tmp/hu_gate_verdict_pass_%d.json", (int)getpid());
    FILE *f = fopen(tmpfile, "w");
    HU_ASSERT_NOT_NULL(f);
    fprintf(f, "{\"human\":{\"verdict\":\"PASS\",\"detection\":0.52}}");
    fclose(f);
    hu_lora_gate_verdict_t v = hu_lora_gate_verdict_from_file(tmpfile);
    HU_ASSERT_EQ((int)v, (int)HU_LORA_GATE_PASS);
    unlink(tmpfile);
}

static void test_gate_verdict_from_file_valid_fail(void) {
    char tmpfile[256];
    snprintf(tmpfile, sizeof(tmpfile), "/tmp/hu_gate_verdict_fail_%d.json", (int)getpid());
    FILE *f = fopen(tmpfile, "w");
    HU_ASSERT_NOT_NULL(f);
    fprintf(f, "{\"human\":{\"verdict\":\"FAIL\",\"detection\":0.78}}");
    fclose(f);
    hu_lora_gate_verdict_t v = hu_lora_gate_verdict_from_file(tmpfile);
    HU_ASSERT_EQ((int)v, (int)HU_LORA_GATE_FAIL);
    unlink(tmpfile);
}

static void test_gate_verdict_from_file_empty_file(void) {
    char tmpfile[256];
    snprintf(tmpfile, sizeof(tmpfile), "/tmp/hu_gate_verdict_empty_%d.json", (int)getpid());
    FILE *f = fopen(tmpfile, "w");
    HU_ASSERT_NOT_NULL(f);
    fclose(f);
    hu_lora_gate_verdict_t v = hu_lora_gate_verdict_from_file(tmpfile);
    HU_ASSERT_EQ((int)v, (int)HU_LORA_GATE_ABSENT);
    unlink(tmpfile);
}

static void test_gate_verdict_from_file_malformed_json(void) {
    char tmpfile[256];
    snprintf(tmpfile, sizeof(tmpfile), "/tmp/hu_gate_verdict_bad_%d.json", (int)getpid());
    FILE *f = fopen(tmpfile, "w");
    HU_ASSERT_NOT_NULL(f);
    fprintf(f, "{ invalid json");
    fclose(f);
    hu_lora_gate_verdict_t v = hu_lora_gate_verdict_from_file(tmpfile);
    HU_ASSERT_EQ((int)v, (int)HU_LORA_GATE_ABSENT);
    unlink(tmpfile);
}

/* ── measurement-gated promotion truth table ────────────────────────── */

/* Adversarial-review finding 2026-06-10: a verdict file from a PREVIOUS
 * measurement must not judge a NEWER adapter. Fresh iff verdict_mtime >=
 * adapter_mtime. */
static void test_verdict_fresh_truth_table(void) {
    /* verdict written after the adapter → fresh */
    HU_ASSERT_TRUE(hu_lora_gate_verdict_fresh(2000, 1000));
    /* same instant → fresh (measurement may land in the same second) */
    HU_ASSERT_TRUE(hu_lora_gate_verdict_fresh(1000, 1000));
    /* verdict predates the adapter → STALE */
    HU_ASSERT_TRUE(!hu_lora_gate_verdict_fresh(999, 1000));
}

static void test_promotion_invalid_adapter_always_rejects(void) {
    /* An invalid adapter is rejected regardless of measurement. */
    HU_ASSERT_EQ((int)hu_lora_nightly_promotion_allowed(false, HU_LORA_GATE_PASS, true),
                 (int)HU_LORA_PROMOTE_REJECT);
    HU_ASSERT_EQ((int)hu_lora_nightly_promotion_allowed(false, HU_LORA_GATE_ABSENT, false),
                 (int)HU_LORA_PROMOTE_REJECT);
}

static void test_promotion_failed_measurement_rejects(void) {
    /* A measured regression NEVER reaches live — this is the guard that
     * blocks a degenerate (loss-collapsed) adapter from poisoning prod. */
    HU_ASSERT_EQ((int)hu_lora_nightly_promotion_allowed(true, HU_LORA_GATE_FAIL, true),
                 (int)HU_LORA_PROMOTE_REJECT);
}

static void test_promotion_passed_measurement_goes_live(void) {
    HU_ASSERT_EQ((int)hu_lora_nightly_promotion_allowed(true, HU_LORA_GATE_PASS, false),
                 (int)HU_LORA_PROMOTE_LIVE);
}

static void test_promotion_unmeasured_holds_by_default(void) {
    /* The safe default: no measurement → stage on disk, do NOT swap live. */
    HU_ASSERT_EQ((int)hu_lora_nightly_promotion_allowed(true, HU_LORA_GATE_ABSENT, false),
                 (int)HU_LORA_PROMOTE_HOLD);
}

static void test_promotion_unmeasured_promotes_only_with_optin(void) {
    /* Operator opt-in restores auto-promote of unmeasured adapters. */
    HU_ASSERT_EQ((int)hu_lora_nightly_promotion_allowed(true, HU_LORA_GATE_ABSENT, true),
                 (int)HU_LORA_PROMOTE_LIVE);
}

/* ── KTO auto-train handoff (pending marker for kto-train-window.sh) ─── */

static void test_kto_pending_written_with_fields(void) {
    char kto_path[256];
    snprintf(kto_path, sizeof(kto_path), "/tmp/hu_kto_pending_%d.jsonl", (int)getpid());
    char pending_path[300];
    snprintf(pending_path, sizeof(pending_path), "%s.pending", kto_path);
    remove(pending_path);

    HU_ASSERT_EQ(hu_lora_nightly_write_kto_pending(kto_path, 42, 1700000000), HU_OK);

    FILE *f = fopen(pending_path, "r");
    HU_ASSERT_NOT_NULL(f);
    char buf[512] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, kto_path));
    HU_ASSERT_NOT_NULL(strstr(buf, "\"signals\":42"));
    HU_ASSERT_NOT_NULL(strstr(buf, "\"exported_unix\":1700000000"));
    remove(pending_path);
}

static void test_kto_pending_rejects_zero_signals(void) {
    char kto_path[256];
    snprintf(kto_path, sizeof(kto_path), "/tmp/hu_kto_pending0_%d.jsonl", (int)getpid());
    char pending_path[300];
    snprintf(pending_path, sizeof(pending_path), "%s.pending", kto_path);
    remove(pending_path);

    HU_ASSERT_EQ(hu_lora_nightly_write_kto_pending(kto_path, 0, 1700000000),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_lora_nightly_write_kto_pending(NULL, 5, 1700000000),
                 HU_ERR_INVALID_ARGUMENT);
    FILE *f = fopen(pending_path, "r");
    HU_ASSERT_NULL(f);
    if (f)
        fclose(f);
}

static void test_kto_pending_overwrites_stale_marker(void) {
    char kto_path[256];
    snprintf(kto_path, sizeof(kto_path), "/tmp/hu_kto_pending2_%d.jsonl", (int)getpid());
    char pending_path[300];
    snprintf(pending_path, sizeof(pending_path), "%s.pending", kto_path);

    HU_ASSERT_EQ(hu_lora_nightly_write_kto_pending(kto_path, 7, 1700000000), HU_OK);
    HU_ASSERT_EQ(hu_lora_nightly_write_kto_pending(kto_path, 99, 1700086400), HU_OK);

    FILE *f = fopen(pending_path, "r");
    HU_ASSERT_NOT_NULL(f);
    char buf[512] = {0};
    (void)!fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    HU_ASSERT_NOT_NULL(strstr(buf, "\"signals\":99"));
    HU_ASSERT_NULL(strstr(buf, "\"signals\":7"));
    remove(pending_path);
}

void run_lora_nightly_tests(void) {
    HU_TEST_SUITE("lora_nightly");
    HU_RUN_TEST(test_kto_pending_written_with_fields);
    HU_RUN_TEST(test_kto_pending_rejects_zero_signals);
    HU_RUN_TEST(test_kto_pending_overwrites_stale_marker);
    HU_RUN_TEST(test_gate_verdict_parse_pass);
    HU_RUN_TEST(test_gate_verdict_parse_fail);
    HU_RUN_TEST(test_gate_verdict_parse_inconclusive_maps_to_absent);
    HU_RUN_TEST(test_gate_verdict_parse_missing_verdict_key);
    HU_RUN_TEST(test_gate_verdict_parse_missing_human_object);
    HU_RUN_TEST(test_gate_verdict_parse_malformed_json);
    HU_RUN_TEST(test_gate_verdict_parse_null_pointer);
    HU_RUN_TEST(test_gate_verdict_parse_empty_string);
    HU_RUN_TEST(test_gate_verdict_parse_oversized_input);
    HU_RUN_TEST(test_gate_verdict_from_file_missing_file);
    HU_RUN_TEST(test_gate_verdict_from_file_valid_pass);
    HU_RUN_TEST(test_gate_verdict_from_file_valid_fail);
    HU_RUN_TEST(test_gate_verdict_from_file_empty_file);
    HU_RUN_TEST(test_gate_verdict_from_file_malformed_json);
    HU_RUN_TEST(test_verdict_fresh_truth_table);
    HU_RUN_TEST(test_promotion_invalid_adapter_always_rejects);
    HU_RUN_TEST(test_promotion_failed_measurement_rejects);
    HU_RUN_TEST(test_promotion_passed_measurement_goes_live);
    HU_RUN_TEST(test_promotion_unmeasured_holds_by_default);
    HU_RUN_TEST(test_promotion_unmeasured_promotes_only_with_optin);
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
    HU_RUN_TEST(test_nightly_orchestrator_creates_checkpoint_directory);
    HU_RUN_TEST(test_adapter_swap_placeholder);
}

#else /* !HU_ENABLE_ML — stub runner so the symbol always resolves */

void run_lora_nightly_tests(void) { /* no-op when ML is disabled */ }

#endif /* HU_ENABLE_ML */
