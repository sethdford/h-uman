/* Pins the proactive reachability pre-filter (2026-09-20, roadmap O3).
 *
 * Why: attributing pre-send drops showed 91% of proactive fires targeted one
 * contact the iMessage blue_guard then HELD. Each fire cost a proposer LLM
 * call, counted against FIR, and could never be delivered. The pre-filter asks
 * blue_guard's own predicate BEFORE the proposer runs, gated
 * HU_PROACTIVE_REACHABILITY = off | shadow | live.
 *
 * Assertions are NON-VACUOUS (tests-that-pin-bugs.md): every test pins a value
 * the code could get wrong — a gate that skipped in SHADOW, or never skipped in
 * LIVE, or skipped a non-iMessage contact, or failed OPEN on junk env — fails
 * here. Under HU_IS_TEST the chat.db inference is compiled out and the
 * predicate is a fail-closed stub (HOLD unless HU_IMESSAGE_ALLOW_GREEN), which
 * is exactly the seam these tests use to drive both verdicts. */
#include "test_framework.h"

#include "human/daemon_proactive.h"
#include <stdlib.h>
#include <string.h>

/* Save/restore so a failing test cannot leak env into its neighbours. */
typedef struct {
    char *mode;
    char *green;
} reach_env_t;

static char *dup_or_null(const char *s) {
    return s ? strdup(s) : NULL;
}

static void env_save(reach_env_t *e) {
    e->mode = dup_or_null(getenv("HU_PROACTIVE_REACHABILITY"));
    e->green = dup_or_null(getenv("HU_IMESSAGE_ALLOW_GREEN"));
    unsetenv("HU_PROACTIVE_REACHABILITY");
    unsetenv("HU_IMESSAGE_ALLOW_GREEN");
}

static void env_restore_one(const char *name, char *val) {
    if (val) {
        setenv(name, val, 1);
        free(val);
    } else {
        unsetenv(name);
    }
}

static void env_restore(reach_env_t *e) {
    env_restore_one("HU_PROACTIVE_REACHABILITY", e->mode);
    env_restore_one("HU_IMESSAGE_ALLOW_GREEN", e->green);
}

/* ── mode parsing: fail closed ───────────────────────────────────────── */

static void test_reach_mode_from_env_fails_closed(void) {
    reach_env_t e;
    env_save(&e);
    HU_ASSERT_EQ((int)hu_daemon_proactive_reach_mode_from_env(), (int)HU_PROACTIVE_REACH_OFF);
    setenv("HU_PROACTIVE_REACHABILITY", "shadow", 1);
    HU_ASSERT_EQ((int)hu_daemon_proactive_reach_mode_from_env(), (int)HU_PROACTIVE_REACH_SHADOW);
    setenv("HU_PROACTIVE_REACHABILITY", "live", 1);
    HU_ASSERT_EQ((int)hu_daemon_proactive_reach_mode_from_env(), (int)HU_PROACTIVE_REACH_LIVE);
    setenv("HU_PROACTIVE_REACHABILITY", "off", 1);
    HU_ASSERT_EQ((int)hu_daemon_proactive_reach_mode_from_env(), (int)HU_PROACTIVE_REACH_OFF);
    /* Junk must not fail OPEN into LIVE ("Live", "on" are junk, not modes). */
    setenv("HU_PROACTIVE_REACHABILITY", "Live", 1);
    HU_ASSERT_EQ((int)hu_daemon_proactive_reach_mode_from_env(), (int)HU_PROACTIVE_REACH_OFF);
    setenv("HU_PROACTIVE_REACHABILITY", "on", 1);
    HU_ASSERT_EQ((int)hu_daemon_proactive_reach_mode_from_env(), (int)HU_PROACTIVE_REACH_OFF);
    env_restore(&e);
}

/* ── pure decision: the full mode × reachable truth table ───────────── */

static void test_reach_decide_truth_table(void) {
    /* OFF: never acts, whatever the evidence says. */
    HU_ASSERT_EQ((int)hu_daemon_proactive_reach_decide(HU_PROACTIVE_REACH_OFF, true),
                 (int)HU_PROACTIVE_REACH_PASS);
    HU_ASSERT_EQ((int)hu_daemon_proactive_reach_decide(HU_PROACTIVE_REACH_OFF, false),
                 (int)HU_PROACTIVE_REACH_PASS);
    /* SHADOW: flags, never skips. */
    HU_ASSERT_EQ((int)hu_daemon_proactive_reach_decide(HU_PROACTIVE_REACH_SHADOW, true),
                 (int)HU_PROACTIVE_REACH_PASS);
    HU_ASSERT_EQ((int)hu_daemon_proactive_reach_decide(HU_PROACTIVE_REACH_SHADOW, false),
                 (int)HU_PROACTIVE_REACH_WOULD_SKIP);
    /* LIVE: skips only the unreachable. */
    HU_ASSERT_EQ((int)hu_daemon_proactive_reach_decide(HU_PROACTIVE_REACH_LIVE, true),
                 (int)HU_PROACTIVE_REACH_PASS);
    HU_ASSERT_EQ((int)hu_daemon_proactive_reach_decide(HU_PROACTIVE_REACH_LIVE, false),
                 (int)HU_PROACTIVE_REACH_SKIP);
}

/* ── should_skip: the wired path, driven through the fail-closed stub ── */

static const char *k_handle = "+15550001111";

static bool should_skip(const char *ch) {
    hu_allocator_t alloc = hu_system_allocator();
    return hu_daemon_proactive_reach_should_skip(NULL, &alloc, ch, "contact-a", k_handle,
                                                 strlen(k_handle));
}

static void test_should_skip_off_never_skips(void) {
    reach_env_t e;
    env_save(&e);
    /* Stub verdict is HOLD here, so a gate that ignored OFF would skip. */
    HU_ASSERT_FALSE(should_skip("imessage"));
    env_restore(&e);
}

static void test_should_skip_shadow_never_skips(void) {
    reach_env_t e;
    env_save(&e);
    setenv("HU_PROACTIVE_REACHABILITY", "shadow", 1);
    /* HOLD verdict + SHADOW: would-exclude is logged, proposer still runs. */
    HU_ASSERT_FALSE(should_skip("imessage"));
    env_restore(&e);
}

static void test_should_skip_live_skips_only_unreachable(void) {
    reach_env_t e;
    env_save(&e);
    setenv("HU_PROACTIVE_REACHABILITY", "live", 1);
    /* Unreachable (stub HOLD) → skipped. */
    HU_ASSERT_TRUE(should_skip("imessage"));
    /* Reachable (ALLOW via the same override blue_guard honours) → not skipped.
     * Pins that the pre-filter tracks blue_guard's verdict, not the mode alone. */
    setenv("HU_IMESSAGE_ALLOW_GREEN", "1", 1);
    HU_ASSERT_FALSE(should_skip("imessage"));
    env_restore(&e);
}

static void test_should_skip_ignores_channels_without_an_oracle(void) {
    reach_env_t e;
    env_save(&e);
    setenv("HU_PROACTIVE_REACHABILITY", "live", 1);
    /* LIVE + HOLD would skip on iMessage; the same contact on another
     * channel has no reachability evidence and must PASS. */
    HU_ASSERT_FALSE(should_skip("telegram"));
    HU_ASSERT_FALSE(should_skip(NULL));
    env_restore(&e);
}

void run_daemon_proactive_reachability_tests(void) {
    HU_TEST_SUITE("DaemonProactiveReachability");
    HU_RUN_TEST(test_reach_mode_from_env_fails_closed);
    HU_RUN_TEST(test_reach_decide_truth_table);
    HU_RUN_TEST(test_should_skip_off_never_skips);
    HU_RUN_TEST(test_should_skip_shadow_never_skips);
    HU_RUN_TEST(test_should_skip_live_skips_only_unreachable);
    HU_RUN_TEST(test_should_skip_ignores_channels_without_an_oracle);
}
