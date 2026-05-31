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
#include "test_tmpdir.h"

#include <stdio.h>
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

/* ── Health probe (Dermot C2) ───────────────────────────────────────── */

/* The test override forces the probe result without any network, in both
 * curl and non-curl builds. Always clear it afterward so later tests (and
 * the daemon path under HU_IS_TEST) see real behavior. */
static void probe_health_test_override_true_returns_true(void) {
    hu_allocator_t alloc = A();
    hu_mlx_admin_set_test_health(true);
    HU_ASSERT_TRUE(hu_mlx_admin_probe_health(&alloc, "http://127.0.0.1:9/v1", 21));
    hu_mlx_admin_clear_test_health();
}

static void probe_health_test_override_false_returns_false(void) {
    hu_allocator_t alloc = A();
    hu_mlx_admin_set_test_health(false);
    HU_ASSERT_FALSE(hu_mlx_admin_probe_health(&alloc, "http://127.0.0.1:9/v1", 21));
    hu_mlx_admin_clear_test_health();
}

/* With the override cleared, NULL/empty inputs are conservatively unhealthy
 * (→ caller routes to cloud) rather than crashing. */
static void probe_health_null_args_return_false(void) {
    hu_allocator_t alloc = A();
    hu_mlx_admin_clear_test_health();
    HU_ASSERT_FALSE(hu_mlx_admin_probe_health(NULL, "http://127.0.0.1:9/v1", 21));
    HU_ASSERT_FALSE(hu_mlx_admin_probe_health(&alloc, NULL, 0));
    HU_ASSERT_FALSE(hu_mlx_admin_probe_health(&alloc, "x", 0));
}

/* No override, real call to an unreachable port: must report unhealthy
 * (curl build → transport failure → false; non-curl build → false stub). */
static void probe_health_unreachable_server_returns_false(void) {
    hu_allocator_t alloc = A();
    hu_mlx_admin_clear_test_health(); /* also clears the 60s cache */
    HU_ASSERT_FALSE(hu_mlx_admin_probe_health(&alloc, "http://127.0.0.1:9/v1", 21));
    hu_mlx_admin_clear_test_health();
}

/* ── Suite runner ─────────────────────────────────────────────────── */

/* ── LoRA scale safety gate (lora-scale-default-or-die.md) ── */

static void lora_scale_classify_truth_table(void) {
    HU_ASSERT_EQ((int)hu_lora_scale_classify(2.0), (int)HU_LORA_SCALE_SAFE);
    HU_ASSERT_EQ((int)hu_lora_scale_classify(4.0), (int)HU_LORA_SCALE_SAFE);
    HU_ASSERT_EQ((int)hu_lora_scale_classify(4.01), (int)HU_LORA_SCALE_WARN);
    HU_ASSERT_EQ((int)hu_lora_scale_classify(8.0), (int)HU_LORA_SCALE_WARN);
    HU_ASSERT_EQ((int)hu_lora_scale_classify(8.01), (int)HU_LORA_SCALE_REJECT);
    HU_ASSERT_EQ((int)hu_lora_scale_classify(10.0), (int)HU_LORA_SCALE_REJECT);
    HU_ASSERT_EQ((int)hu_lora_scale_classify(20.0), (int)HU_LORA_SCALE_REJECT);
}

static void seed_adapter_config(const char *dir, const char *json) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/adapter_config.json", dir);
    FILE *f = fopen(path, "wb");
    HU_ASSERT_NOT_NULL(f);
    fputs(json, f);
    fclose(f);
}

static void lora_scale_guard_refuses_over_scaled_adapter(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char dir[256];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu-lora-scale", dir, sizeof(dir)));

    /* scale=10.0 — exactly the over-scaled artifact this guard exists to stop. */
    seed_adapter_config(dir, "{\"lora_parameters\": {\"rank\": 8, \"scale\": 10.0}}");
    double sc = 0.0;
    HU_ASSERT_EQ(hu_lora_adapter_config_scale(&alloc, dir, strlen(dir), &sc), HU_OK);
    HU_ASSERT_TRUE(sc > 9.9 && sc < 10.1);
    HU_ASSERT_EQ(hu_lora_scale_guard_serveable(&alloc, dir, strlen(dir)), HU_ERR_INVALID_ARGUMENT);

    /* scale=2.0 — the mandated value — is serveable. */
    seed_adapter_config(dir, "{\"lora_parameters\": {\"rank\": 8, \"scale\": 2.0}}");
    HU_ASSERT_EQ(hu_lora_scale_guard_serveable(&alloc, dir, strlen(dir)), HU_OK);

    hu_test_rm_rf(dir);
}

static void lora_scale_guard_fails_open_without_config(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char dir[256];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu-lora-noconfig", dir, sizeof(dir)));
    /* No adapter_config.json → reader reports NOT_FOUND, guard fails open (HU_OK). */
    double sc = 0.0;
    HU_ASSERT_EQ(hu_lora_adapter_config_scale(&alloc, dir, strlen(dir), &sc), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(hu_lora_scale_guard_serveable(&alloc, dir, strlen(dir)), HU_OK);
    hu_test_rm_rf(dir);
}

/* Production swap callers pass the WEIGHTS FILE path (".../adapters.safetensors"),
 * not the adapter directory. The guard must resolve to the parent dir's
 * adapter_config.json — otherwise it fail-opens and never blocks over-scaled
 * adapters on the real swap path (cursor/Copilot review, PR #207). */
static void lora_scale_guard_refuses_over_scaled_adapter_by_file_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char dir[256];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu-lora-filepath", dir, sizeof(dir)));

    seed_adapter_config(dir, "{\"lora_parameters\": {\"rank\": 8, \"scale\": 10.0}}");
    char file_path[1200];
    snprintf(file_path, sizeof(file_path), "%s/adapters.safetensors", dir);
    FILE *wf = fopen(file_path, "wb");
    HU_ASSERT_NOT_NULL(wf);
    fputs("weights", wf);
    fclose(wf);

    /* reader resolves the file path to the adapter dir and reads scale=10.0 */
    double sc = 0.0;
    HU_ASSERT_EQ(hu_lora_adapter_config_scale(&alloc, file_path, strlen(file_path), &sc), HU_OK);
    HU_ASSERT_TRUE(sc > 9.9 && sc < 10.1);
    /* the guard BLOCKS the over-scaled adapter even when given the file path */
    HU_ASSERT_EQ(hu_lora_scale_guard_serveable(&alloc, file_path, strlen(file_path)),
                 HU_ERR_INVALID_ARGUMENT);

    /* a safe (2.0) adapter is serveable by file path too */
    seed_adapter_config(dir, "{\"lora_parameters\": {\"rank\": 8, \"scale\": 2.0}}");
    HU_ASSERT_EQ(hu_lora_scale_guard_serveable(&alloc, file_path, strlen(file_path)), HU_OK);

    hu_test_rm_rf(dir);
}

/* The guard runs BEFORE the swap, so the weights file may not exist yet (about
 * to be written) or anymore. With a ".safetensors" path whose file is ABSENT,
 * the reader must still resolve to the parent dir via the basename heuristic and
 * find the over-scaled config beside it — otherwise it fail-opens (cursor review,
 * PR #207, comment 3330095857). */
static void lora_scale_guard_refuses_over_scaled_adapter_missing_weights_file(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char dir[256];
    HU_ASSERT_TRUE(hu_test_mkdtemp("hu-lora-nofile", dir, sizeof(dir)));

    /* Over-scaled config present; the .safetensors file is NEVER created. */
    seed_adapter_config(dir, "{\"lora_parameters\": {\"rank\": 8, \"scale\": 10.0}}");
    char missing_file[1200];
    snprintf(missing_file, sizeof(missing_file), "%s/adapters.safetensors", dir);

    /* stat() fails on the missing file; the '.safetensors' extension still
     * resolves it to the parent dir, so scale=10.0 is read and BLOCKED. */
    double sc = 0.0;
    HU_ASSERT_EQ(hu_lora_adapter_config_scale(&alloc, missing_file, strlen(missing_file), &sc),
                 HU_OK);
    HU_ASSERT_TRUE(sc > 9.9 && sc < 10.1);
    HU_ASSERT_EQ(hu_lora_scale_guard_serveable(&alloc, missing_file, strlen(missing_file)),
                 HU_ERR_INVALID_ARGUMENT);

    hu_test_rm_rf(dir);
}

void run_mlx_admin_tests(void);
void run_mlx_admin_tests(void) {
    HU_TEST_SUITE("MLXAdmin");
    HU_RUN_TEST(lora_scale_classify_truth_table);
    HU_RUN_TEST(lora_scale_guard_refuses_over_scaled_adapter);
    HU_RUN_TEST(lora_scale_guard_refuses_over_scaled_adapter_by_file_path);
    HU_RUN_TEST(lora_scale_guard_refuses_over_scaled_adapter_missing_weights_file);
    HU_RUN_TEST(lora_scale_guard_fails_open_without_config);
    HU_RUN_TEST(swap_null_alloc_returns_invalid_argument);
    HU_RUN_TEST(swap_null_base_url_returns_invalid_argument);
    HU_RUN_TEST(swap_null_adapter_path_returns_invalid_argument);
    HU_RUN_TEST(swap_null_result_returns_invalid_argument);
    HU_RUN_TEST(current_null_alloc_returns_invalid_argument);
    HU_RUN_TEST(swap_unreachable_server_returns_io_or_not_supported);
    HU_RUN_TEST(current_unreachable_server_returns_io_or_not_supported);
    HU_RUN_TEST(swap_result_free_zero_initialized_is_safe);
    HU_RUN_TEST(current_free_zero_initialized_is_safe);
    HU_RUN_TEST(probe_health_test_override_true_returns_true);
    HU_RUN_TEST(probe_health_test_override_false_returns_false);
    HU_RUN_TEST(probe_health_null_args_return_false);
    HU_RUN_TEST(probe_health_unreachable_server_returns_false);
}
