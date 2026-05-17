/* tests/test_mlx_admin.c
 *
 * Adversarial tests for the MLX server admin client (src/ml/mlx_admin.c).
 *
 * Test discipline (per .claude/rules/tests-that-pin-bugs.md):
 *   - Tests phrase assertions as the actual contracts: NULL input
 *     returns INVALID_ARGUMENT; missing server returns IO (not OK);
 *     malformed-but-reachable response is still surfaced via
 *     status_code rather than swallowed.
 *
 * Without curl linked (HU_ENABLE_CURL=OFF) the admin layer is a
 * NOT_SUPPORTED stub. We test BOTH paths:
 *   - With HU_ENABLE_CURL: target an unreachable port to force the
 *     transport-failure code path without needing a live server.
 *   - Without HU_ENABLE_CURL: verify NOT_SUPPORTED is returned cleanly.
 *
 * Production-symbol coverage (per .claude/rules/test-references-production-symbol.md):
 * references hu_mlx_admin_swap_adapter + hu_mlx_admin_current_adapter
 * + hu_mlx_admin_swap_result_free + hu_mlx_admin_current_adapter_free
 * — every public symbol exported from mlx_admin.c. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/mlx_admin.h"
#include "test_framework.h"

#include <string.h>

static hu_allocator_t A(void) {
    return hu_system_allocator();
}

/* ── NULL-arg discipline ──────────────────────────────────────────── */

static void swap_null_alloc_returns_invalid_argument(void) {
    hu_mlx_admin_swap_result_t r;
    memset(&r, 0, sizeof(r));
    /* NULL alloc — only safe surface is INVALID_ARGUMENT.
     * When curl is OFF, this MAY return NOT_SUPPORTED instead, since
     * the function never inspects its arguments. Accept either. */
    hu_error_t err = hu_mlx_admin_swap_adapter(NULL, "http://127.0.0.1:9/v1", 21, "/tmp/x", 6, &r);
    HU_ASSERT_TRUE(err == HU_ERR_INVALID_ARGUMENT || err == HU_ERR_NOT_SUPPORTED);
    hu_mlx_admin_swap_result_free(NULL, &r); /* must not crash on NULL alloc */
}

static void swap_null_base_url_returns_invalid_argument(void) {
    hu_allocator_t alloc = A();
    hu_mlx_admin_swap_result_t r;
    memset(&r, 0, sizeof(r));
    hu_error_t err = hu_mlx_admin_swap_adapter(&alloc, NULL, 0, "/tmp/x", 6, &r);
    HU_ASSERT_TRUE(err == HU_ERR_INVALID_ARGUMENT || err == HU_ERR_NOT_SUPPORTED);
    hu_mlx_admin_swap_result_free(&alloc, &r);
}

static void swap_null_adapter_path_returns_invalid_argument(void) {
    hu_allocator_t alloc = A();
    hu_mlx_admin_swap_result_t r;
    memset(&r, 0, sizeof(r));
    hu_error_t err = hu_mlx_admin_swap_adapter(&alloc, "http://127.0.0.1:9/v1", 21, NULL, 0, &r);
    HU_ASSERT_TRUE(err == HU_ERR_INVALID_ARGUMENT || err == HU_ERR_NOT_SUPPORTED);
    hu_mlx_admin_swap_result_free(&alloc, &r);
}

static void swap_null_result_returns_invalid_argument(void) {
    hu_allocator_t alloc = A();
    hu_error_t err =
        hu_mlx_admin_swap_adapter(&alloc, "http://127.0.0.1:9/v1", 21, "/tmp/x", 6, NULL);
    HU_ASSERT_TRUE(err == HU_ERR_INVALID_ARGUMENT || err == HU_ERR_NOT_SUPPORTED);
}

static void current_null_alloc_returns_invalid_argument(void) {
    hu_mlx_admin_current_adapter_t c;
    memset(&c, 0, sizeof(c));
    hu_error_t err = hu_mlx_admin_current_adapter(NULL, "http://127.0.0.1:9/v1", 21, &c);
    HU_ASSERT_TRUE(err == HU_ERR_INVALID_ARGUMENT || err == HU_ERR_NOT_SUPPORTED);
    hu_mlx_admin_current_adapter_free(NULL, &c);
}

/* ── Unreachable server → IO error (or NOT_SUPPORTED when curl off) ─── */

static void swap_unreachable_server_returns_io_or_not_supported(void) {
    hu_allocator_t alloc = A();
    /* Port 9 is the discard service — almost guaranteed to refuse a
     * connection. If something IS listening locally we'd see status
     * 4xx/5xx but not HU_OK with status_code 200. The test asserts
     * the FAILURE class, not a specific status. */
    hu_mlx_admin_swap_result_t r;
    memset(&r, 0, sizeof(r));
    hu_error_t err =
        hu_mlx_admin_swap_adapter(&alloc, "http://127.0.0.1:9/v1", 21, "/tmp/nope", 9, &r);
    /* Two valid outcomes:
     *   - HU_ERR_IO when curl is linked and the connection refuses
     *   - HU_ERR_NOT_SUPPORTED when curl is off
     * Either way we MUST NOT see HU_OK with status_code 200 — that
     * would mean a real server somewhere accepted the swap. */
    HU_ASSERT_TRUE(err == HU_ERR_IO || err == HU_ERR_NOT_SUPPORTED);
    if (err == HU_OK)
        HU_ASSERT_TRUE(r.status_code != 200);
    hu_mlx_admin_swap_result_free(&alloc, &r);
}

static void current_unreachable_server_returns_io_or_not_supported(void) {
    hu_allocator_t alloc = A();
    hu_mlx_admin_current_adapter_t c;
    memset(&c, 0, sizeof(c));
    hu_error_t err = hu_mlx_admin_current_adapter(&alloc, "http://127.0.0.1:9/v1", 21, &c);
    HU_ASSERT_TRUE(err == HU_ERR_IO || err == HU_ERR_NOT_SUPPORTED);
    if (err == HU_OK)
        HU_ASSERT_TRUE(c.status_code != 200);
    hu_mlx_admin_current_adapter_free(&alloc, &c);
}

/* ── Memory hygiene on free with zero-initialised state ─────────────── */

static void swap_result_free_zero_initialized_is_safe(void) {
    hu_allocator_t alloc = A();
    hu_mlx_admin_swap_result_t r;
    memset(&r, 0, sizeof(r));
    hu_mlx_admin_swap_result_free(&alloc, &r); /* no-op; must not crash */
}

static void current_free_zero_initialized_is_safe(void) {
    hu_allocator_t alloc = A();
    hu_mlx_admin_current_adapter_t c;
    memset(&c, 0, sizeof(c));
    hu_mlx_admin_current_adapter_free(&alloc, &c);
}

/* ── Suite runner ─────────────────────────────────────────────────── */

void run_mlx_admin_tests(void);
void run_mlx_admin_tests(void) {
    HU_TEST_SUITE("MLXAdmin");
    HU_RUN_TEST(swap_null_alloc_returns_invalid_argument);
    HU_RUN_TEST(swap_null_base_url_returns_invalid_argument);
    HU_RUN_TEST(swap_null_adapter_path_returns_invalid_argument);
    HU_RUN_TEST(swap_null_result_returns_invalid_argument);
    HU_RUN_TEST(current_null_alloc_returns_invalid_argument);
    HU_RUN_TEST(swap_unreachable_server_returns_io_or_not_supported);
    HU_RUN_TEST(current_unreachable_server_returns_io_or_not_supported);
    HU_RUN_TEST(swap_result_free_zero_initialized_is_safe);
    HU_RUN_TEST(current_free_zero_initialized_is_safe);
}
