/* tests/test_mlx_load_adapter.c
 *
 * M3 Phase B5 (2026-05-19) — pins the mlx provider's load_adapter
 * wiring. Previously `mlx_load_adapter` was a NOT_SUPPORTED stub; the
 * vtable returned cleanly but the adapter never made it to the
 * subprocess argv. The audit identified this as the single biggest
 * gap blocking M3 personalization from reaching production chat.
 *
 * Tests pinned (positive contracts per
 * .claude/rules/tests-that-pin-bugs.md):
 *   1. NULL ctx / alloc / path → HU_ERR_INVALID_ARGUMENT (caller-safe)
 *   2. Directory missing adapters.safetensors → HU_ERR_NOT_FOUND
 *   3. Directory with adapters.safetensors → HU_OK + path persisted
 *   4. After successful load, `hu_mlx_provider_active_adapter_path`
 *      returns the new path → confirms subprocess argv builder will
 *      include `--adapter-path <path>` on next chat.
 *   5. A failed load (NOT_FOUND) leaves the prior adapter intact —
 *      atomic-swap discipline (no partial state).
 *
 * Production-symbol coverage (per
 * .claude/rules/test-references-production-symbol.md): this file
 * references hu_mlx_provider_create, hu_mlx_provider_active_adapter_path,
 * and the vtable's load_adapter pointer — every public symbol the
 * load path touches.
 *
 * Side-effect discipline (per .claude/rules/testing.md / quality-gates.md):
 *   - No subprocess spawn — HU_IS_TEST short-circuits mlx_run_subprocess
 *     and we never call chat() in this file.
 *   - No real network — load_adapter is pure filesystem + ctx state.
 *   - mkdtemp under /tmp, removed in teardown.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "human/providers/mlx.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static hu_allocator_t A(void) {
    return hu_system_allocator();
}

/* Build a temp directory tree under /tmp, return strdup'd path; caller
 * must rm -rf afterwards. */
static char *make_tempdir(void) {
    char tmpl[256];
    snprintf(tmpl, sizeof(tmpl), "/tmp/hu_mlx_load_adapter_XXXXXX");
    char *p = mkdtemp(tmpl);
    if (!p)
        return NULL;
    return strdup(p);
}

static void rm_rf(const char *path) {
    if (!path)
        return;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    int rc = system(cmd);
    (void)rc;
}

/* Write an (empty) adapters.safetensors file into `dir`. The mlx-lm CLI
 * will read this — for the load_adapter contract test, an existing
 * (even zero-byte) file at the correct path is enough to satisfy the
 * access(F_OK) check. */
static int write_safetensors_marker(const char *dir) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/adapters.safetensors", dir);
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    /* Minimal safetensors-shaped bytes (8-byte little-endian header
     * length = 0). load_adapter only checks existence; the bytes don't
     * matter, but writing a few keeps the file from looking like a
     * truncate-on-open artifact in dev poking. */
    unsigned char hdr[8] = {0};
    (void)fwrite(hdr, 1, sizeof(hdr), f);
    fclose(f);
    return 0;
}

/* ── 1. NULL/invalid argument contracts ──────────────────────────────── */

static void load_adapter_null_ctx_returns_invalid_argument(void) {
    hu_allocator_t alloc = A();
    hu_mlx_config_t cfg = {0};
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);
    /* Call vtable directly with NULL ctx — the dispatcher's NULL check
     * would short-circuit before reaching here, so we exercise the
     * provider's own guard. */
    HU_ASSERT_EQ(p.vtable->load_adapter(NULL, &alloc, "/tmp", 4, "id", 2), HU_ERR_INVALID_ARGUMENT);
    p.vtable->deinit(p.ctx, &alloc);
}

static void load_adapter_null_alloc_returns_invalid_argument(void) {
    hu_allocator_t alloc = A();
    hu_mlx_config_t cfg = {0};
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);
    HU_ASSERT_EQ(p.vtable->load_adapter(p.ctx, NULL, "/tmp", 4, "id", 2), HU_ERR_INVALID_ARGUMENT);
    p.vtable->deinit(p.ctx, &alloc);
}

static void load_adapter_null_path_returns_invalid_argument(void) {
    hu_allocator_t alloc = A();
    hu_mlx_config_t cfg = {0};
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);
    HU_ASSERT_EQ(p.vtable->load_adapter(p.ctx, &alloc, NULL, 0, "id", 2), HU_ERR_INVALID_ARGUMENT);
    p.vtable->deinit(p.ctx, &alloc);
}

/* ── 2. Missing safetensors → NOT_FOUND ──────────────────────────────── */

static void load_adapter_missing_safetensors_returns_not_found(void) {
    hu_allocator_t alloc = A();
    hu_mlx_config_t cfg = {0};
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);

    char *dir = make_tempdir();
    HU_ASSERT_NOT_NULL(dir);
    /* Deliberately do NOT write adapters.safetensors. */
    hu_error_t err = p.vtable->load_adapter(p.ctx, &alloc, dir, strlen(dir), "missing", 7);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);

    /* Persisted path must still be empty — failed load leaves ctx
     * unchanged (atomic-swap discipline). */
    size_t plen = 0;
    HU_ASSERT_NULL((void *)hu_mlx_provider_active_adapter_path(&p, &plen));
    HU_ASSERT_EQ((long)plen, 0L);

    rm_rf(dir);
    free(dir);
    p.vtable->deinit(p.ctx, &alloc);
}

static void load_adapter_nonexistent_directory_returns_not_found(void) {
    hu_allocator_t alloc = A();
    hu_mlx_config_t cfg = {0};
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);

    /* This path is constructed to not exist. */
    const char *nope = "/tmp/hu_mlx_load_adapter_definitely_does_not_exist_xyz123";
    HU_ASSERT_EQ(p.vtable->load_adapter(p.ctx, &alloc, nope, strlen(nope), "id", 2),
                 HU_ERR_NOT_FOUND);
    p.vtable->deinit(p.ctx, &alloc);
}

/* ── 3. Valid directory → OK + persisted path ────────────────────────── */

static void load_adapter_valid_directory_returns_ok_and_persists_path(void) {
    hu_allocator_t alloc = A();
    hu_mlx_config_t cfg = {0};
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);

    char *dir = make_tempdir();
    HU_ASSERT_NOT_NULL(dir);
    HU_ASSERT_EQ(write_safetensors_marker(dir), 0);

    hu_error_t err = p.vtable->load_adapter(p.ctx, &alloc, dir, strlen(dir), "persona-test", 12);
    HU_ASSERT_EQ(err, HU_OK);

    /* Persisted path matches the input — this is what the subprocess
     * argv builder reads when it constructs `--adapter-path <path>`. */
    size_t plen = 0;
    const char *active = hu_mlx_provider_active_adapter_path(&p, &plen);
    HU_ASSERT_NOT_NULL(active);
    HU_ASSERT_EQ((long)plen, (long)strlen(dir));
    HU_ASSERT_TRUE(strncmp(active, dir, plen) == 0);

    rm_rf(dir);
    free(dir);
    p.vtable->deinit(p.ctx, &alloc);
}

/* ── 4. Failed swap leaves prior adapter intact ──────────────────────── */

static void load_adapter_failed_swap_preserves_prior_adapter(void) {
    hu_allocator_t alloc = A();
    hu_mlx_config_t cfg = {0};
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);

    /* First: load a valid adapter. */
    char *dir = make_tempdir();
    HU_ASSERT_NOT_NULL(dir);
    HU_ASSERT_EQ(write_safetensors_marker(dir), 0);
    HU_ASSERT_EQ(p.vtable->load_adapter(p.ctx, &alloc, dir, strlen(dir), "a", 1), HU_OK);
    size_t plen = 0;
    HU_ASSERT_NOT_NULL((void *)hu_mlx_provider_active_adapter_path(&p, &plen));

    /* Then: attempt to load a missing one. Must fail AND leave the
     * first adapter intact (the daemon must not silently drop the
     * working adapter when an operator typo'd the new path). */
    const char *nope = "/tmp/hu_mlx_load_adapter_swap_target_missing_xyz";
    HU_ASSERT_EQ(p.vtable->load_adapter(p.ctx, &alloc, nope, strlen(nope), "b", 1),
                 HU_ERR_NOT_FOUND);

    size_t plen2 = 0;
    const char *still = hu_mlx_provider_active_adapter_path(&p, &plen2);
    HU_ASSERT_NOT_NULL(still);
    HU_ASSERT_EQ((long)plen2, (long)strlen(dir));
    HU_ASSERT_TRUE(strncmp(still, dir, plen2) == 0);

    rm_rf(dir);
    free(dir);
    p.vtable->deinit(p.ctx, &alloc);
}

/* ── 5. Replace one valid adapter with another ───────────────────────── */

static void load_adapter_replaces_prior_path_on_success(void) {
    hu_allocator_t alloc = A();
    hu_mlx_config_t cfg = {0};
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);

    char *dir1 = make_tempdir();
    char *dir2 = make_tempdir();
    HU_ASSERT_NOT_NULL(dir1);
    HU_ASSERT_NOT_NULL(dir2);
    HU_ASSERT_EQ(write_safetensors_marker(dir1), 0);
    HU_ASSERT_EQ(write_safetensors_marker(dir2), 0);

    HU_ASSERT_EQ(p.vtable->load_adapter(p.ctx, &alloc, dir1, strlen(dir1), "a", 1), HU_OK);
    HU_ASSERT_EQ(p.vtable->load_adapter(p.ctx, &alloc, dir2, strlen(dir2), "b", 1), HU_OK);

    /* Active is now dir2 — the prior dir1 allocation has been freed
     * (ASan would catch a leak if not). */
    size_t plen = 0;
    const char *active = hu_mlx_provider_active_adapter_path(&p, &plen);
    HU_ASSERT_NOT_NULL(active);
    HU_ASSERT_EQ((long)plen, (long)strlen(dir2));
    HU_ASSERT_TRUE(strncmp(active, dir2, plen) == 0);

    rm_rf(dir1);
    rm_rf(dir2);
    free(dir1);
    free(dir2);
    p.vtable->deinit(p.ctx, &alloc);
}

/* ── 6. Initial config adapter_path is reported by accessor ──────────── */

static void load_adapter_reports_initial_config_adapter(void) {
    hu_allocator_t alloc = A();
    const char *init = "/tmp/mlx-load-adapter-initial-path";
    hu_mlx_config_t cfg = {
        .adapter_path = init,
        .adapter_path_len = strlen(init),
    };
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);
    size_t plen = 0;
    const char *active = hu_mlx_provider_active_adapter_path(&p, &plen);
    HU_ASSERT_NOT_NULL(active);
    HU_ASSERT_EQ((long)plen, (long)strlen(init));
    HU_ASSERT_TRUE(strncmp(active, init, plen) == 0);
    p.vtable->deinit(p.ctx, &alloc);
}

/* ── 7. NULL provider → NULL accessor (no crash) ─────────────────────── */

static void active_adapter_path_handles_null_provider(void) {
    HU_ASSERT_NULL((void *)hu_mlx_provider_active_adapter_path(NULL, NULL));
    size_t plen = 42;
    HU_ASSERT_NULL((void *)hu_mlx_provider_active_adapter_path(NULL, &plen));
    /* out_len is touched only when provider is non-NULL — verifying
     * the early-return path doesn't dereference it. The current
     * implementation leaves *out_len untouched on NULL provider; we
     * accept either left-untouched (42) or cleared-to-zero. */
    HU_ASSERT_TRUE(plen == 42 || plen == 0);
}

/* ── 8. Malformed safetensors rejected (B5 negative path) ──────────────── */

static void load_adapter_malformed_safetensors_rejected(void) {
    hu_allocator_t alloc = A();
    hu_mlx_config_t cfg = {0};
    hu_provider_t p = {0};
    HU_ASSERT_EQ(hu_mlx_provider_create(&alloc, &cfg, &p), HU_OK);

    char *dir = make_tempdir();
    HU_ASSERT_NOT_NULL(dir);

    /* Write random bytes to adapters.safetensors — not a valid safetensors
     * file. The load_adapter contract is that malformed files are rejected
     * gracefully (either NOT_FOUND or INVALID_ARGUMENT). */
    char path[1024];
    snprintf(path, sizeof(path), "%s/adapters.safetensors", dir);
    FILE *f = fopen(path, "wb");
    HU_ASSERT_NOT_NULL(f);
    unsigned char garbage[256];
    memset(garbage, 0xAB, sizeof(garbage));
    size_t wrote = fwrite(garbage, 1, sizeof(garbage), f);
    fclose(f);
    HU_ASSERT_EQ(wrote, sizeof(garbage));

    /* Call load_adapter on the malformed file. The contract is that it
     * rejects gracefully (NOT_FOUND if validation fails, INVALID_ARGUMENT
     * if file exists but is corrupt). For now, we verify it doesn't crash
     * and returns an error. */
    hu_error_t err = p.vtable->load_adapter(p.ctx, &alloc, dir, strlen(dir), "corrupt", 7);
    HU_ASSERT_TRUE(err == HU_ERR_NOT_FOUND || err == HU_ERR_INVALID_ARGUMENT);

    /* After a failed load, active_adapter must be NULL (no partial state). */
    size_t plen = 0;
    HU_ASSERT_NULL((void *)hu_mlx_provider_active_adapter_path(&p, &plen));

    rm_rf(dir);
    free(dir);
    p.vtable->deinit(p.ctx, &alloc);
}

void run_mlx_load_adapter_tests(void);
void run_mlx_load_adapter_tests(void) {
    HU_TEST_SUITE("mlx_load_adapter");
    HU_RUN_TEST(load_adapter_null_ctx_returns_invalid_argument);
    HU_RUN_TEST(load_adapter_null_alloc_returns_invalid_argument);
    HU_RUN_TEST(load_adapter_null_path_returns_invalid_argument);
    HU_RUN_TEST(load_adapter_missing_safetensors_returns_not_found);
    HU_RUN_TEST(load_adapter_nonexistent_directory_returns_not_found);
    HU_RUN_TEST(load_adapter_valid_directory_returns_ok_and_persists_path);
    HU_RUN_TEST(load_adapter_failed_swap_preserves_prior_adapter);
    HU_RUN_TEST(load_adapter_replaces_prior_path_on_success);
    HU_RUN_TEST(load_adapter_reports_initial_config_adapter);
    HU_RUN_TEST(active_adapter_path_handles_null_provider);
    HU_RUN_TEST(load_adapter_malformed_safetensors_rejected);
}
