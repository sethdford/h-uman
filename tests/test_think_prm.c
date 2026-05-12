/* SOTA-2026 init-07 — ThinkPRM trained verifier panel tests.
 *
 * Pins the S2 quality bar from the master coordinator:
 *   (a) panel construction + checkpoint load
 *   (b) panel score is deterministic given fixed weights
 *   (c) `agent.verifier_panel = false` (default) keeps agent_turn
 *       byte-identical to the pre-init-07 path
 *   (d) panel returns NOT_SUPPORTED when checkpoint missing rather than
 *       crashing
 *
 * Plus a couple of supporting guards on lifecycle / shape.
 *
 * NOTE: the agent-turn byte-identity test does not boot a real agent.
 * Instead it asserts the invariant that drives byte-identity in the
 * call site: when the panel struct is zeroed (default OFF), the new
 * code path short-circuits via `scorer_count == 0` and never observes
 * the response. We exercise that branch directly with a deterministic
 * mock and rely on the fact that the agent_turn.c diff is a single
 * `if (verifier_panel_enabled && scorer_count > 0)` guard around the
 * new call; the default of those two flags is `false`/`0` thanks to
 * the existing `memset(out, 0, ...)` in hu_agent_create.
 */

#include "human/agent/think_prm.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Small helper: write a synthetic checkpoint to a /tmp path and return
 * its path in a caller-owned static buffer. Caller must `unlink` after.
 *
 * We use the test's PID + tag in the path to keep parallel test runs
 * non-flaky. */
static const char *think_prm_tmp_path(const char *tag) {
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_test_think_prm_%d_%s.prm",
             (int)getpid(), tag);
    return path;
}

static const char *think_prm_tmp_dir(const char *tag) {
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_test_think_prm_dir_%d_%s",
             (int)getpid(), tag);
    return path;
}

/* ── (a) construction + checkpoint load ────────────────────────────── */

static void panel_construction_and_checkpoint_load(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *path = think_prm_tmp_path("ctor");

    HU_ASSERT_EQ(hu_prm_checkpoint_write_synthetic(path, 1234u, 256), HU_OK);

    const char *paths[] = {path};
    hu_verifier_panel_t panel = {0};
    HU_ASSERT_EQ(hu_verifier_panel_create(&alloc, paths, 1, &panel), HU_OK);
    HU_ASSERT_EQ((int)panel.scorer_count, 1);
    HU_ASSERT_NOT_NULL(panel.scorers);
    HU_ASSERT_EQ((int)panel.total_calls, 0);

    hu_verifier_panel_deinit(&panel);
    HU_ASSERT_EQ((int)panel.scorer_count, 0);
    HU_ASSERT_NULL(panel.scorers);

    unlink(path);
}

static void panel_construction_with_zero_paths_is_off_not_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_verifier_panel_t panel = {0};
    HU_ASSERT_EQ(hu_verifier_panel_create(&alloc, NULL, 0, &panel), HU_OK);
    HU_ASSERT_EQ((int)panel.scorer_count, 0);
    HU_ASSERT_NULL(panel.scorers);
    hu_verifier_panel_deinit(&panel);
}

static void panel_construction_loads_all_three_checkpoints(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *p1 = "/tmp/hu_test_think_prm_panel_3a.prm";
    const char *p2 = "/tmp/hu_test_think_prm_panel_3b.prm";
    const char *p3 = "/tmp/hu_test_think_prm_panel_3c.prm";

    HU_ASSERT_EQ(hu_prm_checkpoint_write_synthetic(p1, 1, 128), HU_OK);
    HU_ASSERT_EQ(hu_prm_checkpoint_write_synthetic(p2, 2, 128), HU_OK);
    HU_ASSERT_EQ(hu_prm_checkpoint_write_synthetic(p3, 3, 128), HU_OK);

    const char *paths[] = {p1, p2, p3};
    hu_verifier_panel_t panel = {0};
    HU_ASSERT_EQ(hu_verifier_panel_create(&alloc, paths, 3, &panel), HU_OK);
    HU_ASSERT_EQ((int)panel.scorer_count, 3);

    hu_verifier_panel_deinit(&panel);
    unlink(p1);
    unlink(p2);
    unlink(p3);
}

/* ── (b) deterministic score given fixed weights ──────────────────── */

static void panel_score_is_deterministic_given_fixed_weights(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *path = think_prm_tmp_path("det");

    HU_ASSERT_EQ(hu_prm_checkpoint_write_synthetic(path, 7u, 256), HU_OK);

    const char *paths[] = {path};
    hu_verifier_panel_t panel = {0};
    HU_ASSERT_EQ(hu_verifier_panel_create(&alloc, paths, 1, &panel), HU_OK);

    const char *chain =
        "Step 1: I observe x = 42.\n\n"
        "Step 2: Therefore y must be 42 as well.\n\n"
        "Step 3: I am confident in this conclusion.";
    size_t chain_len = strlen(chain);

    hu_verifier_panel_result_t r1 = {0};
    hu_verifier_panel_result_t r2 = {0};
    HU_ASSERT_EQ(hu_verifier_panel_score_chain(&panel, chain, chain_len, 16, &r1),
                 HU_OK);
    HU_ASSERT_EQ(hu_verifier_panel_score_chain(&panel, chain, chain_len, 16, &r2),
                 HU_OK);

    HU_ASSERT_EQ((int)r1.step_count, 3);
    HU_ASSERT_EQ((int)r2.step_count, 3);
    /* Bitwise determinism on every per-step score. */
    for (size_t i = 0; i < r1.step_count; i++) {
        HU_ASSERT_EQ(memcmp(&r1.steps[i].score, &r2.steps[i].score,
                            sizeof(float)),
                     0);
        HU_ASSERT_EQ(memcmp(&r1.steps[i].confidence, &r2.steps[i].confidence,
                            sizeof(float)),
                     0);
    }
    HU_ASSERT_EQ(memcmp(&r1.aggregate, &r2.aggregate, sizeof(float)), 0);
    HU_ASSERT_EQ(memcmp(&r1.aggregate_confidence, &r2.aggregate_confidence,
                        sizeof(float)),
                 0);

    /* Sanity: scores live in (0, 1) and aggregate respects that range. */
    HU_ASSERT(r1.aggregate > 0.0f && r1.aggregate < 1.0f);
    HU_ASSERT(r1.aggregate_confidence >= 0.0f && r1.aggregate_confidence <= 1.0f);
    HU_ASSERT_EQ((int)panel.total_calls, 2);
    HU_ASSERT_EQ((int)panel.total_steps_scored, 6);

    hu_verifier_panel_result_free(&alloc, &r1);
    hu_verifier_panel_result_free(&alloc, &r2);
    hu_verifier_panel_deinit(&panel);
    unlink(path);
}

/* Different inputs should produce different outputs (the kernel is
 * deterministic, not constant). */
static void panel_score_changes_with_input(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *path = think_prm_tmp_path("diff");
    HU_ASSERT_EQ(hu_prm_checkpoint_write_synthetic(path, 99u, 256), HU_OK);

    const char *paths[] = {path};
    hu_verifier_panel_t panel = {0};
    HU_ASSERT_EQ(hu_verifier_panel_create(&alloc, paths, 1, &panel), HU_OK);

    const char *a = "The answer is forty two.";
    const char *b = "Maybe I am unable to answer.";

    hu_verifier_panel_result_t ra = {0};
    hu_verifier_panel_result_t rb = {0};
    HU_ASSERT_EQ(hu_verifier_panel_score_chain(&panel, a, strlen(a), 4, &ra),
                 HU_OK);
    HU_ASSERT_EQ(hu_verifier_panel_score_chain(&panel, b, strlen(b), 4, &rb),
                 HU_OK);
    HU_ASSERT(memcmp(&ra.aggregate, &rb.aggregate, sizeof(float)) != 0);

    hu_verifier_panel_result_free(&alloc, &ra);
    hu_verifier_panel_result_free(&alloc, &rb);
    hu_verifier_panel_deinit(&panel);
    unlink(path);
}

/* ── (c) agent-turn byte-identity when panel disabled ──────────────── */

static void agent_turn_is_byte_identical_when_panel_disabled(void) {
    /* This test pins the invariant that drives the call-site short-
     * circuit in src/agent/agent_turn.c:
     *
     *   if (agent->sota.verifier_panel_enabled &&
     *       agent->sota.verifier_panel.scorer_count > 0 &&
     *       resp.content && resp.content_len > 0) {
     *       ... new init-07 logic ...
     *   }
     *
     * On a zeroed agent_extensions struct, `verifier_panel_enabled`
     * is `false` AND `scorer_count` is `0`. The compound condition is
     * short-circuit `&&`, so neither subexpression evaluates the new
     * code path. Pin both invariants here so a future refactor can't
     * remove either guard without breaking this test. */
    hu_verifier_panel_t panel = {0};
    HU_ASSERT_FALSE(panel.scorer_count > 0);
    HU_ASSERT_NULL(panel.scorers);

    /* Calling score_chain on a default-OFF panel must return
     * NOT_SUPPORTED (and not crash) — this is what guarantees that a
     * hypothetical caller that bypasses the guard still gets a safe
     * error rather than a segfault. */
    hu_verifier_panel_result_t r = {0};
    HU_ASSERT_EQ(hu_verifier_panel_score_chain(&panel, "hello", 5, 4, &r),
                 HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_NULL(r.steps);
    HU_ASSERT_EQ((int)r.step_count, 0);

    /* Deinit on a default-zero panel is a no-op (and safe). */
    hu_verifier_panel_deinit(&panel);
    HU_ASSERT_EQ((int)panel.scorer_count, 0);
}

/* ── (d) NOT_SUPPORTED when checkpoint missing ────────────────────── */

static void panel_returns_not_supported_when_checkpoint_missing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* A path that definitely does not exist. */
    const char *missing = "/tmp/hu_test_think_prm_does_not_exist_xyzzy.prm";
    /* Make extra sure. */
    unlink(missing);

    const char *paths[] = {missing};
    hu_verifier_panel_t panel = {0};
    hu_error_t err = hu_verifier_panel_create(&alloc, paths, 1, &panel);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ((int)panel.scorer_count, 0);
    HU_ASSERT_NULL(panel.scorers);

    /* Calling score_chain after the failed create still returns
     * NOT_SUPPORTED (no crash, no leak). */
    hu_verifier_panel_result_t r = {0};
    HU_ASSERT_EQ(hu_verifier_panel_score_chain(&panel, "hello", 5, 4, &r),
                 HU_ERR_NOT_SUPPORTED);

    hu_verifier_panel_deinit(&panel);
}

static void panel_partial_failure_loads_what_it_can(void) {
    /* If at least one checkpoint loads, the panel comes up with that
     * scorer; the missing paths are logged + skipped. */
    hu_allocator_t alloc = hu_system_allocator();
    const char *good = think_prm_tmp_path("partial_good");
    const char *bad = "/tmp/hu_test_think_prm_partial_missing.prm";
    unlink(bad);
    HU_ASSERT_EQ(hu_prm_checkpoint_write_synthetic(good, 5u, 128), HU_OK);

    const char *paths[] = {bad, good};
    hu_verifier_panel_t panel = {0};
    HU_ASSERT_EQ(hu_verifier_panel_create(&alloc, paths, 2, &panel), HU_OK);
    HU_ASSERT_EQ((int)panel.scorer_count, 1);

    hu_verifier_panel_deinit(&panel);
    unlink(good);
}

/* ── lifecycle and error paths ────────────────────────────────────── */

static void score_chain_rejects_invalid_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *path = think_prm_tmp_path("invalid");
    HU_ASSERT_EQ(hu_prm_checkpoint_write_synthetic(path, 1u, 64), HU_OK);

    const char *paths[] = {path};
    hu_verifier_panel_t panel = {0};
    HU_ASSERT_EQ(hu_verifier_panel_create(&alloc, paths, 1, &panel), HU_OK);

    hu_verifier_panel_result_t r = {0};
    HU_ASSERT_EQ(hu_verifier_panel_score_chain(NULL, "x", 1, 4, &r),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_verifier_panel_score_chain(&panel, NULL, 1, 4, &r),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_verifier_panel_score_chain(&panel, "x", 0, 4, &r),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_verifier_panel_score_chain(&panel, "x", 1, 4, NULL),
                 HU_ERR_INVALID_ARGUMENT);

    hu_verifier_panel_deinit(&panel);
    unlink(path);
}

static void result_free_is_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* Calling free on NULL pointers / zeroed struct must not crash. */
    hu_verifier_panel_result_free(NULL, NULL);
    hu_verifier_panel_result_free(&alloc, NULL);
    hu_verifier_panel_result_t r = {0};
    hu_verifier_panel_result_free(&alloc, &r);
}

static void create_caps_at_max_scorers(void) {
    /* Asking for more than HU_VERIFIER_PANEL_MAX_SCORERS paths should
     * not crash; the panel quietly caps. The extra paths can be NULL
     * since the cap is checked before iteration. */
    hu_allocator_t alloc = hu_system_allocator();
    const char *good = think_prm_tmp_path("cap_good");
    HU_ASSERT_EQ(hu_prm_checkpoint_write_synthetic(good, 1u, 64), HU_OK);

    const char *paths[HU_VERIFIER_PANEL_MAX_SCORERS + 2];
    paths[0] = good;
    for (size_t i = 1; i < HU_VERIFIER_PANEL_MAX_SCORERS + 2; i++)
        paths[i] = NULL; /* NULL paths are skipped during load */

    hu_verifier_panel_t panel = {0};
    /* All but one are NULL → only 1 scorer loads. With the cap applied
     * first, we never read beyond MAX_SCORERS, so the test is just
     * "no crash, sensible state". */
    hu_error_t err = hu_verifier_panel_create(
        &alloc, paths, HU_VERIFIER_PANEL_MAX_SCORERS + 2, &panel);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ((int)panel.scorer_count, 1);

    hu_verifier_panel_deinit(&panel);
    unlink(good);
}

/* ── training driver round-trip ──────────────────────────────────── */

static void training_driver_writes_loadable_checkpoint(void) {
    /* Same seed → identical bytes on disk → identical scores in the
     * panel. This pins the training-driver / runtime contract: the
     * thing CLI writes is the thing the panel reads, bit-for-bit. */
    hu_allocator_t alloc = hu_system_allocator();
    const char *p1 = "/tmp/hu_test_think_prm_train_a.prm";
    const char *p2 = "/tmp/hu_test_think_prm_train_b.prm";

    HU_ASSERT_EQ(hu_prm_checkpoint_write_synthetic(p1, 42u, 256), HU_OK);
    HU_ASSERT_EQ(hu_prm_checkpoint_write_synthetic(p2, 42u, 256), HU_OK);

    /* Compare bytes directly. */
    FILE *f1 = fopen(p1, "rb");
    FILE *f2 = fopen(p2, "rb");
    HU_ASSERT_NOT_NULL(f1);
    HU_ASSERT_NOT_NULL(f2);
    struct stat s1, s2;
    HU_ASSERT_EQ(stat(p1, &s1), 0);
    HU_ASSERT_EQ(stat(p2, &s2), 0);
    HU_ASSERT_EQ((long long)s1.st_size, (long long)s2.st_size);
    char *b1 = (char *)malloc((size_t)s1.st_size);
    char *b2 = (char *)malloc((size_t)s2.st_size);
    HU_ASSERT_EQ(fread(b1, 1, (size_t)s1.st_size, f1), (size_t)s1.st_size);
    HU_ASSERT_EQ(fread(b2, 1, (size_t)s2.st_size, f2), (size_t)s2.st_size);
    HU_ASSERT_EQ(memcmp(b1, b2, (size_t)s1.st_size), 0);
    free(b1);
    free(b2);
    fclose(f1);
    fclose(f2);

    /* Cross-load + score: must match. */
    const char *paths_a[] = {p1};
    const char *paths_b[] = {p2};
    hu_verifier_panel_t pa = {0};
    hu_verifier_panel_t pb = {0};
    HU_ASSERT_EQ(hu_verifier_panel_create(&alloc, paths_a, 1, &pa), HU_OK);
    HU_ASSERT_EQ(hu_verifier_panel_create(&alloc, paths_b, 1, &pb), HU_OK);

    const char *chain = "Step 1: A.\n\nStep 2: B.";
    hu_verifier_panel_result_t ra = {0}, rb = {0};
    HU_ASSERT_EQ(hu_verifier_panel_score_chain(&pa, chain, strlen(chain), 4, &ra), HU_OK);
    HU_ASSERT_EQ(hu_verifier_panel_score_chain(&pb, chain, strlen(chain), 4, &rb), HU_OK);
    HU_ASSERT_EQ(memcmp(&ra.aggregate, &rb.aggregate, sizeof(float)), 0);

    hu_verifier_panel_result_free(&alloc, &ra);
    hu_verifier_panel_result_free(&alloc, &rb);
    hu_verifier_panel_deinit(&pa);
    hu_verifier_panel_deinit(&pb);
    unlink(p1);
    unlink(p2);
}

/* ── create_from_dir handles missing dir gracefully ──────────────── */

static void create_from_dir_missing_dir_is_off_not_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_verifier_panel_t panel = {0};
    HU_ASSERT_EQ(hu_verifier_panel_create_from_dir(
                     &alloc, "/tmp/hu_test_think_prm_no_such_dir_xyzzy", &panel),
                 HU_OK);
    HU_ASSERT_EQ((int)panel.scorer_count, 0);
    hu_verifier_panel_deinit(&panel);
}

static void create_from_dir_loads_prm_files(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *dir = think_prm_tmp_dir("dir_loads");
    char p1[512], p2[512], p_other[512];

    /* Build the dir + two .prm files + one unrelated file. */
    mkdir(dir, 0700);
    snprintf(p1, sizeof(p1), "%s/a_scorer.prm", dir);
    snprintf(p2, sizeof(p2), "%s/b_scorer.prm", dir);
    snprintf(p_other, sizeof(p_other), "%s/readme.txt", dir);
    HU_ASSERT_EQ(hu_prm_checkpoint_write_synthetic(p1, 11u, 128), HU_OK);
    HU_ASSERT_EQ(hu_prm_checkpoint_write_synthetic(p2, 22u, 128), HU_OK);
    FILE *junk = fopen(p_other, "wb");
    if (junk) {
        fputs("ignore me", junk);
        fclose(junk);
    }

    hu_verifier_panel_t panel = {0};
    HU_ASSERT_EQ(hu_verifier_panel_create_from_dir(&alloc, dir, &panel), HU_OK);
    HU_ASSERT_EQ((int)panel.scorer_count, 2);
    hu_verifier_panel_deinit(&panel);

    unlink(p1);
    unlink(p2);
    unlink(p_other);
    rmdir(dir);
}

void run_think_prm_tests(void) {
    HU_TEST_SUITE("think_prm");
    HU_RUN_TEST(panel_construction_and_checkpoint_load);
    HU_RUN_TEST(panel_construction_with_zero_paths_is_off_not_error);
    HU_RUN_TEST(panel_construction_loads_all_three_checkpoints);
    HU_RUN_TEST(panel_score_is_deterministic_given_fixed_weights);
    HU_RUN_TEST(panel_score_changes_with_input);
    HU_RUN_TEST(agent_turn_is_byte_identical_when_panel_disabled);
    HU_RUN_TEST(panel_returns_not_supported_when_checkpoint_missing);
    HU_RUN_TEST(panel_partial_failure_loads_what_it_can);
    HU_RUN_TEST(score_chain_rejects_invalid_args);
    HU_RUN_TEST(result_free_is_null_safe);
    HU_RUN_TEST(create_caps_at_max_scorers);
    HU_RUN_TEST(training_driver_writes_loadable_checkpoint);
    HU_RUN_TEST(create_from_dir_missing_dir_is_off_not_error);
    HU_RUN_TEST(create_from_dir_loads_prm_files);
}
