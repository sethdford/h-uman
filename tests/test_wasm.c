/* WASM allocator, WASI binding, and WASM runtime vtable tests.
 * Runtime tests run on native (wasm_rt.c is always compiled).
 * Alloc/WASI tests run only when __wasi__ (WASM build). */
#include "test_framework.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* Native-side WASM runtime vtable tests (always run) */
#include "seaclaw/runtime.h"

static void test_wasm_runtime_name(void) {
    sc_runtime_t rt = sc_runtime_wasm(64);
    SC_ASSERT_NOT_NULL(rt.vtable);
    SC_ASSERT_NOT_NULL(rt.ctx);
    const char *name = rt.vtable->name(rt.ctx);
    SC_ASSERT_NOT_NULL(name);
    SC_ASSERT_STR_EQ(name, "wasm");
}

static void test_wasm_runtime_has_shell_access(void) {
    sc_runtime_t rt = sc_runtime_wasm(0);
    SC_ASSERT_FALSE(rt.vtable->has_shell_access(rt.ctx));
}

static void test_wasm_runtime_has_filesystem_access(void) {
    sc_runtime_t rt = sc_runtime_wasm(0);
    /* WASM has limited fs (preopen dirs only), vtable reports false for no full access */
    SC_ASSERT_FALSE(rt.vtable->has_filesystem_access(rt.ctx));
}

static void test_wasm_runtime_memory_budget(void) {
    sc_runtime_t rt = sc_runtime_wasm(0);
    uint64_t budget = rt.vtable->memory_budget(rt.ctx);
    SC_ASSERT(budget > 0);
    SC_ASSERT_EQ(budget, 64ULL * 1024 * 1024);  /* default 64 MB */
}

static void test_wasm_runtime_memory_budget_custom(void) {
    sc_runtime_t rt = sc_runtime_wasm(128);
    uint64_t budget = rt.vtable->memory_budget(rt.ctx);
    SC_ASSERT_EQ(budget, 128ULL * 1024 * 1024);
}

static void test_wasm_runtime_storage_path(void) {
    sc_runtime_t rt = sc_runtime_wasm(0);
    const char *path = rt.vtable->storage_path(rt.ctx);
    SC_ASSERT_NOT_NULL(path);
    SC_ASSERT_STR_EQ(path, ".seaclaw/wasm");
}

static void test_wasm_runtime_supports_long_running(void) {
    sc_runtime_t rt = sc_runtime_wasm(0);
    SC_ASSERT_FALSE(rt.vtable->supports_long_running(rt.ctx));
}

static void test_cloudflare_runtime(void) {
    sc_runtime_t rt = sc_runtime_cloudflare();
    SC_ASSERT_NOT_NULL(rt.vtable);
    SC_ASSERT_STR_EQ(rt.vtable->name(rt.ctx), "cloudflare");
    SC_ASSERT_EQ(rt.vtable->has_shell_access(rt.ctx), false);
    SC_ASSERT_EQ(rt.vtable->has_filesystem_access(rt.ctx), false);
    SC_ASSERT_STR_EQ(rt.vtable->storage_path(rt.ctx), "");
    SC_ASSERT_EQ(rt.vtable->supports_long_running(rt.ctx), false);
    SC_ASSERT_EQ(rt.vtable->memory_budget(rt.ctx), 128ULL * 1024 * 1024);
}

/* wasm_alloc tests: run on native when SC_IS_TEST (wasm_alloc.c compiles for tests) */
#if defined(SC_IS_TEST)
#include "seaclaw/core/allocator.h"
#include "seaclaw/wasm/wasm_alloc.h"

static unsigned char test_alloc_buf[4096];

static void test_wasm_alloc_basic(void) {
    sc_wasm_alloc_ctx_t *ctx = sc_wasm_alloc_ctx_create(
        test_alloc_buf, sizeof(test_alloc_buf), 2048);
    SC_ASSERT_NOT_NULL(ctx);

    sc_allocator_t alloc = sc_wasm_allocator(ctx);
    SC_ASSERT_NOT_NULL(alloc.alloc);

    void *p1 = alloc.alloc(alloc.ctx, 64);
    SC_ASSERT_NOT_NULL(p1);
    memset(p1, 0xAB, 64);

    void *p2 = alloc.alloc(alloc.ctx, 128);
    SC_ASSERT_NOT_NULL(p2);

    SC_ASSERT_EQ(sc_wasm_allocator_used(ctx), 192);
    SC_ASSERT_EQ(sc_wasm_allocator_limit(ctx), 2048);

    alloc.free(alloc.ctx, p1, 64);  /* no-op for bump */
    SC_ASSERT_EQ(sc_wasm_allocator_used(ctx), 192);
}
#endif /* SC_IS_TEST */

#ifdef __wasi__

#include "seaclaw/core/allocator.h"
#include "seaclaw/wasm/wasm_alloc.h"
#include "seaclaw/wasm/wasi_bindings.h"

static unsigned char test_alloc_buf_wasi[4096];

static void test_wasm_bump_allocator_basic(void) {
    sc_wasm_alloc_ctx_t *ctx = sc_wasm_alloc_ctx_create(
        test_alloc_buf_wasi, sizeof(test_alloc_buf_wasi), 2048);
    SC_ASSERT_NOT_NULL(ctx);

    sc_allocator_t alloc = sc_wasm_allocator(ctx);
    SC_ASSERT_NOT_NULL(alloc.alloc);

    void *p1 = alloc.alloc(alloc.ctx, 64);
    SC_ASSERT_NOT_NULL(p1);
    memset(p1, 0xAB, 64);

    void *p2 = alloc.alloc(alloc.ctx, 128);
    SC_ASSERT_NOT_NULL(p2);

    SC_ASSERT_EQ(sc_wasm_allocator_used(ctx), 192);
    SC_ASSERT_EQ(sc_wasm_allocator_limit(ctx), 2048);

    alloc.free(alloc.ctx, p1, 64);  /* no-op for bump */
    SC_ASSERT_EQ(sc_wasm_allocator_used(ctx), 192);
}

static void test_wasm_bump_allocator_realloc(void) {
    sc_wasm_alloc_ctx_t *ctx = sc_wasm_alloc_ctx_create(
        test_alloc_buf_wasi, sizeof(test_alloc_buf_wasi), 1024);
    sc_allocator_t alloc = sc_wasm_allocator(ctx);

    void *p = alloc.alloc(alloc.ctx, 32);
    SC_ASSERT_NOT_NULL(p);
    memset(p, 0x42, 32);

    void *p2 = alloc.realloc(alloc.ctx, p, 32, 96);
    SC_ASSERT_NOT_NULL(p2);
    unsigned char *bytes = (unsigned char *)p2;
    for (int i = 0; i < 32; i++) SC_ASSERT_EQ(bytes[i], 0x42);
}

static void test_wasm_bump_allocator_limit(void) {
    sc_wasm_alloc_ctx_t *ctx = sc_wasm_alloc_ctx_create(
        test_alloc_buf_wasi, sizeof(test_alloc_buf_wasi), 256);
    sc_allocator_t alloc = sc_wasm_allocator(ctx);

    void *p1 = alloc.alloc(alloc.ctx, 200);
    SC_ASSERT_NOT_NULL(p1);

    void *p2 = alloc.alloc(alloc.ctx, 100);  /* over limit */
    SC_ASSERT_NULL(p2);
}

static void test_wasi_clock_time_get(void) {
    uint64_t ns = 0;
    int r = sc_wasi_clock_time_get_realtime(&ns);
    SC_ASSERT_EQ(r, 0);
    SC_ASSERT(ns > 0);
}

static void test_wasi_random_get(void) {
    unsigned char buf[32];
    int r = sc_wasi_random_get(buf, sizeof(buf));
    SC_ASSERT_EQ(r, 0);
    int same = 1;
    for (size_t i = 1; i < sizeof(buf); i++) {
        if (buf[i] != buf[0]) { same = 0; break; }
    }
    SC_ASSERT_FALSE(same);  /* very unlikely all bytes identical */
}

static void test_wasi_args_sizes_get(void) {
    size_t argc = 0, buf_len = 0;
    int r = sc_wasi_args_sizes_get(&argc, &buf_len);
    SC_ASSERT_EQ(r, 0);
    SC_ASSERT_TRUE(argc >= 1);
}

void run_wasm_tests(void) {
    SC_TEST_SUITE("wasm");
    /* Native runtime vtable tests (run on both native and WASM build) */
    SC_RUN_TEST(test_wasm_runtime_name);
    SC_RUN_TEST(test_wasm_runtime_has_shell_access);
    SC_RUN_TEST(test_wasm_runtime_has_filesystem_access);
    SC_RUN_TEST(test_wasm_runtime_memory_budget);
    SC_RUN_TEST(test_wasm_runtime_memory_budget_custom);
    SC_RUN_TEST(test_wasm_runtime_storage_path);
    SC_RUN_TEST(test_wasm_runtime_supports_long_running);
    SC_RUN_TEST(test_cloudflare_runtime);
    /* WASI-specific tests (only when __wasi__) */
    SC_RUN_TEST(test_wasm_bump_allocator_basic);
    SC_RUN_TEST(test_wasm_bump_allocator_realloc);
    SC_RUN_TEST(test_wasm_bump_allocator_limit);
    SC_RUN_TEST(test_wasi_clock_time_get);
    SC_RUN_TEST(test_wasi_random_get);
    SC_RUN_TEST(test_wasi_args_sizes_get);
}

#else

/* When not building for WASI: run runtime vtable + alloc tests (alloc compiles for SC_IS_TEST). */
void run_wasm_tests(void) {
    SC_TEST_SUITE("wasm");
    SC_RUN_TEST(test_wasm_runtime_name);
    SC_RUN_TEST(test_wasm_runtime_has_shell_access);
    SC_RUN_TEST(test_wasm_runtime_has_filesystem_access);
    SC_RUN_TEST(test_wasm_runtime_memory_budget);
    SC_RUN_TEST(test_wasm_runtime_memory_budget_custom);
    SC_RUN_TEST(test_wasm_runtime_storage_path);
    SC_RUN_TEST(test_wasm_runtime_supports_long_running);
    SC_RUN_TEST(test_cloudflare_runtime);
    SC_RUN_TEST(test_wasm_alloc_basic);
    printf("  SKIP  wasm WASI syscall tests (build with wasm32-wasi to run)\n");
}

#endif /* __wasi__ */
