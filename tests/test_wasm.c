/* WASM allocator, WASI binding, and WASM runtime vtable tests.
 * Runtime tests run on native (wasm_rt.c is always compiled).
 * Alloc/WASI tests run only when __wasi__ (WASM build). */
#include "test_framework.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Native-side WASM runtime vtable tests (always run) */
#include "human/runtime.h"

static void test_wasm_runtime_name(void) {
    hu_runtime_t rt = hu_runtime_wasm(64);
    HU_ASSERT_NOT_NULL(rt.vtable);
    HU_ASSERT_NOT_NULL(rt.ctx);
    const char *name = rt.vtable->name(rt.ctx);
    HU_ASSERT_NOT_NULL(name);
    HU_ASSERT_STR_EQ(name, "wasm");
}

static void test_wasm_runtime_has_shell_access(void) {
    hu_runtime_t rt = hu_runtime_wasm(0);
    HU_ASSERT_FALSE(rt.vtable->has_shell_access(rt.ctx));
}

static void test_wasm_runtime_has_filesystem_access(void) {
    hu_runtime_t rt = hu_runtime_wasm(0);
    /* WASM has limited fs (preopen dirs only), vtable reports false for no full access */
    HU_ASSERT_FALSE(rt.vtable->has_filesystem_access(rt.ctx));
}

static void test_wasm_runtime_memory_budget(void) {
    hu_runtime_t rt = hu_runtime_wasm(0);
    uint64_t budget = rt.vtable->memory_budget(rt.ctx);
    HU_ASSERT(budget > 0);
    HU_ASSERT_EQ(budget, 64ULL * 1024 * 1024); /* default 64 MB */
}

static void test_wasm_runtime_memory_budget_custom(void) {
    hu_runtime_t rt = hu_runtime_wasm(128);
    uint64_t budget = rt.vtable->memory_budget(rt.ctx);
    HU_ASSERT_EQ(budget, 128ULL * 1024 * 1024);
}

static void test_wasm_runtime_storage_path(void) {
    hu_runtime_t rt = hu_runtime_wasm(0);
    const char *path = rt.vtable->storage_path(rt.ctx);
    HU_ASSERT_NOT_NULL(path);
    HU_ASSERT_STR_EQ(path, ".human/wasm");
}

static void test_wasm_runtime_supports_long_running(void) {
    hu_runtime_t rt = hu_runtime_wasm(0);
    HU_ASSERT_FALSE(rt.vtable->supports_long_running(rt.ctx));
}

static void test_cloudflare_runtime(void) {
    hu_runtime_t rt = hu_runtime_cloudflare();
    HU_ASSERT_NOT_NULL(rt.vtable);
    HU_ASSERT_STR_EQ(rt.vtable->name(rt.ctx), "cloudflare");
    HU_ASSERT_EQ(rt.vtable->has_shell_access(rt.ctx), false);
    HU_ASSERT_EQ(rt.vtable->has_filesystem_access(rt.ctx), false);
    HU_ASSERT_STR_EQ(rt.vtable->storage_path(rt.ctx), "");
    HU_ASSERT_EQ(rt.vtable->supports_long_running(rt.ctx), false);
    HU_ASSERT_EQ(rt.vtable->memory_budget(rt.ctx), 128ULL * 1024 * 1024);
}

/* wasm_alloc tests: run on native when HU_IS_TEST (wasm_alloc.c compiles for tests) */
#if defined(HU_IS_TEST)
#include "human/core/allocator.h"
#include "human/wasm/wasm_alloc.h"

static unsigned char test_alloc_buf[4096];

static void test_wasm_alloc_basic(void) {
    hu_wasm_alloc_ctx_t *ctx =
        hu_wasm_alloc_ctx_create(test_alloc_buf, sizeof(test_alloc_buf), 2048);
    HU_ASSERT_NOT_NULL(ctx);

    hu_allocator_t alloc = hu_wasm_allocator(ctx);
    HU_ASSERT_NOT_NULL(alloc.alloc);

    void *p1 = alloc.alloc(alloc.ctx, 64);
    HU_ASSERT_NOT_NULL(p1);
    memset(p1, 0xAB, 64);

    void *p2 = alloc.alloc(alloc.ctx, 128);
    HU_ASSERT_NOT_NULL(p2);

    HU_ASSERT_EQ(hu_wasm_allocator_used(ctx), 192);
    HU_ASSERT_EQ(hu_wasm_allocator_limit(ctx), 2048);

    alloc.free(alloc.ctx, p1, 64); /* no-op for bump */
    HU_ASSERT_EQ(hu_wasm_allocator_used(ctx), 192);
}
#endif /* HU_IS_TEST */

#ifdef __wasi__

#include "human/core/allocator.h"
#include "human/wasm/wasi_bindings.h"
#include "human/wasm/wasm_alloc.h"

static unsigned char test_alloc_buf_wasi[4096];

static void test_wasm_bump_allocator_basic(void) {
    hu_wasm_alloc_ctx_t *ctx =
        hu_wasm_alloc_ctx_create(test_alloc_buf_wasi, sizeof(test_alloc_buf_wasi), 2048);
    HU_ASSERT_NOT_NULL(ctx);

    hu_allocator_t alloc = hu_wasm_allocator(ctx);
    HU_ASSERT_NOT_NULL(alloc.alloc);

    void *p1 = alloc.alloc(alloc.ctx, 64);
    HU_ASSERT_NOT_NULL(p1);
    memset(p1, 0xAB, 64);

    void *p2 = alloc.alloc(alloc.ctx, 128);
    HU_ASSERT_NOT_NULL(p2);

    HU_ASSERT_EQ(hu_wasm_allocator_used(ctx), 192);
    HU_ASSERT_EQ(hu_wasm_allocator_limit(ctx), 2048);

    alloc.free(alloc.ctx, p1, 64); /* no-op for bump */
    HU_ASSERT_EQ(hu_wasm_allocator_used(ctx), 192);
}

static void test_wasm_bump_allocator_realloc(void) {
    hu_wasm_alloc_ctx_t *ctx =
        hu_wasm_alloc_ctx_create(test_alloc_buf_wasi, sizeof(test_alloc_buf_wasi), 1024);
    hu_allocator_t alloc = hu_wasm_allocator(ctx);

    void *p = alloc.alloc(alloc.ctx, 32);
    HU_ASSERT_NOT_NULL(p);
    memset(p, 0x42, 32);

    void *p2 = alloc.realloc(alloc.ctx, p, 32, 96);
    HU_ASSERT_NOT_NULL(p2);
    unsigned char *bytes = (unsigned char *)p2;
    for (int i = 0; i < 32; i++)
        HU_ASSERT_EQ(bytes[i], 0x42);
}

static void test_wasm_bump_allocator_limit(void) {
    hu_wasm_alloc_ctx_t *ctx =
        hu_wasm_alloc_ctx_create(test_alloc_buf_wasi, sizeof(test_alloc_buf_wasi), 256);
    hu_allocator_t alloc = hu_wasm_allocator(ctx);

    void *p1 = alloc.alloc(alloc.ctx, 200);
    HU_ASSERT_NOT_NULL(p1);

    void *p2 = alloc.alloc(alloc.ctx, 100); /* over limit */
    HU_ASSERT_NULL(p2);
}

static void test_wasi_clock_time_get(void) {
    uint64_t ns = 0;
    int r = hu_wasi_clock_time_get_realtime(&ns);
    HU_ASSERT_EQ(r, 0);
    HU_ASSERT(ns > 0);
}

static void test_wasi_random_get(void) {
    unsigned char buf[32];
    int r = hu_wasi_random_get(buf, sizeof(buf));
    HU_ASSERT_EQ(r, 0);
    int same = 1;
    for (size_t i = 1; i < sizeof(buf); i++) {
        if (buf[i] != buf[0]) {
            same = 0;
            break;
        }
    }
    HU_ASSERT_FALSE(same); /* very unlikely all bytes identical */
}

static void test_wasi_args_sizes_get(void) {
    size_t argc = 0, buf_len = 0;
    int r = hu_wasi_args_sizes_get(&argc, &buf_len);
    HU_ASSERT_EQ(r, 0);
    HU_ASSERT_TRUE(argc >= 1);
}

void run_wasm_tests(void) {
    HU_TEST_SUITE("wasm");
    /* Native runtime vtable tests (run on both native and WASM build) */
    HU_RUN_TEST(test_wasm_runtime_name);
    HU_RUN_TEST(test_wasm_runtime_has_shell_access);
    HU_RUN_TEST(test_wasm_runtime_has_filesystem_access);
    HU_RUN_TEST(test_wasm_runtime_memory_budget);
    HU_RUN_TEST(test_wasm_runtime_memory_budget_custom);
    HU_RUN_TEST(test_wasm_runtime_storage_path);
    HU_RUN_TEST(test_wasm_runtime_supports_long_running);
    HU_RUN_TEST(test_cloudflare_runtime);
    /* WASI-specific tests (only when __wasi__) */
    HU_RUN_TEST(test_wasm_bump_allocator_basic);
    HU_RUN_TEST(test_wasm_bump_allocator_realloc);
    HU_RUN_TEST(test_wasm_bump_allocator_limit);
    HU_RUN_TEST(test_wasi_clock_time_get);
    HU_RUN_TEST(test_wasi_random_get);
    HU_RUN_TEST(test_wasi_args_sizes_get);
}

#else

/* When not building for WASI: run runtime vtable + alloc tests (alloc compiles for HU_IS_TEST). */
void run_wasm_tests(void) {
    HU_TEST_SUITE("wasm");
    HU_RUN_TEST(test_wasm_runtime_name);
    HU_RUN_TEST(test_wasm_runtime_has_shell_access);
    HU_RUN_TEST(test_wasm_runtime_has_filesystem_access);
    HU_RUN_TEST(test_wasm_runtime_memory_budget);
    HU_RUN_TEST(test_wasm_runtime_memory_budget_custom);
    HU_RUN_TEST(test_wasm_runtime_storage_path);
    HU_RUN_TEST(test_wasm_runtime_supports_long_running);
    HU_RUN_TEST(test_cloudflare_runtime);
    HU_RUN_TEST(test_wasm_alloc_basic);
    printf("  SKIP  wasm WASI syscall tests (build with wasm32-wasi to run)\n");
}

#endif /* __wasi__ */
