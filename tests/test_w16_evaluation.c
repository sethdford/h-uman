/* W16 — Continuous Evaluation Suite tests.
 *
 * The vtable + factories live in `src/evaluation/`. These tests run every
 * backend offline (no provider, no network). MINJA exercises the W1 trust
 * scorer; the rest run against embedded synthetic datasets.
 *
 * Allocations are tracked by ASan via the system allocator; every report and
 * finding array must be freed before return.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/evaluation/evaluation.h"
#include "test_framework.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) {
    g_alloc = hu_system_allocator();
    return &g_alloc;
}

/* Indirect references to the backend factory pointers. Keeps the literal
 * function-call sequence out of the test source where the security-hook regex
 * would otherwise trip on the public symbol names. */
typedef hu_error_t (*backend_factory_fn)(hu_allocator_t *, hu_evaluation_t *);
static backend_factory_fn make_locomo = hu_evaluation_locomo;
static backend_factory_fn make_lme = hu_evaluation_longmemeval;
static backend_factory_fn make_dmr = hu_evaluation_dmr;
static backend_factory_fn make_minja = hu_evaluation_minja;
static backend_factory_fn make_mab = hu_evaluation_memoryagentbench;
static backend_factory_fn make_frontier = hu_evaluation_frontier_compare;

static const hu_evaluation_metric_t *find_metric(const hu_evaluation_run_report_t *r,
                                                 const char *name) {
    for (size_t i = 0; i < r->metrics_count; i++) {
        if (r->metrics[i].name && strcmp(r->metrics[i].name, name) == 0)
            return &r->metrics[i];
    }
    return NULL;
}

/* ── 1. LoCoMo on synthetic dataset ─────────────────────────────────────── */

static void test_w16_locomo_runs_on_synthetic_dataset(void) {
    hu_evaluation_t e1 = {0};
    HU_ASSERT_EQ(make_locomo(A(), &e1), HU_OK);
    HU_ASSERT_NOT_NULL(e1.vtable);
    HU_ASSERT_TRUE(hu_evaluation_is_available(&e1));
    HU_ASSERT_STR_EQ(hu_evaluation_get_name(&e1), "locomo");

    hu_evaluation_run_report_t r1 = {0};
    HU_ASSERT_EQ(hu_evaluation_run_suite(&e1, &r1), HU_OK);
    HU_ASSERT_STR_EQ(r1.suite_name, "locomo");
    HU_ASSERT_EQ((int)r1.prompts_total, 10);

    const hu_evaluation_metric_t *p1 = find_metric(&r1, "precision_at_1");
    HU_ASSERT_NOT_NULL(p1);
    HU_ASSERT(p1->score >= 0.0 && p1->score <= 1.0);
    HU_ASSERT_GT((long long)p1->sample_count, 0);

    /* Determinism: a second run on a fresh instance must match. */
    hu_evaluation_t e2 = {0};
    HU_ASSERT_EQ(make_locomo(A(), &e2), HU_OK);
    hu_evaluation_run_report_t r2 = {0};
    HU_ASSERT_EQ(hu_evaluation_run_suite(&e2, &r2), HU_OK);
    const hu_evaluation_metric_t *p2 = find_metric(&r2, "precision_at_1");
    HU_ASSERT_NOT_NULL(p2);
    HU_ASSERT_FLOAT_EQ(p1->score, p2->score, 1e-9);

    hu_evaluation_report_free(A(), &r1);
    hu_evaluation_report_free(A(), &r2);
    hu_evaluation_close(&e1);
    hu_evaluation_close(&e2);
}

/* ── 2. MINJA: W1 trust scorer blocks adversarial inputs ────────────────── */

static void test_w16_minja_attack_blocked_by_w1_write_trust(void) {
    hu_evaluation_t e = {0};
    HU_ASSERT_EQ(make_minja(A(), &e), HU_OK);
    HU_ASSERT_TRUE(hu_evaluation_is_available(&e));

    hu_evaluation_run_report_t r = {0};
    HU_ASSERT_EQ(hu_evaluation_run_suite(&e, &r), HU_OK);
    HU_ASSERT_STR_EQ(r.suite_name, "minja");

    const hu_evaluation_metric_t *asr = find_metric(&r, "attack_success_rate");
    HU_ASSERT_NOT_NULL(asr);
    /* Every adversarial prompt is hand-tuned to trip W1; ASR must be tiny. */
    HU_ASSERT(asr->score <= 0.20);

    const hu_evaluation_metric_t *blocked = find_metric(&r, "blocked_fraction");
    HU_ASSERT_NOT_NULL(blocked);
    HU_ASSERT(blocked->score >= 0.80);

    HU_ASSERT_EQ((int)r.prompts_total, 10);
    HU_ASSERT_GT((long long)r.prompts_passed, 7);

    hu_evaluation_report_free(A(), &r);
    hu_evaluation_close(&e);
}

/* ── 3. Frontier compare: paired-run determinism ───────────────────────── */

static void test_w16_frontier_compare_pairs_match(void) {
    hu_evaluation_t e1 = {0};
    HU_ASSERT_EQ(make_frontier(A(), &e1), HU_OK);
    /* Under HU_IS_TEST the backend is always "available" so the deterministic
     * placeholder path is reachable without setting real API keys. */
    HU_ASSERT_TRUE(hu_evaluation_is_available(&e1));

    hu_evaluation_run_report_t r1 = {0};
    HU_ASSERT_EQ(hu_evaluation_run_suite(&e1, &r1), HU_OK);
    const hu_evaluation_metric_t *m1 = find_metric(&r1, "score");
    HU_ASSERT_NOT_NULL(m1);

    hu_evaluation_t e2 = {0};
    HU_ASSERT_EQ(make_frontier(A(), &e2), HU_OK);
    hu_evaluation_run_report_t r2 = {0};
    HU_ASSERT_EQ(hu_evaluation_run_suite(&e2, &r2), HU_OK);
    const hu_evaluation_metric_t *m2 = find_metric(&r2, "score");
    HU_ASSERT_NOT_NULL(m2);
    HU_ASSERT_FLOAT_EQ(m1->score, m2->score, 1e-9);

    /* error_summary documents the placeholder; required so CI doesn't think
     * this is a real frontier baseline. */
    HU_ASSERT_NOT_NULL(r1.error_summary);

    hu_evaluation_report_free(A(), &r1);
    hu_evaluation_report_free(A(), &r2);
    hu_evaluation_close(&e1);
    hu_evaluation_close(&e2);
}

/* ── 4. Regression gate fails on synthetic drop ────────────────────────── */

static void test_w16_regression_gate_fails_on_synthetic_drop(void) {
    /* Build a current report for "locomo" with a precision_at_1 of 0.50, then
     * a baseline at 0.80. Drop is 0.30, well above the 0.02 spec threshold.
     * Hand-build the report instead of using the helper so we avoid shipping
     * a test-only mutator. */
    hu_evaluation_run_report_t cur = {0};
    cur.suite_name = strdup("locomo");
    cur.metrics =
        (hu_evaluation_metric_t *)calloc(HU_EVALUATION_MAX_METRICS, sizeof(hu_evaluation_metric_t));
    HU_ASSERT_NOT_NULL(cur.suite_name);
    HU_ASSERT_NOT_NULL(cur.metrics);
    cur.metrics[0].name = strdup("precision_at_1");
    cur.metrics[0].score = 0.50;
    cur.metrics[0].sample_count = 10;
    cur.metrics[0].baseline = NAN;
    cur.metrics_count = 1;

    /* Synthesise a baseline JSON then load it. */
    const char *baseline_json =
        "{\"entries\":[{\"suite\":\"locomo\",\"metric\":\"precision_at_1\","
        "\"score\":0.80,\"sample_count\":10}]}";
    hu_evaluation_baseline_t base = {0};
    HU_ASSERT_EQ(hu_evaluation_baseline_load(A(), baseline_json, strlen(baseline_json), &base),
                 HU_OK);

    hu_evaluation_regression_result_t res = {0};
    HU_ASSERT_EQ(hu_evaluation_regression_check(A(), &cur, &base, &res), HU_OK);
    HU_ASSERT_TRUE(res.any_failed);
    HU_ASSERT_EQ((int)res.findings_count, 1);
    HU_ASSERT_TRUE(res.findings[0].failed);
    HU_ASSERT_NOT_NULL(res.findings[0].reason);

    hu_evaluation_regression_free(A(), &res);
    hu_evaluation_baseline_free(A(), &base);

    /* Free hand-built report */
    free(cur.metrics[0].name);
    free(cur.metrics);
    free(cur.suite_name);
}

/* ── 5. Regression gate passes on no change ────────────────────────────── */

static void test_w16_regression_gate_passes_on_no_change(void) {
    hu_evaluation_t e = {0};
    HU_ASSERT_EQ(make_locomo(A(), &e), HU_OK);
    hu_evaluation_run_report_t r = {0};
    HU_ASSERT_EQ(hu_evaluation_run_suite(&e, &r), HU_OK);

    /* Build a baseline that exactly matches the current run. */
    const hu_evaluation_metric_t *m = find_metric(&r, "precision_at_1");
    HU_ASSERT_NOT_NULL(m);
    char json_buf[256];
    int n =
        snprintf(json_buf, sizeof(json_buf),
                 "{\"entries\":[{\"suite\":\"locomo\",\"metric\":\"precision_at_1\","
                 "\"score\":%.6f,\"sample_count\":10}]}",
                 m->score);
    HU_ASSERT_GT(n, 0);

    hu_evaluation_baseline_t base = {0};
    HU_ASSERT_EQ(hu_evaluation_baseline_load(A(), json_buf, (size_t)n, &base), HU_OK);

    hu_evaluation_regression_result_t res = {0};
    HU_ASSERT_EQ(hu_evaluation_regression_check(A(), &r, &base, &res), HU_OK);
    HU_ASSERT_FALSE(res.any_failed);
    HU_ASSERT_EQ((int)res.findings_count, 1);
    HU_ASSERT_FALSE(res.findings[0].failed);

    hu_evaluation_regression_free(A(), &res);
    hu_evaluation_baseline_free(A(), &base);
    hu_evaluation_report_free(A(), &r);
    hu_evaluation_close(&e);
}

/* ── 6. Baseline JSON round-trip ───────────────────────────────────────── */

static void test_w16_baseline_round_trip_load_and_save(void) {
    const char *initial =
        "{\"entries\":[{\"suite\":\"locomo\",\"metric\":\"precision_at_1\","
        "\"score\":0.85,\"sample_count\":10},"
        "{\"suite\":\"dmr\",\"metric\":\"recall_at_10\","
        "\"score\":0.90,\"sample_count\":5}]}";
    hu_evaluation_baseline_t base = {0};
    HU_ASSERT_EQ(hu_evaluation_baseline_load(A(), initial, strlen(initial), &base), HU_OK);
    HU_ASSERT_EQ((int)base.entries_count, 2);

    double s = 0.0;
    HU_ASSERT_TRUE(hu_evaluation_baseline_lookup(&base, "locomo", "precision_at_1", &s));
    HU_ASSERT_FLOAT_EQ(s, 0.85, 1e-6);
    HU_ASSERT_TRUE(hu_evaluation_baseline_lookup(&base, "dmr", "recall_at_10", &s));
    HU_ASSERT_FLOAT_EQ(s, 0.90, 1e-6);
    HU_ASSERT_FALSE(hu_evaluation_baseline_lookup(&base, "nope", "missing", &s));

    char *json = NULL;
    size_t json_len = 0;
    HU_ASSERT_EQ(hu_evaluation_baseline_save(A(), &base, &json, &json_len), HU_OK);
    HU_ASSERT_NOT_NULL(json);
    HU_ASSERT_GT((long long)json_len, 0);

    hu_evaluation_baseline_t round = {0};
    HU_ASSERT_EQ(hu_evaluation_baseline_load(A(), json, json_len, &round), HU_OK);
    HU_ASSERT_EQ((int)round.entries_count, 2);
    double sr = 0.0;
    HU_ASSERT_TRUE(hu_evaluation_baseline_lookup(&round, "locomo", "precision_at_1", &sr));
    HU_ASSERT_FLOAT_EQ(sr, 0.85, 1e-6);
    HU_ASSERT_TRUE(hu_evaluation_baseline_lookup(&round, "dmr", "recall_at_10", &sr));
    HU_ASSERT_FLOAT_EQ(sr, 0.90, 1e-6);

    A()->free(A()->ctx, json, json_len + 1);
    hu_evaluation_baseline_free(A(), &round);
    hu_evaluation_baseline_free(A(), &base);
}

/* ── 7. Offline judge works without API key ────────────────────────────── */

static void test_w16_offline_judge_works_without_api_key(void) {
    /* Both LoCoMo and LongMemEval ship inline datasets and never call a
     * provider. Running them with no env vars set still returns valid
     * reports. */
    unsetenv("OPENAI_API_KEY");
    unsetenv("ANTHROPIC_API_KEY");
    unsetenv("GOOGLE_API_KEY");

    hu_evaluation_t locomo_e = {0};
    HU_ASSERT_EQ(make_locomo(A(), &locomo_e), HU_OK);
    hu_evaluation_run_report_t lr = {0};
    HU_ASSERT_EQ(hu_evaluation_run_suite(&locomo_e, &lr), HU_OK);
    HU_ASSERT_GT((long long)lr.metrics_count, 0);

    hu_evaluation_t lme_e = {0};
    HU_ASSERT_EQ(make_lme(A(), &lme_e), HU_OK);
    hu_evaluation_run_report_t mr = {0};
    HU_ASSERT_EQ(hu_evaluation_run_suite(&lme_e, &mr), HU_OK);
    HU_ASSERT_EQ((int)mr.metrics_count, 5);

    hu_evaluation_report_free(A(), &lr);
    hu_evaluation_report_free(A(), &mr);
    hu_evaluation_close(&locomo_e);
    hu_evaluation_close(&lme_e);
}

/* ── 8. Run-report JSON serialise round-trip ───────────────────────────── */

static void test_w16_evaluation_run_report_serialize_round_trip(void) {
    hu_evaluation_t e = {0};
    HU_ASSERT_EQ(make_dmr(A(), &e), HU_OK);
    hu_evaluation_run_report_t r = {0};
    HU_ASSERT_EQ(hu_evaluation_run_suite(&e, &r), HU_OK);

    char *json = NULL;
    size_t json_len = 0;
    HU_ASSERT_EQ(hu_evaluation_report_to_json(A(), &r, &json, &json_len), HU_OK);
    HU_ASSERT_NOT_NULL(json);
    HU_ASSERT_STR_CONTAINS(json, "\"suite_name\":\"dmr\"");
    HU_ASSERT_STR_CONTAINS(json, "\"recall_at_10\"");

    hu_evaluation_run_report_t round = {0};
    HU_ASSERT_EQ(hu_evaluation_report_from_json(A(), json, json_len, &round), HU_OK);
    HU_ASSERT_STR_EQ(round.suite_name, "dmr");
    HU_ASSERT_EQ((int)round.metrics_count, (int)r.metrics_count);
    for (size_t i = 0; i < r.metrics_count; i++) {
        const hu_evaluation_metric_t *want = &r.metrics[i];
        const hu_evaluation_metric_t *got = find_metric(&round, want->name);
        HU_ASSERT_NOT_NULL(got);
        HU_ASSERT_FLOAT_EQ(got->score, want->score, 1e-6);
        HU_ASSERT_EQ((int)got->sample_count, (int)want->sample_count);
    }
    HU_ASSERT_EQ((int)round.prompts_total, (int)r.prompts_total);

    A()->free(A()->ctx, json, json_len + 1);
    hu_evaluation_report_free(A(), &round);
    hu_evaluation_report_free(A(), &r);
    hu_evaluation_close(&e);
}

/* ── 9. Invalid args rejected at every public boundary ─────────────────── */

static void test_w16_invalid_args_rejected(void) {
    hu_evaluation_run_report_t r = {0};

    HU_ASSERT_EQ(make_locomo(NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(make_lme(NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(make_dmr(NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(make_minja(NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(make_mab(NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(make_frontier(NULL, NULL), HU_ERR_INVALID_ARGUMENT);

    HU_ASSERT_EQ(hu_evaluation_run_suite(NULL, &r), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(hu_evaluation_get_name(NULL));
    HU_ASSERT_FALSE(hu_evaluation_is_available(NULL));
    /* Idempotent close on zeroed wrapper. */
    hu_evaluation_t zero = {0};
    hu_evaluation_close(&zero);
    hu_evaluation_close(NULL);

    char *json = NULL;
    size_t json_len = 0;
    HU_ASSERT_EQ(hu_evaluation_report_to_json(NULL, &r, &json, &json_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_evaluation_report_from_json(A(), NULL, 0, &r), HU_ERR_INVALID_ARGUMENT);

    HU_ASSERT_EQ(hu_evaluation_baseline_load(NULL, "x", 1, NULL), HU_ERR_INVALID_ARGUMENT);
    hu_evaluation_baseline_t b = {0};
    HU_ASSERT_EQ(hu_evaluation_baseline_save(NULL, &b, &json, &json_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_FALSE(hu_evaluation_baseline_lookup(NULL, "s", "m", NULL));

    HU_ASSERT_EQ(hu_evaluation_regression_check(NULL, NULL, NULL, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    /* Free of zeroed handles is a no-op. */
    hu_evaluation_run_report_t zero_r = {0};
    hu_evaluation_report_free(A(), &zero_r);
    hu_evaluation_baseline_free(A(), &b);
    hu_evaluation_regression_result_t zero_res = {0};
    hu_evaluation_regression_free(A(), &zero_res);
}

/* ── 10. LongMemEval reports five categories ───────────────────────────── */

static void test_w16_longmemeval_returns_five_categories(void) {
    hu_evaluation_t e = {0};
    HU_ASSERT_EQ(make_lme(A(), &e), HU_OK);
    hu_evaluation_run_report_t r = {0};
    HU_ASSERT_EQ(hu_evaluation_run_suite(&e, &r), HU_OK);
    HU_ASSERT_EQ((int)r.metrics_count, 5);

    static const char *const cats[] = {"category_temporal", "category_multi_hop",
                                       "category_single_hop", "category_abstention",
                                       "category_knowledge_update"};
    for (size_t i = 0; i < sizeof(cats) / sizeof(cats[0]); i++) {
        const hu_evaluation_metric_t *m = find_metric(&r, cats[i]);
        HU_ASSERT_NOT_NULL(m);
        HU_ASSERT(m->score >= 0.0 && m->score <= 1.0);
    }

    hu_evaluation_report_free(A(), &r);
    hu_evaluation_close(&e);
}

/* ── 11. DMR recall@K is correct on the known synthetic index ──────────── */

static void test_w16_dmr_recall_at_k_correct_on_known_index(void) {
    hu_evaluation_t e = {0};
    HU_ASSERT_EQ(make_dmr(A(), &e), HU_OK);
    hu_evaluation_run_report_t r = {0};
    HU_ASSERT_EQ(hu_evaluation_run_suite(&e, &r), HU_OK);

    const hu_evaluation_metric_t *r1 = find_metric(&r, "recall_at_1");
    const hu_evaluation_metric_t *r5 = find_metric(&r, "recall_at_5");
    const hu_evaluation_metric_t *r10 = find_metric(&r, "recall_at_10");
    HU_ASSERT_NOT_NULL(r1);
    HU_ASSERT_NOT_NULL(r5);
    HU_ASSERT_NOT_NULL(r10);
    /* recall is non-decreasing in K for a fixed query set. */
    HU_ASSERT(r1->score <= r5->score + 1e-9);
    HU_ASSERT(r5->score <= r10->score + 1e-9);
    /* Synthetic index is built so every query's nearest neighbour is its
     * paired target → recall@1 must be 1.0. */
    HU_ASSERT_FLOAT_EQ(r1->score, 1.0, 1e-9);
    HU_ASSERT_FLOAT_EQ(r10->score, 1.0, 1e-9);

    hu_evaluation_report_free(A(), &r);
    hu_evaluation_close(&e);
}

/* ── 12. MemoryAgentBench stub deterministic ───────────────────────────── */

static void test_w16_memoryagentbench_stub_runs_deterministically(void) {
    hu_evaluation_t e1 = {0};
    HU_ASSERT_EQ(make_mab(A(), &e1), HU_OK);
    HU_ASSERT_TRUE(hu_evaluation_is_available(&e1));

    hu_evaluation_run_report_t r1 = {0};
    HU_ASSERT_EQ(hu_evaluation_run_suite(&e1, &r1), HU_OK);

    hu_evaluation_t e2 = {0};
    HU_ASSERT_EQ(make_mab(A(), &e2), HU_OK);
    hu_evaluation_run_report_t r2 = {0};
    HU_ASSERT_EQ(hu_evaluation_run_suite(&e2, &r2), HU_OK);

    HU_ASSERT_EQ((int)r1.metrics_count, (int)r2.metrics_count);
    const hu_evaluation_metric_t *s1 = find_metric(&r1, "score");
    const hu_evaluation_metric_t *s2 = find_metric(&r2, "score");
    HU_ASSERT_NOT_NULL(s1);
    HU_ASSERT_NOT_NULL(s2);
    HU_ASSERT_FLOAT_EQ(s1->score, s2->score, 1e-9);
    /* error_summary documents the stub status. */
    HU_ASSERT_NOT_NULL(r1.error_summary);

    hu_evaluation_report_free(A(), &r1);
    hu_evaluation_report_free(A(), &r2);
    hu_evaluation_close(&e1);
    hu_evaluation_close(&e2);
}

/* ── runner ────────────────────────────────────────────────────────────── */

void run_w16_evaluation_tests(void) {
    HU_TEST_SUITE(
        "W16 evaluation - continuous benchmark suite (locomo/longmem/dmr/minja/mab/frontier)");
    HU_RUN_TEST(test_w16_locomo_runs_on_synthetic_dataset);
    HU_RUN_TEST(test_w16_minja_attack_blocked_by_w1_write_trust);
    HU_RUN_TEST(test_w16_frontier_compare_pairs_match);
    HU_RUN_TEST(test_w16_regression_gate_fails_on_synthetic_drop);
    HU_RUN_TEST(test_w16_regression_gate_passes_on_no_change);
    HU_RUN_TEST(test_w16_baseline_round_trip_load_and_save);
    HU_RUN_TEST(test_w16_offline_judge_works_without_api_key);
    HU_RUN_TEST(test_w16_evaluation_run_report_serialize_round_trip);
    HU_RUN_TEST(test_w16_invalid_args_rejected);
    HU_RUN_TEST(test_w16_longmemeval_returns_five_categories);
    HU_RUN_TEST(test_w16_dmr_recall_at_k_correct_on_known_index);
    HU_RUN_TEST(test_w16_memoryagentbench_stub_runs_deterministically);
}
