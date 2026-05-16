/*
 * Regression tests for the shell tool's deny-by-default sandbox contract.
 *
 * Audit finding (May 2026): the shell tool's post-fork code path could fall
 * through to bare /bin/sh when a sandbox was configured on the policy but
 * (a) hu_sandbox_is_available returned false, or (b) hu_sandbox_wrap_command
 * failed. The operator's sandbox intent was silently discarded.
 *
 * These tests pin the contract of the pure predicate that the fork path now
 * consults before launching the child program. They do NOT actually fork —
 * that would be flaky and platform-dependent. The fork path itself is
 * reviewed for the call site to the predicate; the predicate truth table is
 * locked here.
 */

#include "human/security.h"
#include "human/security/sandbox.h"
#include "human/tools/shell.h"
#include "test_framework.h"
#include <stdbool.h>
#include <string.h>

/* ── No-op sandbox vtable used to stand up a non-NULL policy->sandbox.
 * Only the *presence* of a sandbox pointer matters to the predicate; the
 * vtable methods are not invoked from the predicate itself. */
static hu_error_t noop_apply(void *ctx) {
    (void)ctx;
    return HU_OK;
}
static bool noop_is_available(void *ctx) {
    (void)ctx;
    return true;
}
static hu_error_t noop_wrap(void *ctx, const char *const *argv, size_t argc, const char **out_argv,
                            size_t out_cap, size_t *out_count) {
    (void)ctx;
    (void)argv;
    (void)argc;
    (void)out_argv;
    (void)out_cap;
    if (out_count)
        *out_count = 0;
    return HU_OK;
}

static hu_sandbox_vtable_t k_noop_vtable = {
    .apply = noop_apply,
    .is_available = noop_is_available,
    .wrap_command = noop_wrap,
};

static hu_sandbox_t k_noop_sandbox = {
    .ctx = NULL,
    .vtable = &k_noop_vtable,
};

/* ── Tests ────────────────────────────────────────────────────────────── */

static void test_shell_sandbox_null_policy_allows_bare_launch(void) {
    HU_ASSERT_FALSE(hu_shell_must_deny_unsandboxed(NULL, false, false));
}

static void test_shell_sandbox_no_sandbox_configured_allows_bare_launch(void) {
    hu_security_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    policy.sandbox = NULL;
    HU_ASSERT_FALSE(hu_shell_must_deny_unsandboxed(&policy, false, false));
}

static void test_shell_sandbox_apply_applied_allows_launch(void) {
    hu_security_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    policy.sandbox = &k_noop_sandbox;
    HU_ASSERT_FALSE(hu_shell_must_deny_unsandboxed(&policy, /*apply*/ true,
                                                   /*wrap_succeeded*/ false));
}

static void test_shell_sandbox_wrap_succeeded_allows_launch(void) {
    hu_security_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    policy.sandbox = &k_noop_sandbox;
    HU_ASSERT_FALSE(hu_shell_must_deny_unsandboxed(&policy, /*apply*/ false,
                                                   /*wrap_succeeded*/ true));
}

/* Regression for cursor-bot review on PR #90: a process with active kernel
   sandbox AND a failed wrap call must still be allowed to launch. The
   earlier predicate short-circuited on wrap_attempted && !wrap_succeeded,
   denying a process that was already contained by Landlock/seccomp. */
static void test_shell_sandbox_apply_applied_and_wrap_failed_allows_launch(void) {
    hu_security_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    policy.sandbox = &k_noop_sandbox;
    HU_ASSERT_FALSE(hu_shell_must_deny_unsandboxed(&policy, /*apply*/ true,
                                                   /*wrap_succeeded*/ false));
}

/* Regression: sandbox configured but neither apply nor wrap actually
   protected the process (e.g. backend reports NOT_SUPPORTED and
   is_available returns false). Deny rather than launch uncontained. */
static void test_shell_sandbox_configured_but_nothing_applied_denies(void) {
    hu_security_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    policy.sandbox = &k_noop_sandbox;
    HU_ASSERT_TRUE(hu_shell_must_deny_unsandboxed(&policy, /*apply*/ false,
                                                  /*wrap_succeeded*/ false));
}

/* Defensive: redundant containment (apply + wrap both succeeded) is allowed. */
static void test_shell_sandbox_apply_and_wrap_both_allow_launch(void) {
    hu_security_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    policy.sandbox = &k_noop_sandbox;
    HU_ASSERT_FALSE(hu_shell_must_deny_unsandboxed(&policy, /*apply*/ true,
                                                   /*wrap_succeeded*/ true));
}

/* ── Suite registration ───────────────────────────────────────────────── */

void run_shell_sandbox_tests(void) {
    HU_TEST_SUITE("shell sandbox deny-by-default");
    HU_RUN_TEST(test_shell_sandbox_null_policy_allows_bare_launch);
    HU_RUN_TEST(test_shell_sandbox_no_sandbox_configured_allows_bare_launch);
    HU_RUN_TEST(test_shell_sandbox_apply_applied_allows_launch);
    HU_RUN_TEST(test_shell_sandbox_wrap_succeeded_allows_launch);
    HU_RUN_TEST(test_shell_sandbox_apply_applied_and_wrap_failed_allows_launch);
    HU_RUN_TEST(test_shell_sandbox_configured_but_nothing_applied_denies);
    HU_RUN_TEST(test_shell_sandbox_apply_and_wrap_both_allow_launch);
}
