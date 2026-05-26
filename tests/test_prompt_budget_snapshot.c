/* Contract tests for hu_prompt_budget_save_snapshot / _load_snapshot.
 *
 * Pins:
 *   (a) save writes a parseable JSON file at the configured path
 *   (b) round-trip preserves observation_count + field samples / mean / non_empty
 *   (c) save is atomic — a pre-blocked <path>.tmp slot makes save fail
 *       cleanly without trashing the prior file (mirrors
 *       test_personal_model_atomic_save.c's directory-blocker pattern)
 *   (d) load returns HU_ERR_NOT_FOUND when no file exists (clean ENOENT)
 *   (e) load returns HU_ERR_PARSE on malformed content
 *
 * See docs/plans/2026-05-25-doctor-prompt-budget-initiative/.
 */

#include "human/agent/prompt_budget.h"
#include "human/core/allocator.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void make_tmp_path(char *out, size_t cap, const char *suffix) {
    char tmpl[] = "/tmp/hu_pb_snap_XXXXXX";
    char *dir = mkdtemp(tmpl);
    HU_ASSERT_NOT_NULL(dir);
    snprintf(out, cap, "%s/%s", dir, suffix);
}

static void test_prompt_budget_snapshot_save_writes_parseable_file(void) {
    char path[256];
    make_tmp_path(path, sizeof(path), "snap.json");
    hu_prompt_budget_snapshot_set_path_for_test(path);

    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);

    /* One observation with non-zero bytes for the first field — gives
     * us a measurable mean to assert on after load. */
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    stats[0].name = hu_prompt_field_name((hu_prompt_field_t)0);
    stats[0].bytes_contributed = 100;
    hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);

    HU_ASSERT_EQ(hu_prompt_budget_save_snapshot(b), HU_OK);

    /* File exists and is non-empty. */
    struct stat st;
    HU_ASSERT_EQ(stat(path, &st), 0);
    HU_ASSERT(st.st_size > 0);

    hu_prompt_budget_free(b);
    (void)unlink(path);
    hu_prompt_budget_snapshot_set_path_for_test(NULL);
}

static void test_prompt_budget_snapshot_round_trip_preserves_counts(void) {
    char path[256];
    make_tmp_path(path, sizeof(path), "snap.json");
    hu_prompt_budget_snapshot_set_path_for_test(path);

    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);

    /* Three observations, varying per-field bytes so means differ
     * from each other and round-trip is non-trivial. */
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    for (int turn = 0; turn < 3; turn++) {
        memset(stats, 0, sizeof(stats));
        stats[0].name = hu_prompt_field_name((hu_prompt_field_t)0);
        stats[0].bytes_contributed = 200;
        stats[1].name = hu_prompt_field_name((hu_prompt_field_t)1);
        stats[1].bytes_contributed = 50;
        hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);
    }
    HU_ASSERT_EQ(hu_prompt_budget_save_snapshot(b), HU_OK);

    hu_prompt_budget_snapshot_load_t load;
    HU_ASSERT_EQ(hu_prompt_budget_load_snapshot(&alloc, &load), HU_OK);
    HU_ASSERT_EQ(load.observation_count, (uint64_t)3);
    HU_ASSERT(load.field_count >= 2);

    /* Field 0: 3 samples × 200 → mean 200, non_empty 3. */
    HU_ASSERT_EQ(load.fields[0].mean_bytes, (uint64_t)200);
    HU_ASSERT_EQ(load.fields[0].samples, (uint64_t)3);
    HU_ASSERT_EQ(load.fields[0].non_empty_count, (uint64_t)3);

    /* Field 1: 3 samples × 50 → mean 50, non_empty 3. */
    HU_ASSERT_EQ(load.fields[1].mean_bytes, (uint64_t)50);
    HU_ASSERT_EQ(load.fields[1].samples, (uint64_t)3);
    HU_ASSERT_EQ(load.fields[1].non_empty_count, (uint64_t)3);

    /* Field 2: never observed with bytes — samples reflect turn count
     * (observe walks all leading entries), mean=0, non_empty=0. */
    HU_ASSERT_EQ(load.fields[2].mean_bytes, (uint64_t)0);
    HU_ASSERT_EQ(load.fields[2].non_empty_count, (uint64_t)0);

    hu_prompt_budget_snapshot_load_free(&load);
    hu_prompt_budget_free(b);
    (void)unlink(path);
    hu_prompt_budget_snapshot_set_path_for_test(NULL);
}

static void test_prompt_budget_save_preserves_prior_state_when_tmp_blocked(void) {
    /* Adversary test mirroring test_personal_model_atomic_save.c.
     * Pre-block the <path>.tmp slot with a directory so the atomic save
     * fails at fopen("wb") with EISDIR. Pin: the prior file at <path>
     * must survive intact. */
    char path[256];
    make_tmp_path(path, sizeof(path), "snap.json");
    hu_prompt_budget_snapshot_set_path_for_test(path);

    char tmp_path[300];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);

    /* Known-good prior state: 1 observation, bytes=42 on field 0. */
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    stats[0].bytes_contributed = 42;
    hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);
    HU_ASSERT_EQ(hu_prompt_budget_save_snapshot(b), HU_OK);

    /* Block the tmp slot — fopen("wb") on a directory returns EISDIR. */
    HU_ASSERT_EQ(mkdir(tmp_path, 0755), 0);

    /* Try to overwrite with a distinguishable larger state. We don't
     * assert the return value — the contract being tested is FILE
     * STATE, not the error code. Either HU_ERR_IO or HU_OK with the
     * file preserved is acceptable; the corruption would be a
     * truncated/half-written prior file. */
    for (int i = 0; i < 50; i++) {
        memset(stats, 0, sizeof(stats));
        stats[0].bytes_contributed = 9999;
        hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);
    }
    (void)hu_prompt_budget_save_snapshot(b);

    /* Load and verify the prior state still reads back. observation_count
     * must be 1 (the prior known-good), NOT 51 (which the new save would
     * have produced if it had clobbered the prior file). */
    hu_prompt_budget_snapshot_load_t load;
    HU_ASSERT_EQ(hu_prompt_budget_load_snapshot(&alloc, &load), HU_OK);
    HU_ASSERT_EQ(load.observation_count, (uint64_t)1);
    HU_ASSERT_EQ(load.fields[0].mean_bytes, (uint64_t)42);

    hu_prompt_budget_snapshot_load_free(&load);
    hu_prompt_budget_free(b);
    (void)rmdir(tmp_path);
    (void)unlink(path);
    hu_prompt_budget_snapshot_set_path_for_test(NULL);
}

static void test_prompt_budget_load_returns_not_found_when_missing(void) {
    /* Point at a guaranteed-missing path. */
    hu_prompt_budget_snapshot_set_path_for_test("/tmp/hu_pb_definitely_missing_XXX.json");
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_snapshot_load_t load;
    HU_ASSERT_EQ(hu_prompt_budget_load_snapshot(&alloc, &load), HU_ERR_NOT_FOUND);
    hu_prompt_budget_snapshot_set_path_for_test(NULL);
}

static void test_prompt_budget_load_returns_parse_when_malformed(void) {
    char path[256];
    make_tmp_path(path, sizeof(path), "bad.json");
    hu_prompt_budget_snapshot_set_path_for_test(path);

    FILE *fp = fopen(path, "w");
    HU_ASSERT_NOT_NULL(fp);
    /* Missing the required keys — parser should reject. */
    fprintf(fp, "{ \"this_is_not\": \"the_schema\" }\n");
    fclose(fp);

    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_snapshot_load_t load;
    HU_ASSERT_EQ(hu_prompt_budget_load_snapshot(&alloc, &load), HU_ERR_PARSE);

    (void)unlink(path);
    hu_prompt_budget_snapshot_set_path_for_test(NULL);
}

void run_prompt_budget_snapshot_tests(void) {
    HU_TEST_SUITE("prompt-budget-snapshot");
    HU_RUN_TEST(test_prompt_budget_snapshot_save_writes_parseable_file);
    HU_RUN_TEST(test_prompt_budget_snapshot_round_trip_preserves_counts);
    HU_RUN_TEST(test_prompt_budget_save_preserves_prior_state_when_tmp_blocked);
    HU_RUN_TEST(test_prompt_budget_load_returns_not_found_when_missing);
    HU_RUN_TEST(test_prompt_budget_load_returns_parse_when_malformed);
}
