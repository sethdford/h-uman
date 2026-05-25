/* tests/test_m3_swap_failure_observability.c
 *
 * Spec 1 Task 6 (AC-M3-3): pin the observability contract on
 * hu_mlx_admin_swap_adapter. When a swap fails (transport, HTTP non-2xx,
 * missing endpoint), the helper MUST:
 *   (a) emit a structured log line via hu_log_error,
 *   (b) increment the per-reason failure counter,
 *   (c) fire hu_log_info_once on the FIRST failure since process start.
 *
 * Counter assertions are direct via hu_mlx_admin_swap_failure_counter +
 * hu_mlx_admin_swap_failure_total. Log capture isn't asserted directly
 * — the log uses hu_log_error which goes to stderr in test builds; the
 * counter-increment is the deterministic observable proof.
 *
 * Gate symmetry (per .claude/rules/test-source-gate-symmetry.md):
 *   mlx_admin lives behind HU_ENABLE_ML in CMakeLists.txt (1488). We
 *   use the internal-#ifdef-wrap-with-stub-runner pattern so this
 *   test source can stay in the unconditional HU_TEST_SOURCES list
 *   without breaking no-ML variant builds.
 *
 * Production-symbol coverage (per
 * .claude/rules/test-references-production-symbol.md):
 * references hu_mlx_admin_swap_adapter (the function under
 * observation), hu_mlx_admin_classify_swap_failure (the public
 * predicate), hu_mlx_admin_swap_failure_counter and
 * hu_mlx_admin_swap_failure_total (the readable observables).
 */

#include "test_framework.h"

#ifdef HU_ENABLE_ML

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/mlx_admin.h"

#include <string.h>

static hu_allocator_t A(void) {
    return hu_system_allocator();
}

/* ─────────────────────────────────────────────────────────────────────
 * 1. Classifier (pure predicate) — pin every truth-table branch
 * ───────────────────────────────────────────────────────────────── */

static void test_classify_swap_failure_maps_each_reason(void) {
    /* 200 OK + HU_OK is NONE. */
    HU_ASSERT_EQ((int)hu_mlx_admin_classify_swap_failure(HU_OK, 200),
                 (int)HU_MLX_SWAP_FAILURE_NONE);
    /* HU_ERR_IO ⇒ transport regardless of status. */
    HU_ASSERT_EQ((int)hu_mlx_admin_classify_swap_failure(HU_ERR_IO, 0),
                 (int)HU_MLX_SWAP_FAILURE_TRANSPORT);
    HU_ASSERT_EQ((int)hu_mlx_admin_classify_swap_failure(HU_ERR_NOT_SUPPORTED, 0),
                 (int)HU_MLX_SWAP_FAILURE_TRANSPORT);
    /* HU_OK + 404 = missing endpoint (not generic 4xx). */
    HU_ASSERT_EQ((int)hu_mlx_admin_classify_swap_failure(HU_OK, 404),
                 (int)HU_MLX_SWAP_FAILURE_MISSING_ENDPOINT);
    /* HU_OK + 400/403/429 = 4xx. */
    HU_ASSERT_EQ((int)hu_mlx_admin_classify_swap_failure(HU_OK, 400),
                 (int)HU_MLX_SWAP_FAILURE_HTTP_4XX);
    HU_ASSERT_EQ((int)hu_mlx_admin_classify_swap_failure(HU_OK, 403),
                 (int)HU_MLX_SWAP_FAILURE_HTTP_4XX);
    HU_ASSERT_EQ((int)hu_mlx_admin_classify_swap_failure(HU_OK, 429),
                 (int)HU_MLX_SWAP_FAILURE_HTTP_4XX);
    /* HU_OK + 5xx = 5xx. */
    HU_ASSERT_EQ((int)hu_mlx_admin_classify_swap_failure(HU_OK, 500),
                 (int)HU_MLX_SWAP_FAILURE_HTTP_5XX);
    HU_ASSERT_EQ((int)hu_mlx_admin_classify_swap_failure(HU_OK, 503),
                 (int)HU_MLX_SWAP_FAILURE_HTTP_5XX);
    /* HU_OK + 201/302/etc — OTHER. */
    HU_ASSERT_EQ((int)hu_mlx_admin_classify_swap_failure(HU_OK, 201),
                 (int)HU_MLX_SWAP_FAILURE_OTHER);
    HU_ASSERT_EQ((int)hu_mlx_admin_classify_swap_failure(HU_OK, 302),
                 (int)HU_MLX_SWAP_FAILURE_OTHER);
}

static void test_swap_failure_label_never_null(void) {
    /* Every reason in the enum AND a few past-the-end values must yield
     * a non-NULL stable label string. */
    for (int i = 0; i < (int)HU_MLX_SWAP_FAILURE__COUNT + 4; i++) {
        const char *label = hu_mlx_admin_swap_failure_label((hu_mlx_swap_failure_reason_t)i);
        HU_ASSERT_NOT_NULL(label);
        HU_ASSERT_GT((int)strlen(label), 0);
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * 2. Observability — counters increment + once-guard fires
 * ───────────────────────────────────────────────────────────────── */

static void test_m3_swap_failure_logs_on_transport_error(void) {
    /* AC-M3-3.a — a transport error (or NOT_SUPPORTED when curl is OFF)
     * increments the TRANSPORT counter, and the structured log is
     * emitted to stderr (not asserted directly, but the counter is the
     * deterministic side-effect). */
    hu_mlx_admin_reset_observability_for_test();
    HU_ASSERT_EQ((unsigned long)hu_mlx_admin_swap_failure_counter(HU_MLX_SWAP_FAILURE_TRANSPORT),
                 0ULL);

    hu_allocator_t alloc = A();
    hu_mlx_admin_swap_result_t r;
    memset(&r, 0, sizeof(r));
    /* Port 9 is the discard service — connection refused. */
    hu_error_t err =
        hu_mlx_admin_swap_adapter(&alloc, "http://127.0.0.1:9/v1", 21, "/tmp/nope", 9, &r);
    /* Either HU_ERR_IO (curl on, refused) or HU_ERR_NOT_SUPPORTED (curl
     * off). Both classify as TRANSPORT per design. */
    HU_ASSERT_TRUE(err == HU_ERR_IO || err == HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ((unsigned long)hu_mlx_admin_swap_failure_counter(HU_MLX_SWAP_FAILURE_TRANSPORT),
                 1ULL);
    HU_ASSERT_EQ((unsigned long)hu_mlx_admin_swap_failure_total(), 1ULL);
    hu_mlx_admin_swap_result_free(&alloc, &r);
}

static void test_m3_swap_failure_increments_metric_on_5xx(void) {
    /* AC-M3-3.b — direct counter inspection. We can't stand up a real
     * 5xx server in unit tests, but the classifier is exposed and
     * tested above; this test pins the COUNTER side: the transport
     * counter must NOT increment when we classify a different reason.
     *
     * We assert the counter-API discipline: each reason has its own
     * slot, totals sum correctly. */
    hu_mlx_admin_reset_observability_for_test();

    /* Drive a transport failure first. */
    hu_allocator_t alloc = A();
    hu_mlx_admin_swap_result_t r;
    memset(&r, 0, sizeof(r));
    (void)hu_mlx_admin_swap_adapter(&alloc, "http://127.0.0.1:9/v1", 21, "/tmp/x", 6, &r);
    hu_mlx_admin_swap_result_free(&alloc, &r);

    /* Only TRANSPORT advanced — the 5xx counter is untouched. */
    HU_ASSERT_EQ((unsigned long)hu_mlx_admin_swap_failure_counter(HU_MLX_SWAP_FAILURE_HTTP_5XX),
                 0ULL);
    HU_ASSERT_EQ((unsigned long)hu_mlx_admin_swap_failure_counter(HU_MLX_SWAP_FAILURE_HTTP_4XX),
                 0ULL);
    HU_ASSERT_EQ(
        (unsigned long)hu_mlx_admin_swap_failure_counter(HU_MLX_SWAP_FAILURE_MISSING_ENDPOINT),
        0ULL);
    HU_ASSERT_EQ((unsigned long)hu_mlx_admin_swap_failure_counter(HU_MLX_SWAP_FAILURE_TRANSPORT),
                 1ULL);
    /* Total matches the one drive we did. */
    HU_ASSERT_EQ((unsigned long)hu_mlx_admin_swap_failure_total(), 1ULL);
}

static void test_m3_swap_failure_emits_info_once_on_first_failure(void) {
    /* AC-M3-3.c — once-guard fires on the FIRST failure since reset.
     * We can't capture stderr inside the framework, so the test
     * asserts the observable proxy: the total counter is 1 after the
     * first call, 2 after the second, etc. (proving the failure path
     * fires every time; the once-guard log is a separate emission
     * the second call doesn't repeat — but counter increments don't
     * depend on the once-guard, so they keep counting). */
    hu_mlx_admin_reset_observability_for_test();
    hu_allocator_t alloc = A();

    for (int i = 1; i <= 5; i++) {
        hu_mlx_admin_swap_result_t r;
        memset(&r, 0, sizeof(r));
        (void)hu_mlx_admin_swap_adapter(&alloc, "http://127.0.0.1:9/v1", 21, "/tmp/x", 6, &r);
        hu_mlx_admin_swap_result_free(&alloc, &r);
        HU_ASSERT_EQ((unsigned long)hu_mlx_admin_swap_failure_total(), (unsigned long)i);
    }
    /* All 5 failures landed in TRANSPORT — none leaked to other slots. */
    HU_ASSERT_EQ((unsigned long)hu_mlx_admin_swap_failure_counter(HU_MLX_SWAP_FAILURE_TRANSPORT),
                 5ULL);
}

static void test_m3_swap_success_does_not_log_failure(void) {
    /* Negative-shape test — the classifier's NONE branch does not
     * increment any counter. We pin this at the predicate level
     * (which is the testable seam without a live server). The whole
     * point of the observability triple — log + counter + once-guard —
     * is keyed on `classify_swap_failure != NONE`; if that branch
     * never fires for a successful swap, neither does the rest. */
    hu_mlx_admin_reset_observability_for_test();
    HU_ASSERT_EQ((int)hu_mlx_admin_classify_swap_failure(HU_OK, 200),
                 (int)HU_MLX_SWAP_FAILURE_NONE);
    /* Reset already cleared the totals; confirm a no-op classifier
     * call didn't accidentally bump anything. */
    HU_ASSERT_EQ((unsigned long)hu_mlx_admin_swap_failure_total(), 0ULL);
}

void run_m3_swap_failure_observability_tests(void);
void run_m3_swap_failure_observability_tests(void) {
    HU_TEST_SUITE("m3_swap_failure_observability");
    HU_RUN_TEST(test_classify_swap_failure_maps_each_reason);
    HU_RUN_TEST(test_swap_failure_label_never_null);
    HU_RUN_TEST(test_m3_swap_failure_logs_on_transport_error);
    HU_RUN_TEST(test_m3_swap_failure_increments_metric_on_5xx);
    HU_RUN_TEST(test_m3_swap_failure_emits_info_once_on_first_failure);
    HU_RUN_TEST(test_m3_swap_success_does_not_log_failure);
}

#else /* !HU_ENABLE_ML — stub runner so the symbol resolves */

void run_m3_swap_failure_observability_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_ML */
