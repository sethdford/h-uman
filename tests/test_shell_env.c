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
#include <stdio.h>
#include <stdlib.h>
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

/* ── The HU_SHELL_MAX_ENV_VARS cap: truncation must be SIGNALLED ────────── */

/*
 * Final-review finding (2026-09-21): entries past the 512th were silently
 * never examined by the sanitizer — a blocklisted LD_PRELOAD sitting at index
 * 600 of a long environment reached the shell child untouched, with nothing
 * anywhere saying so. The cap itself is deliberate (truncating the child's
 * environment would be worse than leaving the tail unsanitized), so the fix is
 * to make the condition observable: hu_shell_count_env reports it through its
 * `truncated` out-parameter, shell.c's parent-side hu_shell_warn_env_cap_once
 * turns that into one WARN per process.
 *
 * These assert on the returned count and the flag, never on log text.
 */

/* Backing store for the over-cap fixtures: HU_SHELL_MAX_ENV_VARS + 1 entries
 * plus the NULL terminator. Static because 513 * 32 bytes is more than belongs
 * on a test stack. */
#define HU_TEST_ENV_OVER (HU_SHELL_MAX_ENV_VARS + 1)
static char s_env_storage[HU_TEST_ENV_OVER][32];
static char *s_env[HU_TEST_ENV_OVER + 1];

/* Fill `count` entries named VAR_0..VAR_n, NULL-terminated. If
 * blocked_at >= 0, that index instead carries a blocklisted LD_PRELOAD. */
static void build_env_fixture(size_t count, long blocked_at) {
    for (size_t i = 0; i < count; i++) {
        if (blocked_at >= 0 && i == (size_t)blocked_at)
            snprintf(s_env_storage[i], sizeof(s_env_storage[i]), "LD_PRELOAD=/tmp/x.so");
        else
            snprintf(s_env_storage[i], sizeof(s_env_storage[i]), "VAR_%zu=v", i);
        s_env[i] = s_env_storage[i];
    }
    s_env[count] = NULL;
}

static void test_shell_env_count_signals_truncation_past_cap(void) {
    build_env_fixture(HU_TEST_ENV_OVER, -1); /* 513 entries */
    bool truncated = false;
    size_t n = hu_shell_count_env(s_env, HU_SHELL_MAX_ENV_VARS, &truncated);

    /* Counts exactly the cap, and SAYS it stopped early. */
    HU_ASSERT_EQ(n, (size_t)HU_SHELL_MAX_ENV_VARS);
    HU_ASSERT_TRUE(truncated);
}

static void test_shell_env_count_exactly_at_cap_is_not_truncated(void) {
    build_env_fixture(HU_SHELL_MAX_ENV_VARS, -1); /* 512 entries */
    bool truncated = true;                        /* start true so a no-op would fail this */
    size_t n = hu_shell_count_env(s_env, HU_SHELL_MAX_ENV_VARS, &truncated);

    HU_ASSERT_EQ(n, (size_t)HU_SHELL_MAX_ENV_VARS);
    HU_ASSERT_TRUE(!truncated);
}

static void test_shell_env_count_short_env_is_not_truncated(void) {
    build_env_fixture(3, -1);
    bool truncated = true;
    HU_ASSERT_EQ(hu_shell_count_env(s_env, HU_SHELL_MAX_ENV_VARS, &truncated), 3u);
    HU_ASSERT_TRUE(!truncated);
    HU_ASSERT_EQ(hu_shell_count_env(NULL, HU_SHELL_MAX_ENV_VARS, &truncated), 0u);
    HU_ASSERT_TRUE(!truncated);
}

/* ── The sanitizer seam, exercised without forking ──────────────────────── */

static void test_shell_env_collect_blocked_finds_blocklisted(void) {
    char *env[] = {
        "PATH=/usr/bin", "LD_PRELOAD=/tmp/x.so", "MAVEN_OPTS=-Dx", "HOME=/home/user", NULL,
    };
    char *blocked[8];
    bool truncated = true;
    size_t n = hu_shell_collect_blocked_env(env, 8, blocked, 8, &truncated);

    HU_ASSERT_EQ(n, 2u);
    HU_ASSERT_STR_EQ(blocked[0], "LD_PRELOAD=/tmp/x.so");
    HU_ASSERT_STR_EQ(blocked[1], "MAVEN_OPTS=-Dx");
    HU_ASSERT_TRUE(!truncated);
}

static void test_shell_env_collect_blocked_clean_env_is_empty(void) {
    char *env[] = {"PATH=/usr/bin", "HOME=/home/user", NULL};
    char *blocked[8];
    HU_ASSERT_EQ(hu_shell_collect_blocked_env(env, 8, blocked, 8, NULL), 0u);
}

/*
 * The whole point of the cap: a blocklisted entry BEYOND it is not collected,
 * so it survives into the child. Pins the documented behavior (leave the tail
 * alone rather than truncate the environment) AND that the caller is told.
 */
static void test_shell_env_collect_blocked_misses_entry_past_cap(void) {
    build_env_fixture(HU_TEST_ENV_OVER, HU_SHELL_MAX_ENV_VARS); /* LD_PRELOAD at index 512 */
    char *blocked[HU_SHELL_MAX_ENV_VARS];
    bool truncated = false;
    size_t n = hu_shell_collect_blocked_env(s_env, HU_TEST_ENV_OVER, blocked, HU_SHELL_MAX_ENV_VARS,
                                            &truncated);

    /* Nothing collected — the only blocklisted entry is past the cap — and the
     * truncation flag is what tells the caller to warn about exactly that. */
    HU_ASSERT_EQ(n, 0u);
    HU_ASSERT_TRUE(truncated);
}

/* Same fixture, blocklisted entry moved just INSIDE the cap: now it is caught.
 * Paired with the test above so neither can pass by doing nothing. */
static void test_shell_env_collect_blocked_catches_entry_at_cap_edge(void) {
    build_env_fixture(HU_TEST_ENV_OVER, HU_SHELL_MAX_ENV_VARS - 1); /* index 511 */
    char *blocked[HU_SHELL_MAX_ENV_VARS];
    bool truncated = false;
    size_t n = hu_shell_collect_blocked_env(s_env, HU_TEST_ENV_OVER, blocked, HU_SHELL_MAX_ENV_VARS,
                                            &truncated);

    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_STR_EQ(blocked[0], "LD_PRELOAD=/tmp/x.so");
    HU_ASSERT_TRUE(truncated);
}

static void test_shell_env_collect_blocked_null_and_empty_inputs(void) {
    char *env[] = {"LD_PRELOAD=/tmp/x.so", NULL};
    char *blocked[4];
    HU_ASSERT_EQ(hu_shell_collect_blocked_env(NULL, 4, blocked, 4, NULL), 0u);
    HU_ASSERT_EQ(hu_shell_collect_blocked_env(env, 4, NULL, 4, NULL), 0u);
    HU_ASSERT_EQ(hu_shell_collect_blocked_env(env, 4, blocked, 0, NULL), 0u);
    HU_ASSERT_EQ(hu_shell_collect_blocked_env(env, 0, blocked, 4, NULL), 0u);
}

void run_shell_env_tests(void) {
    HU_TEST_SUITE("shell_env");
    HU_RUN_TEST(test_shell_env_fixture_contains_blocked_names);
    HU_RUN_TEST(test_shell_env_drops_ld_preload_and_maven_opts);
    HU_RUN_TEST(test_shell_env_glibc_tunables_dropped);
    HU_RUN_TEST(test_shell_env_no_blocked_vars_is_noop);
    HU_RUN_TEST(test_shell_env_null_and_empty_inputs);
    HU_RUN_TEST(test_shell_env_truncates_to_out_cap);
    HU_RUN_TEST(test_shell_env_count_signals_truncation_past_cap);
    HU_RUN_TEST(test_shell_env_count_exactly_at_cap_is_not_truncated);
    HU_RUN_TEST(test_shell_env_count_short_env_is_not_truncated);
    HU_RUN_TEST(test_shell_env_collect_blocked_finds_blocklisted);
    HU_RUN_TEST(test_shell_env_collect_blocked_clean_env_is_empty);
    HU_RUN_TEST(test_shell_env_collect_blocked_misses_entry_past_cap);
    HU_RUN_TEST(test_shell_env_collect_blocked_catches_entry_at_cap_edge);
    HU_RUN_TEST(test_shell_env_collect_blocked_null_and_empty_inputs);
}
