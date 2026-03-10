/* Runtime adapter tests */
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/platform.h"
#include "human/runtime.h"
#include "test_framework.h"
#include <string.h>

static void test_runtime_native_create(void) {
    hu_runtime_t r = hu_runtime_native();
    HU_ASSERT_NOT_NULL(r.ctx);
    HU_ASSERT_NOT_NULL(r.vtable);
    HU_ASSERT_STR_EQ(r.vtable->name(r.ctx), "native");
}

static void test_runtime_native_has_shell(void) {
    hu_runtime_t r = hu_runtime_native();
    HU_ASSERT_TRUE(r.vtable->has_shell_access(r.ctx));
}

static void test_runtime_native_has_filesystem(void) {
    hu_runtime_t r = hu_runtime_native();
    HU_ASSERT_TRUE(r.vtable->has_filesystem_access(r.ctx));
}

static void test_runtime_native_supports_long_running(void) {
    hu_runtime_t r = hu_runtime_native();
    HU_ASSERT_TRUE(r.vtable->supports_long_running(r.ctx));
}

static void test_runtime_docker_create(void) {
    hu_runtime_t r = hu_runtime_docker(true, 256, "alpine:latest", ".");
    HU_ASSERT_NOT_NULL(r.ctx);
    HU_ASSERT_STR_EQ(r.vtable->name(r.ctx), "docker");
}

static void test_runtime_docker_memory_budget(void) {
    hu_runtime_t r = hu_runtime_docker(false, 512, "alpine:latest", ".");
    uint64_t budget = r.vtable->memory_budget(r.ctx);
    HU_ASSERT_EQ(budget, 512u * 1024 * 1024);
}

static void test_runtime_wasm_create(void) {
    hu_runtime_t r = hu_runtime_wasm(128);
    HU_ASSERT_NOT_NULL(r.ctx);
    HU_ASSERT_STR_EQ(r.vtable->name(r.ctx), "wasm");
}

static void test_runtime_wasm_no_shell(void) {
    hu_runtime_t r = hu_runtime_wasm(64);
    HU_ASSERT_FALSE(r.vtable->has_shell_access(r.ctx));
}

static void test_runtime_wasm_memory_budget(void) {
    hu_runtime_t r = hu_runtime_wasm(256);
    uint64_t budget = r.vtable->memory_budget(r.ctx);
    HU_ASSERT_EQ(budget, 256u * 1024 * 1024);
}

static void test_runtime_cloudflare_create(void) {
    hu_runtime_t r = hu_runtime_cloudflare();
    HU_ASSERT_NOT_NULL(r.ctx);
    HU_ASSERT_STR_EQ(r.vtable->name(r.ctx), "cloudflare");
}

static void test_platform_is_windows_or_unix(void) {
    bool is_win = hu_platform_is_windows();
    bool is_unix = hu_platform_is_unix();
    HU_ASSERT_TRUE(is_win != is_unix);
}

static void test_platform_get_shell(void) {
    const char *shell = hu_platform_get_shell();
    HU_ASSERT_NOT_NULL(shell);
    HU_ASSERT_TRUE(strlen(shell) > 0);
}

static void test_platform_get_shell_flag(void) {
    const char *flag = hu_platform_get_shell_flag();
    HU_ASSERT_NOT_NULL(flag);
}

static void test_platform_get_temp_dir(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *tmp = hu_platform_get_temp_dir(&alloc);
    HU_ASSERT_NOT_NULL(tmp);
    HU_ASSERT_TRUE(strlen(tmp) > 0);
    alloc.free(alloc.ctx, tmp, strlen(tmp) + 1);
}

static void test_platform_get_home_dir(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *home = hu_platform_get_home_dir(&alloc);
    HU_ASSERT_NOT_NULL(home);
    HU_ASSERT_TRUE(strlen(home) > 0);
    alloc.free(alloc.ctx, home, strlen(home) + 1);
}

static void test_runtime_docker_storage_path(void) {
    hu_runtime_t r = hu_runtime_docker(true, 128, "alpine:latest", ".");
    const char *path = r.vtable->storage_path(r.ctx);
    HU_ASSERT_NOT_NULL(path);
}

static void test_runtime_native_storage_path(void) {
    hu_runtime_t r = hu_runtime_native();
    const char *path = r.vtable->storage_path(r.ctx);
    HU_ASSERT_NOT_NULL(path);
}

static void test_runtime_wasm_no_long_running(void) {
    hu_runtime_t r = hu_runtime_wasm(64);
    HU_ASSERT_FALSE(r.vtable->supports_long_running(r.ctx));
}

static void test_runtime_docker_mount_workspace(void) {
    hu_runtime_t r_mount = hu_runtime_docker(true, 64, "alpine:latest", ".");
    hu_runtime_t r_no_mount = hu_runtime_docker(false, 64, "alpine:latest", ".");
    HU_ASSERT_NOT_NULL(r_mount.ctx);
    HU_ASSERT_NOT_NULL(r_no_mount.ctx);
}

static void test_runtime_native_memory_budget(void) {
    hu_runtime_t r = hu_runtime_native();
    uint64_t budget = r.vtable->memory_budget(r.ctx);
    HU_ASSERT(budget >= 0);
}

static void test_runtime_native_storage_contains_human(void) {
    hu_runtime_t r = hu_runtime_native();
    const char *path = r.vtable->storage_path(r.ctx);
    HU_ASSERT_NOT_NULL(path);
    HU_ASSERT_TRUE(strstr(path, "human") != NULL);
}

static void test_runtime_docker_has_shell(void) {
    hu_runtime_t r = hu_runtime_docker(false, 0, "alpine:latest", ".");
    HU_ASSERT_TRUE(r.vtable->has_shell_access(r.ctx));
}

static void test_runtime_docker_no_long_running(void) {
    hu_runtime_t r = hu_runtime_docker(true, 128, "alpine:latest", ".");
    HU_ASSERT_FALSE(r.vtable->supports_long_running(r.ctx));
}

static void test_runtime_docker_fs_with_mount(void) {
    hu_runtime_t r = hu_runtime_docker(true, 64, "alpine:latest", ".");
    HU_ASSERT_TRUE(r.vtable->has_filesystem_access(r.ctx));
    const char *path = r.vtable->storage_path(r.ctx);
    HU_ASSERT_TRUE(strstr(path, "workspace") != NULL);
}

static void test_runtime_docker_fs_without_mount(void) {
    hu_runtime_t r = hu_runtime_docker(false, 64, "alpine:latest", ".");
    HU_ASSERT_FALSE(r.vtable->has_filesystem_access(r.ctx));
    const char *path = r.vtable->storage_path(r.ctx);
    HU_ASSERT_TRUE(strstr(path, "tmp") != NULL);
}

static void test_runtime_docker_memory_zero_when_unlimited(void) {
    hu_runtime_t r = hu_runtime_docker(false, 0, "alpine:latest", ".");
    uint64_t budget = r.vtable->memory_budget(r.ctx);
    HU_ASSERT_EQ(budget, 0u);
}

static void test_runtime_cloudflare_all_vtable_methods(void) {
    hu_runtime_t r = hu_runtime_cloudflare();
    HU_ASSERT_STR_EQ(r.vtable->name(r.ctx), "cloudflare");
    HU_ASSERT_FALSE(r.vtable->has_shell_access(r.ctx));
    HU_ASSERT_FALSE(r.vtable->has_filesystem_access(r.ctx));
    HU_ASSERT_STR_EQ(r.vtable->storage_path(r.ctx), "");
    HU_ASSERT_FALSE(r.vtable->supports_long_running(r.ctx));
    HU_ASSERT_EQ(r.vtable->memory_budget(r.ctx), 128u * 1024 * 1024);
}

static void test_runtime_wasm_storage_path(void) {
    hu_runtime_t r = hu_runtime_wasm(64);
    const char *path = r.vtable->storage_path(r.ctx);
    HU_ASSERT_NOT_NULL(path);
    HU_ASSERT_TRUE(strlen(path) > 0);
}

static void test_runtime_wasm_no_fs(void) {
    hu_runtime_t r = hu_runtime_wasm(64);
    HU_ASSERT_FALSE(r.vtable->has_filesystem_access(r.ctx));
}

static void test_runtime_docker_storage_workspace_path(void) {
    hu_runtime_t r = hu_runtime_docker(true, 64, "alpine:latest", ".");
    const char *path = r.vtable->storage_path(r.ctx);
    HU_ASSERT_STR_EQ(path, "/workspace/.human");
}

static void test_runtime_docker_storage_tmp_path(void) {
    hu_runtime_t r = hu_runtime_docker(false, 64, "alpine:latest", ".");
    const char *path = r.vtable->storage_path(r.ctx);
    HU_ASSERT_STR_EQ(path, "/tmp/.human");
}

static void test_runtime_vtable_dispatch_native(void) {
    hu_runtime_t r = hu_runtime_native();
    HU_ASSERT_TRUE(strlen(r.vtable->name(r.ctx)) > 0);
    (void)r.vtable->has_shell_access(r.ctx);
    (void)r.vtable->has_filesystem_access(r.ctx);
    (void)r.vtable->storage_path(r.ctx);
    (void)r.vtable->supports_long_running(r.ctx);
    (void)r.vtable->memory_budget(r.ctx);
}

static void test_runtime_vtable_dispatch_docker(void) {
    hu_runtime_t r = hu_runtime_docker(true, 256, "alpine:latest", ".");
    HU_ASSERT_TRUE(strlen(r.vtable->name(r.ctx)) > 0);
    (void)r.vtable->has_shell_access(r.ctx);
    (void)r.vtable->has_filesystem_access(r.ctx);
    (void)r.vtable->storage_path(r.ctx);
    (void)r.vtable->supports_long_running(r.ctx);
    (void)r.vtable->memory_budget(r.ctx);
}

static void test_runtime_kind_enum_values(void) {
    hu_runtime_kind_t k1 = HU_RUNTIME_NATIVE;
    hu_runtime_kind_t k2 = HU_RUNTIME_DOCKER;
    hu_runtime_kind_t k3 = HU_RUNTIME_WASM;
    hu_runtime_kind_t k4 = HU_RUNTIME_CLOUDFLARE;
    HU_ASSERT_EQ((int)k1, 0);
    HU_ASSERT_TRUE(k1 != k2);
    HU_ASSERT_TRUE(k2 != k3);
    HU_ASSERT_TRUE(k3 != k4);
}

static void test_runtime_from_config_native(void) {
    hu_config_t cfg = {0};
    cfg.runtime.kind = "native";
    hu_runtime_t r;
    hu_error_t err = hu_runtime_from_config(&cfg, &r);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(r.vtable);
    HU_ASSERT_STR_EQ(r.vtable->name(r.ctx), "native");
}

static void test_runtime_from_config_null_defaults_native(void) {
    hu_config_t cfg = {0};
    cfg.runtime.kind = NULL;
    hu_runtime_t r;
    hu_error_t err = hu_runtime_from_config(&cfg, &r);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(r.vtable->name(r.ctx), "native");
}

static void test_runtime_from_config_docker(void) {
    hu_config_t cfg = {0};
    cfg.runtime.kind = "docker";
    hu_runtime_t r;
    hu_error_t err = hu_runtime_from_config(&cfg, &r);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(r.vtable->name(r.ctx), "docker");
}

static void test_runtime_from_config_wasm(void) {
    hu_config_t cfg = {0};
    cfg.runtime.kind = "wasm";
    hu_runtime_t r;
    hu_error_t err = hu_runtime_from_config(&cfg, &r);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(r.vtable->name(r.ctx), "wasm");
}

static void test_runtime_from_config_cloudflare(void) {
    hu_config_t cfg = {0};
    cfg.runtime.kind = "cloudflare";
    hu_runtime_t r;
    hu_error_t err = hu_runtime_from_config(&cfg, &r);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(r.vtable->name(r.ctx), "cloudflare");
}

static void test_runtime_from_config_unknown_returns_error(void) {
    hu_config_t cfg = {0};
    cfg.runtime.kind = "unknown_runtime_xyz";
    hu_runtime_t r;
    hu_error_t err = hu_runtime_from_config(&cfg, &r);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
}

static void test_runtime_from_config_null_config_returns_error(void) {
    hu_runtime_t r;
    hu_error_t err = hu_runtime_from_config(NULL, &r);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_runtime_from_config_null_out_returns_error(void) {
    hu_config_t cfg = {0};
    cfg.runtime.kind = "native";
    hu_error_t err = hu_runtime_from_config(&cfg, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_docker_runtime_wrap_command(void) {
    hu_runtime_t r = hu_runtime_docker(true, 256, "alpine:3", "/home/ws");
    HU_ASSERT_NOT_NULL(r.vtable->wrap_command);

    const char *argv_in[] = {"echo", "hello"};
    const char *argv_out[32];
    size_t argc_out = 0;
    hu_error_t err = r.vtable->wrap_command(r.ctx, argv_in, 2, argv_out, 32, &argc_out);
    HU_ASSERT_EQ(err, HU_OK);
    /* docker run --rm -m 256m -v /home/ws:/workspace -w /workspace alpine:3 echo hello = 12 args */
    HU_ASSERT_EQ(argc_out, 12u);
    HU_ASSERT_STR_EQ(argv_out[0], "docker");
    HU_ASSERT_STR_EQ(argv_out[1], "run");
    HU_ASSERT_STR_EQ(argv_out[2], "--rm");
    HU_ASSERT_STR_EQ(argv_out[3], "-m");
    HU_ASSERT_STR_EQ(argv_out[5], "-v");
    HU_ASSERT_STR_EQ(argv_out[7], "-w");
    HU_ASSERT_STR_EQ(argv_out[8], "/workspace");
    HU_ASSERT_STR_EQ(argv_out[9], "alpine:3");
    HU_ASSERT_STR_EQ(argv_out[10], "echo");
    HU_ASSERT_STR_EQ(argv_out[11], "hello");
    HU_ASSERT_EQ(argv_out[12], (const char *)NULL);
}

static void test_docker_runtime_wrap_command_no_image_returns_not_supported(void) {
    hu_runtime_t r = hu_runtime_docker(true, 0, NULL, ".");
    HU_ASSERT_NOT_NULL(r.vtable->wrap_command);

    const char *argv_in[] = {"ls"};
    const char *argv_out[32];
    size_t argc_out = 0;
    hu_error_t err = r.vtable->wrap_command(r.ctx, argv_in, 1, argv_out, 32, &argc_out);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
}

static void test_docker_runtime_workspace_with_colon_returns_invalid_argument(void) {
    hu_runtime_t r = hu_runtime_docker(true, 64, "alpine:latest", "/path/with:colon");
    HU_ASSERT_NOT_NULL(r.vtable->wrap_command);

    const char *argv_in[] = {"echo", "hello"};
    const char *argv_out[32];
    size_t argc_out = 0;
    hu_error_t err = r.vtable->wrap_command(r.ctx, argv_in, 2, argv_out, 32, &argc_out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_docker_runtime_workspace_null_with_mount(void) {
    hu_runtime_t r = hu_runtime_docker(true, 64, "alpine:latest", NULL);
    HU_ASSERT_NOT_NULL(r.vtable->wrap_command);
    /* NULL workspace with mount_workspace: no -v flag, wrap_command should succeed */
    const char *argv_in[] = {"echo", "x"};
    const char *argv_out[32];
    size_t argc_out = 0;
    hu_error_t err = r.vtable->wrap_command(r.ctx, argv_in, 2, argv_out, 32, &argc_out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(argv_out[0], "docker");
}

static void test_docker_runtime_init_deinit_lifecycle(void) {
    hu_runtime_t r = hu_runtime_docker(false, 128, "alpine:3", ".");
    HU_ASSERT_NOT_NULL(r.ctx);
    HU_ASSERT_NOT_NULL(r.vtable);
    HU_ASSERT_STR_EQ(r.vtable->name(r.ctx), "docker");
    HU_ASSERT_EQ(r.vtable->memory_budget(r.ctx), 128u * 1024 * 1024);
    HU_ASSERT_FALSE(r.vtable->has_filesystem_access(r.ctx));
    /* No explicit deinit; vtable uses static ctx. Second create overwrites. */
    hu_runtime_t r2 = hu_runtime_docker(true, 0, "busybox:1", "/tmp");
    HU_ASSERT_STR_EQ(r2.vtable->name(r2.ctx), "docker");
    HU_ASSERT_TRUE(r2.vtable->has_filesystem_access(r2.ctx));
}

static void test_native_runtime_no_wrap(void) {
    hu_runtime_t r = hu_runtime_native();
    HU_ASSERT_EQ(r.vtable->wrap_command, (void *)NULL);
}

static void test_runtime_gce_create(void) {
    hu_runtime_t r = hu_runtime_gce("my-project", "us-central1-a", "vm-1", 1024);
    HU_ASSERT_NOT_NULL(r.ctx);
    HU_ASSERT_NOT_NULL(r.vtable);
    HU_ASSERT_STR_EQ(r.vtable->name(r.ctx), "gce");
}

static void test_runtime_gce_has_shell(void) {
    hu_runtime_t r = hu_runtime_gce("p", "z", "i", 0);
    HU_ASSERT_TRUE(r.vtable->has_shell_access(r.ctx));
}

static void test_runtime_gce_has_filesystem(void) {
    hu_runtime_t r = hu_runtime_gce("p", "z", "i", 0);
    HU_ASSERT_TRUE(r.vtable->has_filesystem_access(r.ctx));
}

static void test_runtime_gce_supports_long_running(void) {
    hu_runtime_t r = hu_runtime_gce("p", "z", "i", 0);
    HU_ASSERT_TRUE(r.vtable->supports_long_running(r.ctx));
}

static void test_runtime_gce_memory_budget(void) {
    hu_runtime_t r = hu_runtime_gce("p", "z", "i", 512);
    uint64_t budget = r.vtable->memory_budget(r.ctx);
    HU_ASSERT_EQ(budget, 512u * 1024 * 1024);
}

static void test_runtime_gce_memory_zero_when_unlimited(void) {
    hu_runtime_t r = hu_runtime_gce("p", "z", "i", 0);
    uint64_t budget = r.vtable->memory_budget(r.ctx);
    HU_ASSERT_EQ(budget, 0u);
}

static void test_runtime_gce_wrap_command(void) {
    hu_runtime_t r = hu_runtime_gce("my-project", "us-central1-a", "my-vm", 0);
    HU_ASSERT_NOT_NULL(r.vtable->wrap_command);
    const char *argv_in[] = {"echo", "hello"};
    const char *argv_out[32];
    size_t argc_out = 0;
    hu_error_t err = r.vtable->wrap_command(r.ctx, argv_in, 2, argv_out, 32, &argc_out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(argv_out[0], "gcloud");
    HU_ASSERT_STR_EQ(argv_out[1], "compute");
    HU_ASSERT_STR_EQ(argv_out[2], "ssh");
    HU_ASSERT_STR_EQ(argv_out[3], "my-vm");
    HU_ASSERT_TRUE(argc_out >= 6);
    HU_ASSERT_EQ(argv_out[argc_out], (const char *)NULL);
}

static void test_runtime_gce_wrap_no_instance_returns_error(void) {
    hu_runtime_t r = hu_runtime_gce("p", "z", NULL, 0);
    const char *argv_in[] = {"ls"};
    const char *argv_out[32];
    size_t argc_out = 0;
    hu_error_t err = r.vtable->wrap_command(r.ctx, argv_in, 1, argv_out, 32, &argc_out);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
}

static void test_runtime_from_config_gce(void) {
    hu_config_t cfg = {0};
    cfg.runtime.kind = "gce";
    cfg.runtime.gce_project = "proj";
    cfg.runtime.gce_zone = "zone";
    cfg.runtime.gce_instance = "vm";
    hu_runtime_t r;
    hu_error_t err = hu_runtime_from_config(&cfg, &r);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(r.vtable->name(r.ctx), "gce");
}

static void test_runtime_gce_storage_path(void) {
    hu_runtime_t r = hu_runtime_gce("p", "z", "i", 0);
    const char *path = r.vtable->storage_path(r.ctx);
    HU_ASSERT_NOT_NULL(path);
    HU_ASSERT_TRUE(strstr(path, "human") != NULL);
}

static void test_wasm_runtime_wrap_not_supported(void) {
    hu_runtime_t r = hu_runtime_wasm(64);
    HU_ASSERT_NOT_NULL(r.vtable->wrap_command);

    const char *argv_in[] = {"echo", "x"};
    const char *argv_out[32];
    size_t argc_out = 0;
    hu_error_t err = r.vtable->wrap_command(r.ctx, argv_in, 2, argv_out, 32, &argc_out);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
}

void run_runtime_tests(void) {
    HU_TEST_SUITE("Runtime");
    HU_RUN_TEST(test_runtime_native_create);
    HU_RUN_TEST(test_runtime_native_has_shell);
    HU_RUN_TEST(test_runtime_native_has_filesystem);
    HU_RUN_TEST(test_runtime_native_supports_long_running);
    HU_RUN_TEST(test_runtime_native_storage_path);
    HU_RUN_TEST(test_runtime_docker_create);
    HU_RUN_TEST(test_runtime_docker_memory_budget);
    HU_RUN_TEST(test_runtime_docker_storage_path);
    HU_RUN_TEST(test_runtime_docker_mount_workspace);
    HU_RUN_TEST(test_runtime_wasm_create);
    HU_RUN_TEST(test_runtime_wasm_no_shell);
    HU_RUN_TEST(test_runtime_wasm_memory_budget);
    HU_RUN_TEST(test_runtime_wasm_no_long_running);
    HU_RUN_TEST(test_runtime_cloudflare_create);
    HU_RUN_TEST(test_runtime_native_memory_budget);
    HU_RUN_TEST(test_runtime_native_storage_contains_human);
    HU_RUN_TEST(test_runtime_docker_has_shell);
    HU_RUN_TEST(test_runtime_docker_no_long_running);
    HU_RUN_TEST(test_runtime_docker_fs_with_mount);
    HU_RUN_TEST(test_runtime_docker_fs_without_mount);
    HU_RUN_TEST(test_runtime_docker_memory_zero_when_unlimited);
    HU_RUN_TEST(test_runtime_docker_storage_workspace_path);
    HU_RUN_TEST(test_runtime_docker_storage_tmp_path);
    HU_RUN_TEST(test_runtime_cloudflare_all_vtable_methods);
    HU_RUN_TEST(test_runtime_wasm_storage_path);
    HU_RUN_TEST(test_runtime_wasm_no_fs);
    HU_RUN_TEST(test_runtime_vtable_dispatch_native);
    HU_RUN_TEST(test_runtime_vtable_dispatch_docker);
    HU_RUN_TEST(test_runtime_kind_enum_values);
    HU_RUN_TEST(test_runtime_from_config_native);
    HU_RUN_TEST(test_runtime_from_config_null_defaults_native);
    HU_RUN_TEST(test_runtime_from_config_docker);
    HU_RUN_TEST(test_runtime_from_config_wasm);
    HU_RUN_TEST(test_runtime_from_config_cloudflare);
    HU_RUN_TEST(test_runtime_from_config_unknown_returns_error);
    HU_RUN_TEST(test_runtime_from_config_null_config_returns_error);
    HU_RUN_TEST(test_runtime_from_config_null_out_returns_error);
    HU_RUN_TEST(test_docker_runtime_wrap_command);
    HU_RUN_TEST(test_docker_runtime_wrap_command_no_image_returns_not_supported);
    HU_RUN_TEST(test_docker_runtime_workspace_with_colon_returns_invalid_argument);
    HU_RUN_TEST(test_docker_runtime_workspace_null_with_mount);
    HU_RUN_TEST(test_docker_runtime_init_deinit_lifecycle);
    HU_RUN_TEST(test_native_runtime_no_wrap);
    HU_RUN_TEST(test_wasm_runtime_wrap_not_supported);
    HU_RUN_TEST(test_runtime_gce_create);
    HU_RUN_TEST(test_runtime_gce_has_shell);
    HU_RUN_TEST(test_runtime_gce_has_filesystem);
    HU_RUN_TEST(test_runtime_gce_supports_long_running);
    HU_RUN_TEST(test_runtime_gce_memory_budget);
    HU_RUN_TEST(test_runtime_gce_memory_zero_when_unlimited);
    HU_RUN_TEST(test_runtime_gce_wrap_command);
    HU_RUN_TEST(test_runtime_gce_wrap_no_instance_returns_error);
    HU_RUN_TEST(test_runtime_from_config_gce);
    HU_RUN_TEST(test_runtime_gce_storage_path);
    HU_RUN_TEST(test_platform_is_windows_or_unix);
    HU_RUN_TEST(test_platform_get_shell);
    HU_RUN_TEST(test_platform_get_shell_flag);
    HU_RUN_TEST(test_platform_get_temp_dir);
    HU_RUN_TEST(test_platform_get_home_dir);
}
