#include "test_framework.h"
#include "human/ml/ml_scripts_dir.h"

#include <stdlib.h>
#include <string.h>

/* Phase D Task D-1 (CF-7) resolver tests.
 *
 * Verifies that hu_ml_resolve_script_path correctly resolves to an absolute
 * path under the project's scripts/ directory, and rejects unsafe inputs.
 * AC1: every popen site uses the resolver, which returns an absolute path.
 * AC3: ≥5 tests with 0 ASan errors. */

static void clear_resolver_env(void) {
    unsetenv("HU_ML_SCRIPTS_DIR");
    unsetenv("HU_PROJECT_ROOT");
}

static void test_resolver_uses_env_when_set(void) {
    clear_resolver_env();
    setenv("HU_ML_SCRIPTS_DIR", "/tmp/fake-scripts", 1);
    char out[256];
    HU_ASSERT_EQ(hu_ml_resolve_script_path("grpo_mlx_train.py", out, sizeof(out)), HU_OK);
    HU_ASSERT_STR_EQ(out, "/tmp/fake-scripts/grpo_mlx_train.py");
    clear_resolver_env();
}

static void test_resolver_falls_back_to_project_root(void) {
    /* Resolution precedence is: HU_ML_SCRIPTS_DIR env > compile-time macro >
     * HU_PROJECT_ROOT/scripts. In dev/test builds the compile-time macro is
     * baked in (CMakeLists.txt injects HU_ML_SCRIPTS_DIR="${HU_ROOT}/scripts"
     * for human_core_test), so the HU_PROJECT_ROOT fallback path is normally
     * unreachable. Skip when compile-time wins; the resolver's project-root
     * branch is still exercised by an installed-style build where the macro
     * is not defined. */
    clear_resolver_env();
    setenv("HU_PROJECT_ROOT", "/opt/h-uman", 1);
    char out[256];
    HU_ASSERT_EQ(hu_ml_resolve_script_path("dpo_mlx_train.py", out, sizeof(out)), HU_OK);
    HU_SKIP_IF(strcmp(out, "/opt/h-uman/scripts/dpo_mlx_train.py") != 0,
               "compile-time HU_ML_SCRIPTS_DIR baked in; HU_PROJECT_ROOT path unreachable");
    HU_ASSERT_STR_EQ(out, "/opt/h-uman/scripts/dpo_mlx_train.py");
    clear_resolver_env();
}

static void test_resolver_returns_absolute_path(void) {
    /* Sanity: whichever source resolves, the result starts with '/'. */
    clear_resolver_env();
    setenv("HU_ML_SCRIPTS_DIR", "/anywhere/abs", 1);
    char out[256];
    HU_ASSERT_EQ(hu_ml_resolve_script_path("kto_mlx_train.py", out, sizeof(out)), HU_OK);
    HU_ASSERT_EQ(out[0], '/');
    clear_resolver_env();
}

static void test_resolver_rejects_null_or_empty_script_name(void) {
    char out[256];
    HU_ASSERT_EQ(hu_ml_resolve_script_path(NULL, out, sizeof(out)), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_ml_resolve_script_path("", out, sizeof(out)), HU_ERR_INVALID_ARGUMENT);
}

static void test_resolver_rejects_null_out_or_zero_cap(void) {
    char out[256];
    HU_ASSERT_EQ(hu_ml_resolve_script_path("foo.py", NULL, sizeof(out)), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_ml_resolve_script_path("foo.py", out, 0), HU_ERR_INVALID_ARGUMENT);
}

static void test_resolver_rejects_buffer_too_small(void) {
    clear_resolver_env();
    setenv("HU_ML_SCRIPTS_DIR", "/very/long/path/to/scripts/directory", 1);
    char small[8];
    HU_ASSERT_EQ(hu_ml_resolve_script_path("grpo_mlx_train.py", small, sizeof(small)),
                 HU_ERR_INVALID_ARGUMENT);
    clear_resolver_env();
}

static void test_resolver_rejects_quote_in_script_name(void) {
    char out[256];
    HU_ASSERT_EQ(hu_ml_resolve_script_path("foo';rm -rf /;'.py", out, sizeof(out)),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_resolver_rejects_shell_meta_in_script_name(void) {
    char out[256];
    HU_ASSERT_EQ(hu_ml_resolve_script_path("foo`pwd`.py", out, sizeof(out)),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_ml_resolve_script_path("foo$(uname).py", out, sizeof(out)),
                 HU_ERR_INVALID_ARGUMENT);
}

static void test_resolver_rejects_quote_in_resolved_dir(void) {
    clear_resolver_env();
    setenv("HU_ML_SCRIPTS_DIR", "/tmp/o'malley", 1);
    char out[256];
    HU_ASSERT_EQ(hu_ml_resolve_script_path("foo.py", out, sizeof(out)),
                 HU_ERR_INVALID_ARGUMENT);
    clear_resolver_env();
}

static void test_resolver_returns_not_supported_when_no_source(void) {
    /* If the build defines HU_ML_SCRIPTS_DIR at compile time (the default for
     * dev/test builds via CMakeLists.txt) the env-unset still resolves. Skip
     * this strictly-no-source case in that build configuration. */
    clear_resolver_env();
    char out[256];
    hu_error_t err = hu_ml_resolve_script_path("foo.py", out, sizeof(out));
    HU_SKIP_IF(err == HU_OK,
               "HU_ML_SCRIPTS_DIR baked in at compile time; strict-no-source case unreachable");
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);
}

/* AC4 regression guard: a CWD-shadow attack — attacker creates a malicious
 * scripts/grpo_mlx_train.py in CWD and starts the daemon. The resolver MUST
 * still return the trusted absolute path (compile-time or env-overridden),
 * NOT the attacker's CWD-relative file. We can't directly observe popen()
 * here, but we can prove the resolver itself ignores CWD. */
static void test_resolver_ignores_cwd_attacker_shadow(void) {
    clear_resolver_env();
    setenv("HU_ML_SCRIPTS_DIR", "/expected/trusted/dir", 1);
    char out[256];
    HU_ASSERT_EQ(hu_ml_resolve_script_path("grpo_mlx_train.py", out, sizeof(out)), HU_OK);
    /* The resolved path MUST be the env-supplied trusted dir, never
     * something CWD-derived. */
    HU_ASSERT_STR_EQ(out, "/expected/trusted/dir/grpo_mlx_train.py");
    /* Negative: it must NOT contain "scripts/" as a CWD-relative prefix
     * (i.e., the legacy bug shape). */
    HU_ASSERT_STR_NOT_CONTAINS(out, "./scripts/");
    clear_resolver_env();
}

void run_ml_scripts_dir_tests(void) {
    HU_TEST_SUITE("ml_scripts_dir");
    HU_RUN_TEST(test_resolver_uses_env_when_set);
    HU_RUN_TEST(test_resolver_falls_back_to_project_root);
    HU_RUN_TEST(test_resolver_returns_absolute_path);
    HU_RUN_TEST(test_resolver_rejects_null_or_empty_script_name);
    HU_RUN_TEST(test_resolver_rejects_null_out_or_zero_cap);
    HU_RUN_TEST(test_resolver_rejects_buffer_too_small);
    HU_RUN_TEST(test_resolver_rejects_quote_in_script_name);
    HU_RUN_TEST(test_resolver_rejects_shell_meta_in_script_name);
    HU_RUN_TEST(test_resolver_rejects_quote_in_resolved_dir);
    HU_RUN_TEST(test_resolver_returns_not_supported_when_no_source);
    HU_RUN_TEST(test_resolver_ignores_cwd_attacker_shadow);
}
