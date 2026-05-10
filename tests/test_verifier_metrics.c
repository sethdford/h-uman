/* Verifier metrics persistence + doctor surface tests.
 *
 * The W4 response verifier ticks counters on hu_agent_t every turn (FIX 2).
 * Those counters live in-process and reset on restart unless the daemon
 * flushes them. This suite proves the persistence path round-trips and the
 * doctor reports the right state for every reasonable input. */

#include "human/agent/verifier_metrics.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/doctor.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Pin HOME to a tmp dir per test so the metrics file doesn't collide with
 * the developer's real ~/.human or with parallel test runs. The ASan-clean
 * mkstemp + readback pattern matches what test_imessage.c and friends use. */
typedef struct {
    char tmp_home[256];
    char *prev_home;
} tmp_home_t;

static void tmp_home_setup(tmp_home_t *th) {
    snprintf(th->tmp_home, sizeof(th->tmp_home), "/tmp/hu_verifier_metrics_test_%d_%lu",
             (int)getpid(), (unsigned long)time(NULL));
    (void)mkdir(th->tmp_home, 0700);
    const char *prev = getenv("HOME");
    th->prev_home = prev ? strdup(prev) : NULL;
    setenv("HOME", th->tmp_home, 1);
}

static void tmp_home_teardown(tmp_home_t *th) {
    /* Best-effort cleanup; even if we can't unlink everything the next run's
     * timestamped tmp_home is unique. */
    char path[512];
    snprintf(path, sizeof(path), "%s/.human/verifier_metrics.json", th->tmp_home);
    (void)unlink(path);
    snprintf(path, sizeof(path), "%s/.human", th->tmp_home);
    (void)rmdir(path);
    (void)rmdir(th->tmp_home);
    if (th->prev_home) {
        setenv("HOME", th->prev_home, 1);
        free(th->prev_home);
    } else {
        unsetenv("HOME");
    }
}

static void test_verifier_metrics_path_uses_home(void) {
    tmp_home_t th;
    tmp_home_setup(&th);
    char path[512];
    HU_ASSERT_TRUE(hu_verifier_metrics_path(path, sizeof(path)));
    HU_ASSERT_TRUE(strstr(path, "/.human/verifier_metrics.json") != NULL);
    HU_ASSERT_TRUE(strstr(path, th.tmp_home) != NULL);
    tmp_home_teardown(&th);
}

static void test_verifier_metrics_path_rejects_unset_home(void) {
    /* Save HOME, unset it, expect false. */
    const char *prev = getenv("HOME");
    char *saved = prev ? strdup(prev) : NULL;
    unsetenv("HOME");
    char path[512];
    HU_ASSERT_FALSE(hu_verifier_metrics_path(path, sizeof(path)));
    if (saved) {
        setenv("HOME", saved, 1);
        free(saved);
    }
}

static void test_verifier_metrics_load_returns_not_found_initially(void) {
    tmp_home_t th;
    tmp_home_setup(&th);
    hu_verifier_metrics_t m;
    HU_ASSERT_EQ(hu_verifier_metrics_load(&m), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ((unsigned long long)m.total_runs, 0ULL);
    HU_ASSERT_EQ((unsigned long long)m.total_claims_extracted, 0ULL);
    HU_ASSERT_EQ((unsigned long long)m.total_claims_flagged, 0ULL);
    HU_ASSERT_EQ((long long)m.last_update_epoch, 0LL);
    tmp_home_teardown(&th);
}

static void test_verifier_metrics_save_then_load_roundtrip(void) {
    tmp_home_t th;
    tmp_home_setup(&th);
    hu_verifier_metrics_t in = {
        .total_runs = 42,
        .total_claims_extracted = 200,
        .total_claims_flagged = 17,
        .last_update_epoch = 0,
    };
    HU_ASSERT_EQ(hu_verifier_metrics_save(&in), HU_OK);
    /* save() stamps last_update_epoch with current time. */
    HU_ASSERT_GT((long long)in.last_update_epoch, 0LL);

    hu_verifier_metrics_t out;
    HU_ASSERT_EQ(hu_verifier_metrics_load(&out), HU_OK);
    HU_ASSERT_EQ((unsigned long long)out.total_runs, 42ULL);
    HU_ASSERT_EQ((unsigned long long)out.total_claims_extracted, 200ULL);
    HU_ASSERT_EQ((unsigned long long)out.total_claims_flagged, 17ULL);
    HU_ASSERT_EQ((long long)out.last_update_epoch, (long long)in.last_update_epoch);
    tmp_home_teardown(&th);
}

static void test_verifier_metrics_flagged_rate_zero_when_no_claims(void) {
    hu_verifier_metrics_t m = {.total_runs = 1, .total_claims_extracted = 0,
                                .total_claims_flagged = 0, .last_update_epoch = 0};
    HU_ASSERT_TRUE(hu_verifier_metrics_flagged_rate(&m) == 0.0);
}

static void test_verifier_metrics_flagged_rate_division(void) {
    hu_verifier_metrics_t m = {.total_runs = 0, .total_claims_extracted = 200,
                                .total_claims_flagged = 50, .last_update_epoch = 0};
    HU_ASSERT_TRUE(hu_verifier_metrics_flagged_rate(&m) == 0.25);
}

/* Adversarial: garbage / partial files must not crash. The parser falls
 * through field-by-field; missing fields stay at 0 (matches "fresh" semantic). */
static void test_verifier_metrics_load_partial_file(void) {
    tmp_home_t th;
    tmp_home_setup(&th);
    char path[512];
    HU_ASSERT_TRUE(hu_verifier_metrics_path(path, sizeof(path)));
    /* Mkdir parent and write a JSON file with only one of the fields. */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.human", th.tmp_home);
    (void)mkdir(dir, 0700);
    FILE *f = fopen(path, "w");
    HU_ASSERT_NOT_NULL(f);
    fprintf(f, "{\n  \"total_runs\": 99\n}\n");
    fclose(f);

    hu_verifier_metrics_t out;
    HU_ASSERT_EQ(hu_verifier_metrics_load(&out), HU_OK);
    HU_ASSERT_EQ((unsigned long long)out.total_runs, 99ULL);
    HU_ASSERT_EQ((unsigned long long)out.total_claims_extracted, 0ULL);
    HU_ASSERT_EQ((unsigned long long)out.total_claims_flagged, 0ULL);
    HU_ASSERT_EQ((long long)out.last_update_epoch, 0LL);
    tmp_home_teardown(&th);
}

/* Doctor wire — when no file exists, doctor reports a single WARN about
 * the missing first flush. This is the "daemon just started" path. */
static void test_doctor_check_verifier_no_file(void) {
    tmp_home_t th;
    tmp_home_setup(&th);
    hu_allocator_t alloc = hu_system_allocator();
    size_t cap = 8;
    hu_diag_item_t *items =
        (hu_diag_item_t *)alloc.alloc(alloc.ctx, sizeof(hu_diag_item_t) * cap);
    HU_ASSERT_NOT_NULL(items);
    size_t count = 0;
    HU_ASSERT_EQ(
        hu_doctor_check_verifier(&alloc, (int64_t)time(NULL), 300, 0.10, &items, &count, &cap),
        HU_OK);
    HU_ASSERT_EQ(count, (size_t)1);
    HU_ASSERT_EQ(items[0].severity, HU_DIAG_WARN);
    HU_ASSERT_TRUE(strstr(items[0].message, "no metrics yet") != NULL);
    for (size_t i = 0; i < count; i++) {
        if (items[i].category)
            alloc.free(alloc.ctx, (void *)items[i].category, strlen(items[i].category) + 1);
        if (items[i].message)
            alloc.free(alloc.ctx, (void *)items[i].message, strlen(items[i].message) + 1);
    }
    alloc.free(alloc.ctx, items, cap * sizeof(hu_diag_item_t));
    tmp_home_teardown(&th);
}

/* Doctor wire — fresh metrics produce OK lines for the count + heartbeat,
 * with no flagged-rate WARN when the rate is below threshold. */
static void test_doctor_check_verifier_fresh_low_flagged_rate(void) {
    tmp_home_t th;
    tmp_home_setup(&th);
    hu_verifier_metrics_t m = {.total_runs = 100, .total_claims_extracted = 500,
                                .total_claims_flagged = 5, .last_update_epoch = 0};
    HU_ASSERT_EQ(hu_verifier_metrics_save(&m), HU_OK);

    hu_allocator_t alloc = hu_system_allocator();
    size_t cap = 8;
    hu_diag_item_t *items =
        (hu_diag_item_t *)alloc.alloc(alloc.ctx, sizeof(hu_diag_item_t) * cap);
    HU_ASSERT_NOT_NULL(items);
    size_t count = 0;
    HU_ASSERT_EQ(
        hu_doctor_check_verifier(&alloc, (int64_t)time(NULL), 300, 0.10, &items, &count, &cap),
        HU_OK);
    /* 1) counts line, 2) fresh heartbeat. No flagged WARN since 5/500 = 1%. */
    HU_ASSERT_EQ(count, (size_t)2);
    HU_ASSERT_EQ(items[0].severity, HU_DIAG_OK);
    HU_ASSERT_TRUE(strstr(items[0].message, "100 turns") != NULL);
    HU_ASSERT_TRUE(strstr(items[0].message, "500 claims") != NULL);
    HU_ASSERT_EQ(items[1].severity, HU_DIAG_OK);
    HU_ASSERT_TRUE(strstr(items[1].message, "fresh") != NULL);
    for (size_t i = 0; i < count; i++) {
        if (items[i].category)
            alloc.free(alloc.ctx, (void *)items[i].category, strlen(items[i].category) + 1);
        if (items[i].message)
            alloc.free(alloc.ctx, (void *)items[i].message, strlen(items[i].message) + 1);
    }
    alloc.free(alloc.ctx, items, cap * sizeof(hu_diag_item_t));
    tmp_home_teardown(&th);
}

/* Doctor wire — flagged rate over the threshold WARNs with both the rate
 * and the threshold so ops can act. */
static void test_doctor_check_verifier_high_flagged_rate(void) {
    tmp_home_t th;
    tmp_home_setup(&th);
    hu_verifier_metrics_t m = {.total_runs = 100, .total_claims_extracted = 500,
                                .total_claims_flagged = 100, .last_update_epoch = 0};
    HU_ASSERT_EQ(hu_verifier_metrics_save(&m), HU_OK);

    hu_allocator_t alloc = hu_system_allocator();
    size_t cap = 8;
    hu_diag_item_t *items =
        (hu_diag_item_t *)alloc.alloc(alloc.ctx, sizeof(hu_diag_item_t) * cap);
    HU_ASSERT_NOT_NULL(items);
    size_t count = 0;
    HU_ASSERT_EQ(
        hu_doctor_check_verifier(&alloc, (int64_t)time(NULL), 300, 0.10, &items, &count, &cap),
        HU_OK);
    /* counts + fresh heartbeat + flagged WARN (100/500 = 20% >= 10%). */
    HU_ASSERT_EQ(count, (size_t)3);
    HU_ASSERT_EQ(items[2].severity, HU_DIAG_WARN);
    HU_ASSERT_TRUE(strstr(items[2].message, "HIGH") != NULL);
    HU_ASSERT_TRUE(strstr(items[2].message, "20.0") != NULL);
    for (size_t i = 0; i < count; i++) {
        if (items[i].category)
            alloc.free(alloc.ctx, (void *)items[i].category, strlen(items[i].category) + 1);
        if (items[i].message)
            alloc.free(alloc.ctx, (void *)items[i].message, strlen(items[i].message) + 1);
    }
    alloc.free(alloc.ctx, items, cap * sizeof(hu_diag_item_t));
    tmp_home_teardown(&th);
}

/* Doctor wire — when last_update_epoch is older than the staleness threshold,
 * heartbeat WARNs. This is the "daemon offline / wedged" path. */
static void test_doctor_check_verifier_stale_heartbeat(void) {
    tmp_home_t th;
    tmp_home_setup(&th);
    hu_verifier_metrics_t m = {.total_runs = 1, .total_claims_extracted = 0,
                                .total_claims_flagged = 0, .last_update_epoch = 0};
    HU_ASSERT_EQ(hu_verifier_metrics_save(&m), HU_OK);

    hu_allocator_t alloc = hu_system_allocator();
    size_t cap = 8;
    hu_diag_item_t *items =
        (hu_diag_item_t *)alloc.alloc(alloc.ctx, sizeof(hu_diag_item_t) * cap);
    HU_ASSERT_NOT_NULL(items);
    size_t count = 0;
    /* Pretend now is 1000s in the future relative to the just-saved file. */
    int64_t pseudo_now = (int64_t)time(NULL) + 1000;
    HU_ASSERT_EQ(
        hu_doctor_check_verifier(&alloc, pseudo_now, 300, 0.10, &items, &count, &cap), HU_OK);
    /* counts + STALE heartbeat. No flagged WARN (zero claims extracted). */
    HU_ASSERT_EQ(count, (size_t)2);
    HU_ASSERT_EQ(items[1].severity, HU_DIAG_WARN);
    HU_ASSERT_TRUE(strstr(items[1].message, "STALE") != NULL);
    for (size_t i = 0; i < count; i++) {
        if (items[i].category)
            alloc.free(alloc.ctx, (void *)items[i].category, strlen(items[i].category) + 1);
        if (items[i].message)
            alloc.free(alloc.ctx, (void *)items[i].message, strlen(items[i].message) + 1);
    }
    alloc.free(alloc.ctx, items, cap * sizeof(hu_diag_item_t));
    tmp_home_teardown(&th);
}

void run_verifier_metrics_tests(void) {
    HU_TEST_SUITE("VerifierMetrics");
    HU_RUN_TEST(test_verifier_metrics_path_uses_home);
    HU_RUN_TEST(test_verifier_metrics_path_rejects_unset_home);
    HU_RUN_TEST(test_verifier_metrics_load_returns_not_found_initially);
    HU_RUN_TEST(test_verifier_metrics_save_then_load_roundtrip);
    HU_RUN_TEST(test_verifier_metrics_flagged_rate_zero_when_no_claims);
    HU_RUN_TEST(test_verifier_metrics_flagged_rate_division);
    HU_RUN_TEST(test_verifier_metrics_load_partial_file);
    HU_RUN_TEST(test_doctor_check_verifier_no_file);
    HU_RUN_TEST(test_doctor_check_verifier_fresh_low_flagged_rate);
    HU_RUN_TEST(test_doctor_check_verifier_high_flagged_rate);
    HU_RUN_TEST(test_doctor_check_verifier_stale_heartbeat);
}
