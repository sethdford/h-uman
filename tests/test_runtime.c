/* Runtime adapter tests */
#include "test_framework.h"
#include "seaclaw/runtime.h"
#include "seaclaw/config.h"
#include "seaclaw/platform.h"
#include "seaclaw/core/allocator.h"
#include <string.h>

static void test_runtime_native_create(void) {
    sc_runtime_t r = sc_runtime_native();
    SC_ASSERT_NOT_NULL(r.ctx);
    SC_ASSERT_NOT_NULL(r.vtable);
    SC_ASSERT_STR_EQ(r.vtable->name(r.ctx), "native");
}

static void test_runtime_native_has_shell(void) {
    sc_runtime_t r = sc_runtime_native();
    SC_ASSERT_TRUE(r.vtable->has_shell_access(r.ctx));
}

static void test_runtime_native_has_filesystem(void) {
    sc_runtime_t r = sc_runtime_native();
    SC_ASSERT_TRUE(r.vtable->has_filesystem_access(r.ctx));
}

static void test_runtime_native_supports_long_running(void) {
    sc_runtime_t r = sc_runtime_native();
    SC_ASSERT_TRUE(r.vtable->supports_long_running(r.ctx));
}

static void test_runtime_docker_create(void) {
    sc_runtime_t r = sc_runtime_docker(true, 256);
    SC_ASSERT_NOT_NULL(r.ctx);
    SC_ASSERT_STR_EQ(r.vtable->name(r.ctx), "docker");
}

static void test_runtime_docker_memory_budget(void) {
    sc_runtime_t r = sc_runtime_docker(false, 512);
    uint64_t budget = r.vtable->memory_budget(r.ctx);
    SC_ASSERT_EQ(budget, 512u * 1024 * 1024);
}

static void test_runtime_wasm_create(void) {
    sc_runtime_t r = sc_runtime_wasm(128);
    SC_ASSERT_NOT_NULL(r.ctx);
    SC_ASSERT_STR_EQ(r.vtable->name(r.ctx), "wasm");
}

static void test_runtime_wasm_no_shell(void) {
    sc_runtime_t r = sc_runtime_wasm(64);
    SC_ASSERT_FALSE(r.vtable->has_shell_access(r.ctx));
}

static void test_runtime_wasm_memory_budget(void) {
    sc_runtime_t r = sc_runtime_wasm(256);
    uint64_t budget = r.vtable->memory_budget(r.ctx);
    SC_ASSERT_EQ(budget, 256u * 1024 * 1024);
}

static void test_runtime_cloudflare_create(void) {
    sc_runtime_t r = sc_runtime_cloudflare();
    SC_ASSERT_NOT_NULL(r.ctx);
    SC_ASSERT_STR_EQ(r.vtable->name(r.ctx), "cloudflare");
}

static void test_platform_is_windows_or_unix(void) {
    bool win = sc_platform_is_windows();
    bool unix = sc_platform_is_unix();
    SC_ASSERT_TRUE(win != unix);
}

static void test_platform_get_shell(void) {
    const char *shell = sc_platform_get_shell();
    SC_ASSERT_NOT_NULL(shell);
    SC_ASSERT_TRUE(strlen(shell) > 0);
}

static void test_platform_get_shell_flag(void) {
    const char *flag = sc_platform_get_shell_flag();
    SC_ASSERT_NOT_NULL(flag);
}

static void test_platform_get_temp_dir(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *tmp = sc_platform_get_temp_dir(&alloc);
    SC_ASSERT_NOT_NULL(tmp);
    SC_ASSERT_TRUE(strlen(tmp) > 0);
    alloc.free(alloc.ctx, tmp, strlen(tmp) + 1);
}

static void test_platform_get_home_dir(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *home = sc_platform_get_home_dir(&alloc);
    SC_ASSERT_NOT_NULL(home);
    SC_ASSERT_TRUE(strlen(home) > 0);
    alloc.free(alloc.ctx, home, strlen(home) + 1);
}

static void test_runtime_docker_storage_path(void) {
    sc_runtime_t r = sc_runtime_docker(true, 128);
    const char *path = r.vtable->storage_path(r.ctx);
    SC_ASSERT_NOT_NULL(path);
}

static void test_runtime_native_storage_path(void) {
    sc_runtime_t r = sc_runtime_native();
    const char *path = r.vtable->storage_path(r.ctx);
    SC_ASSERT_NOT_NULL(path);
}

static void test_runtime_wasm_no_long_running(void) {
    sc_runtime_t r = sc_runtime_wasm(64);
    SC_ASSERT_FALSE(r.vtable->supports_long_running(r.ctx));
}

static void test_runtime_docker_mount_workspace(void) {
    sc_runtime_t r_mount = sc_runtime_docker(true, 64);
    sc_runtime_t r_no_mount = sc_runtime_docker(false, 64);
    SC_ASSERT_NOT_NULL(r_mount.ctx);
    SC_ASSERT_NOT_NULL(r_no_mount.ctx);
}

static void test_runtime_native_memory_budget(void) {
    sc_runtime_t r = sc_runtime_native();
    uint64_t budget = r.vtable->memory_budget(r.ctx);
    SC_ASSERT(budget >= 0);
}

static void test_runtime_native_storage_contains_seaclaw(void) {
    sc_runtime_t r = sc_runtime_native();
    const char *path = r.vtable->storage_path(r.ctx);
    SC_ASSERT_NOT_NULL(path);
    SC_ASSERT_TRUE(strstr(path, "seaclaw") != NULL);
}

static void test_runtime_docker_has_shell(void) {
    sc_runtime_t r = sc_runtime_docker(false, 0);
    SC_ASSERT_TRUE(r.vtable->has_shell_access(r.ctx));
}

static void test_runtime_docker_no_long_running(void) {
    sc_runtime_t r = sc_runtime_docker(true, 128);
    SC_ASSERT_FALSE(r.vtable->supports_long_running(r.ctx));
}

static void test_runtime_docker_fs_with_mount(void) {
    sc_runtime_t r = sc_runtime_docker(true, 64);
    SC_ASSERT_TRUE(r.vtable->has_filesystem_access(r.ctx));
    const char *path = r.vtable->storage_path(r.ctx);
    SC_ASSERT_TRUE(strstr(path, "workspace") != NULL);
}

static void test_runtime_docker_fs_without_mount(void) {
    sc_runtime_t r = sc_runtime_docker(false, 64);
    SC_ASSERT_FALSE(r.vtable->has_filesystem_access(r.ctx));
    const char *path = r.vtable->storage_path(r.ctx);
    SC_ASSERT_TRUE(strstr(path, "tmp") != NULL);
}

static void test_runtime_docker_memory_zero_when_unlimited(void) {
    sc_runtime_t r = sc_runtime_docker(false, 0);
    uint64_t budget = r.vtable->memory_budget(r.ctx);
    SC_ASSERT_EQ(budget, 0u);
}

static void test_runtime_cloudflare_all_vtable_methods(void) {
    sc_runtime_t r = sc_runtime_cloudflare();
    SC_ASSERT_STR_EQ(r.vtable->name(r.ctx), "cloudflare");
    SC_ASSERT_FALSE(r.vtable->has_shell_access(r.ctx));
    SC_ASSERT_FALSE(r.vtable->has_filesystem_access(r.ctx));
    SC_ASSERT_STR_EQ(r.vtable->storage_path(r.ctx), "");
    SC_ASSERT_FALSE(r.vtable->supports_long_running(r.ctx));
    SC_ASSERT_EQ(r.vtable->memory_budget(r.ctx), 128u * 1024 * 1024);
}

static void test_runtime_wasm_storage_path(void) {
    sc_runtime_t r = sc_runtime_wasm(64);
    const char *path = r.vtable->storage_path(r.ctx);
    SC_ASSERT_NOT_NULL(path);
    SC_ASSERT_TRUE(strlen(path) > 0);
}

static void test_runtime_wasm_no_fs(void) {
    sc_runtime_t r = sc_runtime_wasm(64);
    SC_ASSERT_FALSE(r.vtable->has_filesystem_access(r.ctx));
}

static void test_runtime_docker_storage_workspace_path(void) {
    sc_runtime_t r = sc_runtime_docker(true, 64);
    const char *path = r.vtable->storage_path(r.ctx);
    SC_ASSERT_STR_EQ(path, "/workspace/.seaclaw");
}

static void test_runtime_docker_storage_tmp_path(void) {
    sc_runtime_t r = sc_runtime_docker(false, 64);
    const char *path = r.vtable->storage_path(r.ctx);
    SC_ASSERT_STR_EQ(path, "/tmp/.seaclaw");
}

static void test_runtime_vtable_dispatch_native(void) {
    sc_runtime_t r = sc_runtime_native();
    SC_ASSERT_TRUE(strlen(r.vtable->name(r.ctx)) > 0);
    (void)r.vtable->has_shell_access(r.ctx);
    (void)r.vtable->has_filesystem_access(r.ctx);
    (void)r.vtable->storage_path(r.ctx);
    (void)r.vtable->supports_long_running(r.ctx);
    (void)r.vtable->memory_budget(r.ctx);
}

static void test_runtime_vtable_dispatch_docker(void) {
    sc_runtime_t r = sc_runtime_docker(true, 256);
    SC_ASSERT_TRUE(strlen(r.vtable->name(r.ctx)) > 0);
    (void)r.vtable->has_shell_access(r.ctx);
    (void)r.vtable->has_filesystem_access(r.ctx);
    (void)r.vtable->storage_path(r.ctx);
    (void)r.vtable->supports_long_running(r.ctx);
    (void)r.vtable->memory_budget(r.ctx);
}

static void test_runtime_kind_enum_values(void) {
    sc_runtime_kind_t k1 = SC_RUNTIME_NATIVE;
    sc_runtime_kind_t k2 = SC_RUNTIME_DOCKER;
    sc_runtime_kind_t k3 = SC_RUNTIME_WASM;
    sc_runtime_kind_t k4 = SC_RUNTIME_CLOUDFLARE;
    SC_ASSERT_EQ((int)k1, 0);
    SC_ASSERT_TRUE(k1 != k2);
    SC_ASSERT_TRUE(k2 != k3);
    SC_ASSERT_TRUE(k3 != k4);
}

static void test_runtime_from_config_native(void) {
    sc_config_t cfg = {0};
    cfg.runtime.kind = "native";
    sc_runtime_t r;
    sc_error_t err = sc_runtime_from_config(&cfg, &r);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(r.vtable);
    SC_ASSERT_STR_EQ(r.vtable->name(r.ctx), "native");
}

static void test_runtime_from_config_null_defaults_native(void) {
    sc_config_t cfg = {0};
    cfg.runtime.kind = NULL;
    sc_runtime_t r;
    sc_error_t err = sc_runtime_from_config(&cfg, &r);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(r.vtable->name(r.ctx), "native");
}

static void test_runtime_from_config_docker(void) {
    sc_config_t cfg = {0};
    cfg.runtime.kind = "docker";
    sc_runtime_t r;
    sc_error_t err = sc_runtime_from_config(&cfg, &r);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(r.vtable->name(r.ctx), "docker");
}

static void test_runtime_from_config_wasm(void) {
    sc_config_t cfg = {0};
    cfg.runtime.kind = "wasm";
    sc_runtime_t r;
    sc_error_t err = sc_runtime_from_config(&cfg, &r);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(r.vtable->name(r.ctx), "wasm");
}

static void test_runtime_from_config_cloudflare(void) {
    sc_config_t cfg = {0};
    cfg.runtime.kind = "cloudflare";
    sc_runtime_t r;
    sc_error_t err = sc_runtime_from_config(&cfg, &r);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(r.vtable->name(r.ctx), "cloudflare");
}

static void test_runtime_from_config_unknown_returns_error(void) {
    sc_config_t cfg = {0};
    cfg.runtime.kind = "unknown_runtime_xyz";
    sc_runtime_t r;
    sc_error_t err = sc_runtime_from_config(&cfg, &r);
    SC_ASSERT_NEQ(err, SC_OK);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
}

static void test_runtime_from_config_null_config_returns_error(void) {
    sc_runtime_t r;
    sc_error_t err = sc_runtime_from_config(NULL, &r);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_runtime_from_config_null_out_returns_error(void) {
    sc_config_t cfg = {0};
    cfg.runtime.kind = "native";
    sc_error_t err = sc_runtime_from_config(&cfg, NULL);
    SC_ASSERT_NEQ(err, SC_OK);
}

void run_runtime_tests(void) {
    SC_TEST_SUITE("Runtime");
    SC_RUN_TEST(test_runtime_native_create);
    SC_RUN_TEST(test_runtime_native_has_shell);
    SC_RUN_TEST(test_runtime_native_has_filesystem);
    SC_RUN_TEST(test_runtime_native_supports_long_running);
    SC_RUN_TEST(test_runtime_native_storage_path);
    SC_RUN_TEST(test_runtime_docker_create);
    SC_RUN_TEST(test_runtime_docker_memory_budget);
    SC_RUN_TEST(test_runtime_docker_storage_path);
    SC_RUN_TEST(test_runtime_docker_mount_workspace);
    SC_RUN_TEST(test_runtime_wasm_create);
    SC_RUN_TEST(test_runtime_wasm_no_shell);
    SC_RUN_TEST(test_runtime_wasm_memory_budget);
    SC_RUN_TEST(test_runtime_wasm_no_long_running);
    SC_RUN_TEST(test_runtime_cloudflare_create);
    SC_RUN_TEST(test_runtime_native_memory_budget);
    SC_RUN_TEST(test_runtime_native_storage_contains_seaclaw);
    SC_RUN_TEST(test_runtime_docker_has_shell);
    SC_RUN_TEST(test_runtime_docker_no_long_running);
    SC_RUN_TEST(test_runtime_docker_fs_with_mount);
    SC_RUN_TEST(test_runtime_docker_fs_without_mount);
    SC_RUN_TEST(test_runtime_docker_memory_zero_when_unlimited);
    SC_RUN_TEST(test_runtime_docker_storage_workspace_path);
    SC_RUN_TEST(test_runtime_docker_storage_tmp_path);
    SC_RUN_TEST(test_runtime_cloudflare_all_vtable_methods);
    SC_RUN_TEST(test_runtime_wasm_storage_path);
    SC_RUN_TEST(test_runtime_wasm_no_fs);
    SC_RUN_TEST(test_runtime_vtable_dispatch_native);
    SC_RUN_TEST(test_runtime_vtable_dispatch_docker);
    SC_RUN_TEST(test_runtime_kind_enum_values);
    SC_RUN_TEST(test_runtime_from_config_native);
    SC_RUN_TEST(test_runtime_from_config_null_defaults_native);
    SC_RUN_TEST(test_runtime_from_config_docker);
    SC_RUN_TEST(test_runtime_from_config_wasm);
    SC_RUN_TEST(test_runtime_from_config_cloudflare);
    SC_RUN_TEST(test_runtime_from_config_unknown_returns_error);
    SC_RUN_TEST(test_runtime_from_config_null_config_returns_error);
    SC_RUN_TEST(test_runtime_from_config_null_out_returns_error);
    SC_RUN_TEST(test_platform_is_windows_or_unix);
    SC_RUN_TEST(test_platform_get_shell);
    SC_RUN_TEST(test_platform_get_shell_flag);
    SC_RUN_TEST(test_platform_get_temp_dir);
    SC_RUN_TEST(test_platform_get_home_dir);
}
