/*
 * Regression tests for the shell tool's child-environment sanitization.
 *
 * Dead-code finding (2026-09-20 dead-code-plan, task 2): `src/doctor/doctor.c`
 * reported "Exec env sanitization: active (blocks MAVEN_OPTS, LD_PRELOAD,
 * GLIBC_TUNABLES)", but `hu_exec_env_sanitize` (src/security/exec_env.c) had
 * no caller anywhere in the codebase — the shell tool built the child's
 * environment with bare setenv() calls on top of whatever it inherited from
 * fork(), so a blocklisted variable in the daemon's own environment would
 * reach every shell command the agent ran.
 *
 * These tests pin the contract of the pure helper that the fork children in
 * shell.c now consult before exec. They do NOT actually fork or exec — the
 * shell tool's real fork/exec path is stubbed out under HU_IS_TEST (see
 * shell_execute's `#if HU_IS_TEST` stub), the same reasoning that keeps
 * test_shell_sandbox.c testing the pure `hu_shell_must_deny_unsandboxed`
 * predicate instead of forking.
 */

#include "../src/tools/shell_internal.h"
#include "test_framework.h"
#include <stdbool.h>
#include <string.h>

/* ── Pre: confirm the fixture actually contains the dangerous names ─────── */

static void test_shell_env_fixture_contains_blocked_names(void) {
    char *env[] = {
        "PATH=/usr/bin",
        "LD_PRELOAD=/tmp/x.so",
        "MAVEN_OPTS=-Dx",
        "HOME=/home/user",
    };
    bool saw_ld_preload = false;
    bool saw_maven_opts = false;
    for (size_t i = 0; i < 4; i++) {
        if (strncmp(env[i], "LD_PRELOAD=", 11) == 0)
            saw_ld_preload = true;
        if (strncmp(env[i], "MAVEN_OPTS=", 11) == 0)
            saw_maven_opts = true;
    }
    HU_ASSERT_TRUE(saw_ld_preload);
    HU_ASSERT_TRUE(saw_maven_opts);
}

/* ── Post: the built child env drops the blocklisted names ──────────────── */

static void test_shell_env_drops_ld_preload_and_maven_opts(void) {
    char *env[] = {
        "PATH=/usr/bin",
        "LD_PRELOAD=/tmp/x.so",
        "MAVEN_OPTS=-Dx",
        "HOME=/home/user",
    };
    char *out[8];
    size_t n = hu_shell_build_child_env(env, 4, out, 8);

    HU_ASSERT_EQ(n, 2u);
    for (size_t i = 0; i < n; i++) {
        HU_ASSERT_TRUE(strncmp(out[i], "LD_PRELOAD=", 11) != 0);
        HU_ASSERT_TRUE(strncmp(out[i], "MAVEN_OPTS=", 11) != 0);
    }

    bool saw_path = false;
    bool saw_home = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(out[i], "PATH=/usr/bin") == 0)
            saw_path = true;
        if (strcmp(out[i], "HOME=/home/user") == 0)
            saw_home = true;
    }
    HU_ASSERT_TRUE(saw_path);
    HU_ASSERT_TRUE(saw_home);
}

static void test_shell_env_glibc_tunables_dropped(void) {
    char *env[] = {
        "GLIBC_TUNABLES=glibc.malloc.check=0",
        "PATH=/usr/bin",
    };
    char *out[4];
    size_t n = hu_shell_build_child_env(env, 2, out, 4);

    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_STR_EQ(out[0], "PATH=/usr/bin");
}

static void test_shell_env_no_blocked_vars_is_noop(void) {
    char *env[] = {"PATH=/usr/bin", "HOME=/home/user", "SHELL=/bin/bash"};
    char *out[8];
    size_t n = hu_shell_build_child_env(env, 3, out, 8);

    HU_ASSERT_EQ(n, 3u);
    HU_ASSERT_STR_EQ(out[0], "PATH=/usr/bin");
    HU_ASSERT_STR_EQ(out[1], "HOME=/home/user");
    HU_ASSERT_STR_EQ(out[2], "SHELL=/bin/bash");
}

static void test_shell_env_null_and_empty_inputs(void) {
    char *out[4];
    HU_ASSERT_EQ(hu_shell_build_child_env(NULL, 0, out, 4), 0u);

    char *env[] = {"PATH=/usr/bin"};
    HU_ASSERT_EQ(hu_shell_build_child_env(env, 1, NULL, 4), 0u);
    HU_ASSERT_EQ(hu_shell_build_child_env(env, 1, out, 0), 0u);
    HU_ASSERT_EQ(hu_shell_build_child_env(env, 0, out, 4), 0u);
}

static void test_shell_env_truncates_to_out_cap(void) {
    char *env[] = {"LD_PRELOAD=/tmp/x.so", "PATH=/usr/bin", "HOME=/home/user"};
    char *out[1];
    /* out_cap=1 means only the first input entry is even considered; it is
     * blocked, so nothing survives. */
    size_t n = hu_shell_build_child_env(env, 3, out, 1);
    HU_ASSERT_EQ(n, 0u);
}

void run_shell_env_tests(void) {
    HU_TEST_SUITE("shell_env");
    HU_RUN_TEST(test_shell_env_fixture_contains_blocked_names);
    HU_RUN_TEST(test_shell_env_drops_ld_preload_and_maven_opts);
    HU_RUN_TEST(test_shell_env_glibc_tunables_dropped);
    HU_RUN_TEST(test_shell_env_no_blocked_vars_is_noop);
    HU_RUN_TEST(test_shell_env_null_and_empty_inputs);
    HU_RUN_TEST(test_shell_env_truncates_to_out_cap);
}
