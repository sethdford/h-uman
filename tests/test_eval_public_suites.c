/* Init #14 — Public benchmark suite regression gates.
 *
 * Validates that each of the five committed smoke fixtures
 * (longmemeval, locomo, knowu, empa, proagentbench) loads, scores
 * against a deterministic mock provider, and clears its checked-in
 * regression floor.  This is the CI smoke gate from
 * docs/plans/2026-05-11-init-14-public-benchmarks.md §Test plan.
 *
 * All fixtures use synthetic personas (user_a, agent_a, example.com).
 * The privacy scanner is exercised both on its own and as part of the
 * end-to-end loader contract.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/eval.h"
#include "human/eval_public_suites.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

static void public_benchmark_locked_job_kind_ordinal(void) {
    /* The locked allocation in docs/plans/2026-05-11-sota-2026-massive-team-program.md
     * §"hu_job_kind_t enum allocation" reserves ordinal 7 for the benchmark
     * job kind. This is cross-initiative-locked: future scheduler
     * unification must keep this slot. */
    HU_ASSERT_EQ(HU_JOB_KIND_BENCHMARK, 7);
}

static void public_benchmark_from_string_round_trip(void) {
    static const char *names[] = {"longmemeval", "locomo", "knowu", "empa", "proagentbench"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        hu_public_benchmark_t b;
        HU_ASSERT_TRUE(hu_public_benchmark_from_string(names[i], &b));
        HU_ASSERT_STR_EQ(hu_public_benchmark_name(b), names[i]);
        HU_ASSERT_NOT_NULL(hu_public_benchmark_fixture_path(b));
        HU_ASSERT_TRUE(hu_public_benchmark_floor(b) > 0.0);
        HU_ASSERT_TRUE(hu_public_benchmark_floor(b) <= 1.0);
    }
}

static void public_benchmark_from_string_rejects_unknown(void) {
    hu_public_benchmark_t b = (hu_public_benchmark_t)999;
    HU_ASSERT_FALSE(hu_public_benchmark_from_string("not-a-benchmark", &b));
    HU_ASSERT_FALSE(hu_public_benchmark_from_string(NULL, &b));
    HU_ASSERT_FALSE(hu_public_benchmark_from_string("longmemeval", NULL));
}

static void public_benchmark_count_is_five(void) {
    HU_ASSERT_EQ((int)HU_PUBLIC_BENCHMARK_COUNT, 5);
}

static void public_benchmark_check_fixture_privacy_accepts_synthetic(void) {
    static const char clean[] =
        "{\"user\":\"user_a\",\"email\":\"agent_a@example.com\",\"goal\":\"project_alpha\"}";
    HU_ASSERT_EQ(hu_public_benchmark_check_fixture_privacy(clean, sizeof(clean) - 1), HU_OK);
}

static void public_benchmark_check_fixture_privacy_rejects_ssn(void) {
    static const char ssn[] = "{\"ssn\":\"123-45-6789\"}";
    HU_ASSERT_NEQ(hu_public_benchmark_check_fixture_privacy(ssn, sizeof(ssn) - 1), HU_OK);
}

static void public_benchmark_check_fixture_privacy_rejects_real_email(void) {
    static const char real[] = "{\"email\":\"alice@gmail.com\"}";
    HU_ASSERT_NEQ(hu_public_benchmark_check_fixture_privacy(real, sizeof(real) - 1), HU_OK);
}

static void public_benchmark_check_fixture_privacy_rejects_phone_run(void) {
    static const char phone[] = "{\"phone\":\"5551234567\"}";
    HU_ASSERT_NEQ(hu_public_benchmark_check_fixture_privacy(phone, sizeof(phone) - 1), HU_OK);
}

static void public_benchmark_smoke_runs_for_one(hu_public_benchmark_t b, const char *expected_name) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_public_benchmark_result_t pbr;
    memset(&pbr, 0, sizeof(pbr));
    hu_error_t err = hu_public_benchmark_run_smoke(&alloc, b, NULL, "mock", 4, &pbr);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(pbr.name);
    HU_ASSERT_STR_EQ(pbr.name, expected_name);
    HU_ASSERT_GE((int)pbr.tasks_run, 3);
    HU_ASSERT_LE((int)pbr.tasks_run, 10);
    HU_ASSERT_TRUE(pbr.passed_floor);
    HU_ASSERT_GE((int)(pbr.score * 100.0), (int)(pbr.floor * 100.0));
    fprintf(stderr, "  [bench] %-14s score=%.4f floor=%.4f tasks=%zu/%zu elapsed=%lld ms\n",
            pbr.name, pbr.score, pbr.floor, pbr.tasks_passed, pbr.tasks_run,
            (long long)pbr.elapsed_ms);
    hu_public_benchmark_result_free(&alloc, &pbr);
}

static void public_benchmark_longmemeval_smoke_passes_floor(void) {
    public_benchmark_smoke_runs_for_one(HU_PUBLIC_BENCHMARK_LONGMEMEVAL, "longmemeval");
}

static void public_benchmark_locomo_smoke_passes_floor(void) {
    public_benchmark_smoke_runs_for_one(HU_PUBLIC_BENCHMARK_LOCOMO, "locomo");
}

static void public_benchmark_knowu_smoke_passes_floor(void) {
    public_benchmark_smoke_runs_for_one(HU_PUBLIC_BENCHMARK_KNOWU, "knowu");
}

static void public_benchmark_empa_smoke_passes_floor(void) {
    public_benchmark_smoke_runs_for_one(HU_PUBLIC_BENCHMARK_EMPA, "empa");
}

static void public_benchmark_proagentbench_smoke_passes_floor(void) {
    public_benchmark_smoke_runs_for_one(HU_PUBLIC_BENCHMARK_PROAGENTBENCH, "proagentbench");
}

static void public_benchmark_result_to_json_emits_required_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_public_benchmark_result_t pbr;
    memset(&pbr, 0, sizeof(pbr));
    HU_ASSERT_EQ(hu_public_benchmark_run_smoke(&alloc, HU_PUBLIC_BENCHMARK_LONGMEMEVAL, NULL,
                                                "mock", 4, &pbr),
                 HU_OK);
    char *json = NULL;
    size_t json_len = 0;
    HU_ASSERT_EQ(hu_public_benchmark_result_to_json(&alloc, &pbr, &json, &json_len), HU_OK);
    HU_ASSERT_NOT_NULL(json);
    HU_ASSERT_STR_CONTAINS(json, "\"benchmark\":\"longmemeval\"");
    HU_ASSERT_STR_CONTAINS(json, "\"tasks_run\":");
    HU_ASSERT_STR_CONTAINS(json, "\"tasks_passed\":");
    HU_ASSERT_STR_CONTAINS(json, "\"score\":");
    HU_ASSERT_STR_CONTAINS(json, "\"floor\":");
    HU_ASSERT_STR_CONTAINS(json, "\"passed_floor\":true");
    HU_ASSERT_STR_CONTAINS(json, "\"elapsed_ms\":");
    alloc.free(alloc.ctx, json, json_len + 1);
    hu_public_benchmark_result_free(&alloc, &pbr);
}

static void public_benchmark_publish_results_atomic_round_trip(void) {
    /* mkstemp picks an unused filename; remove the empty file so the
     * atomic writer can perform its own rename(tmp, path). */
    char tmpl[] = "/tmp/hu_pbench_XXXXXX";
    int fd = mkstemp(tmpl);
    HU_ASSERT(fd >= 0);
    close(fd);
    /* mkstemp creates the file; remove so our atomic writer can rename. */
    (void)remove(tmpl);

    static const char payload[] = "{\"benchmark\":\"unit\",\"score\":0.85}";
    HU_ASSERT_EQ(hu_public_benchmark_publish_results(tmpl, payload, sizeof(payload) - 1), HU_OK);

    FILE *f = fopen(tmpl, "rb");
    HU_ASSERT_NOT_NULL(f);
    char buf[128];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    HU_ASSERT_STR_EQ(buf, payload);

    /* Confirm the tmp sibling did not leak. */
    char tmp_sibling[256];
    snprintf(tmp_sibling, sizeof(tmp_sibling), "%s.tmp", tmpl);
    struct stat st;
    HU_ASSERT_NEQ(stat(tmp_sibling, &st), 0);
    (void)remove(tmpl);
}

static void public_benchmark_publish_results_rejects_bad_args(void) {
    HU_ASSERT_NEQ(hu_public_benchmark_publish_results(NULL, "x", 1), HU_OK);
    HU_ASSERT_NEQ(hu_public_benchmark_publish_results("/tmp/x", NULL, 0), HU_OK);
}

static void public_benchmark_run_smoke_rejects_invalid_enum(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_public_benchmark_result_t pbr;
    memset(&pbr, 0, sizeof(pbr));
    HU_ASSERT_NEQ(hu_public_benchmark_run_smoke(&alloc, (hu_public_benchmark_t)999, NULL, "mock",
                                                 4, &pbr),
                  HU_OK);
}

static void public_benchmark_fixture_path_is_under_tests_fixtures_benchmarks(void) {
    /* Defense in depth: ensures the loader never silently grows a code path
     * that reads from outside the committed fixtures tree. */
    for (int i = 0; i < (int)HU_PUBLIC_BENCHMARK_COUNT; i++) {
        const char *p = hu_public_benchmark_fixture_path((hu_public_benchmark_t)i);
        HU_ASSERT_NOT_NULL(p);
        HU_ASSERT_STR_CONTAINS(p, "tests/fixtures/benchmarks/");
        HU_ASSERT_STR_CONTAINS(p, "/smoke.json");
    }
}

static void public_benchmark_smoke_walltime_well_under_30s(void) {
    /* The full smoke fan-out across all five benchmarks must comfortably
     * fit inside the 30 s CI budget asserted by init-14 §Test plan #9. */
    hu_allocator_t alloc = hu_system_allocator();
    int64_t total_ms = 0;
    for (int i = 0; i < (int)HU_PUBLIC_BENCHMARK_COUNT; i++) {
        hu_public_benchmark_result_t pbr;
        memset(&pbr, 0, sizeof(pbr));
        HU_ASSERT_EQ(hu_public_benchmark_run_smoke(&alloc, (hu_public_benchmark_t)i, NULL, "mock",
                                                    4, &pbr),
                     HU_OK);
        total_ms += pbr.elapsed_ms;
        hu_public_benchmark_result_free(&alloc, &pbr);
    }
    /* Generous ceiling: mock-provider smoke for 5 benchmarks × ≤ 10 tasks
     * each is microseconds in practice. 5000 ms guards against future
     * regressions that introduce real I/O or sleeps. */
    HU_ASSERT_LT(total_ms, 5000);
}

void run_eval_public_suites_tests(void) {
    HU_TEST_SUITE("public-benchmarks");
    HU_RUN_TEST(public_benchmark_locked_job_kind_ordinal);
    HU_RUN_TEST(public_benchmark_from_string_round_trip);
    HU_RUN_TEST(public_benchmark_from_string_rejects_unknown);
    HU_RUN_TEST(public_benchmark_count_is_five);
    HU_RUN_TEST(public_benchmark_check_fixture_privacy_accepts_synthetic);
    HU_RUN_TEST(public_benchmark_check_fixture_privacy_rejects_ssn);
    HU_RUN_TEST(public_benchmark_check_fixture_privacy_rejects_real_email);
    HU_RUN_TEST(public_benchmark_check_fixture_privacy_rejects_phone_run);
    HU_RUN_TEST(public_benchmark_longmemeval_smoke_passes_floor);
    HU_RUN_TEST(public_benchmark_locomo_smoke_passes_floor);
    HU_RUN_TEST(public_benchmark_knowu_smoke_passes_floor);
    HU_RUN_TEST(public_benchmark_empa_smoke_passes_floor);
    HU_RUN_TEST(public_benchmark_proagentbench_smoke_passes_floor);
    HU_RUN_TEST(public_benchmark_result_to_json_emits_required_fields);
    HU_RUN_TEST(public_benchmark_publish_results_atomic_round_trip);
    HU_RUN_TEST(public_benchmark_publish_results_rejects_bad_args);
    HU_RUN_TEST(public_benchmark_run_smoke_rejects_invalid_enum);
    HU_RUN_TEST(public_benchmark_fixture_path_is_under_tests_fixtures_benchmarks);
    HU_RUN_TEST(public_benchmark_smoke_walltime_well_under_30s);
}
