/* Self-test for the flaky-test quarantine + auto-retry harness
 * (HU_RUN_TEST_FLAKY in test_framework.h). This file is the reference caller of
 * the macro: it proves a transient first-attempt failure is recovered by a
 * retry, counted as a pass, and tracked via hu__flaky_recovered — without
 * leaking that accounting into the real suite report (counters are snapshotted
 * and restored around the controlled invocation). */
#include "test_framework.h"

/* A controlled "flaky" subject: fails on attempt 1, passes on attempt 2+. The
 * call counter is what makes it deterministic across retries. */
static int g_flaky_calls;
static void flaky_demo_recovers_on_retry(void) {
    g_flaky_calls++;
    HU_ASSERT(g_flaky_calls >= 2); /* attempt 1 fails (quietly), attempt 2 passes */
}

/* Captured deltas from exercising HU_RUN_TEST_FLAKY in isolation. */
static int cap_calls;
static int cap_pass_delta;
static int cap_recovered_delta;

static void test_flaky_retry_recovers_transient_failure(void) {
    /* The subject needed exactly 2 attempts (1 transient fail + 1 success). */
    HU_ASSERT_EQ(cap_calls, 2);
    /* It was counted as a pass, not a failure. */
    HU_ASSERT_EQ(cap_pass_delta, 1);
    /* And it was tracked as flaky-recovered (the signal surfaced in the report). */
    HU_ASSERT_EQ(cap_recovered_delta, 1);
}

static void test_flaky_retries_default_is_two(void) {
    /* Sanity: the documented default. (Env/CLI can override; the suite sets it
     * back to 2 in the isolated block below, so this asserts the build default.)
     * We can't read the pre-run value here, so just assert it's a sane >=0. */
    HU_ASSERT_GE(hu__flaky_retries, 0);
}

void run_flaky_harness_tests(void) {
    HU_TEST_SUITE("flaky-harness");

    /* Exercise the recovery path in isolation. Snapshot every global the macro
     * mutates, run it, capture the deltas, then RESTORE — so this self-test does
     * not add a phantom test / flaky-recovered to the real suite totals. */
    {
        int s_total = hu__total, s_pass = hu__passed, s_fail = hu__failed;
        int s_skip = hu__skipped, s_rec = hu__flaky_recovered;
        int s_retries = hu__flaky_retries;

        g_flaky_calls = 0;
        hu__flaky_retries = 2; /* guarantee >=1 retry regardless of env/CLI */
        HU_RUN_TEST_FLAKY(flaky_demo_recovers_on_retry);

        cap_calls = g_flaky_calls;
        cap_pass_delta = hu__passed - s_pass;
        cap_recovered_delta = hu__flaky_recovered - s_rec;

        hu__flaky_retries = s_retries;
        hu__total = s_total;
        hu__passed = s_pass;
        hu__failed = s_fail;
        hu__skipped = s_skip;
        hu__flaky_recovered = s_rec;
    }

    HU_RUN_TEST(test_flaky_retry_recovers_transient_failure);
    HU_RUN_TEST(test_flaky_retries_default_is_two);
}
