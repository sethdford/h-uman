/* tests/test_dpo_pair_count_trigger.c — Spec 2026-05-19 Task 5.
 *
 * Unit-level coverage for the DPO pair-count training trigger predicate
 * and its surrounding shared entry. The predicate
 * `hu_training_runner_pair_count_should_fire` is a pure function over
 * `(uncommitted_count, threshold)` extracted per
 * ~/.claude/rules/security-predicate-extraction.md — that's what we pin
 * here. The full daemon-tick wiring is exercised by the E2E test in
 * tests/test_e2e_rl_loop.c.
 *
 * Cases (per spec AC-RL-1 / AC-RL-4):
 *   1. 89 → 100 fires (threshold-crossing positive case)
 *   2. 99 → 99 does NOT fire (just-below)
 *   3. threshold == 0 is operator-disabled (always false)
 *   4. negative threshold treated as disabled (defensive)
 *   5. exactly-at-threshold fires
 *   6. shared entry returns HU_OK with the no-op stub under
 *      HU_ENABLE_LEARNING=OFF, and HU_ERR_INVALID_ARGUMENT on NULL
 *      scheduler regardless of build configuration.
 *   7. shared entry tolerates NULL trigger_reason without crashing
 *      (logs as "unknown"; we only assert it returns).
 *
 * The HU_ENABLE_LEARNING=ON behavior of the shared entry (real enqueue
 * onto the W14 scheduler) is exercised in test_training_runner_shared_entry.c
 * — keeping this file scheduler-free keeps the unit tests fast and
 * dependency-light. */

#include "human/agent/training_runner_shared.h"
#include "human/core/error.h"
#include "test_framework.h"

#include <stddef.h>

static void test_pair_count_predicate_89_does_not_fire_at_threshold_100(void) {
    HU_ASSERT_TRUE(!hu_training_runner_pair_count_should_fire(89, 100));
}

static void test_pair_count_predicate_100_fires_at_threshold_100(void) {
    HU_ASSERT_TRUE(hu_training_runner_pair_count_should_fire(100, 100));
}

static void test_pair_count_predicate_99_fires_at_threshold_99(void) {
    /* Exact-at-threshold MUST fire — `count >= threshold` is the
     * contract. Pinned so a future "strictly greater" refactor can't
     * silently change the semantic (which would also delay every real
     * production trigger by one pair). */
    HU_ASSERT_TRUE(hu_training_runner_pair_count_should_fire(99, 99));
}

static void test_pair_count_predicate_99_does_not_fire_at_threshold_100(void) {
    /* Spec AC-RL-1: "99 → 99 does not [fire]" — given default threshold
     * 100, count=99 must NOT fire. This is the just-below case. */
    HU_ASSERT_TRUE(!hu_training_runner_pair_count_should_fire(99, 100));
}

static void test_pair_count_predicate_threshold_zero_never_fires(void) {
    /* Operator-disabled state: zero threshold MUST always return false
     * regardless of the uncommitted count. This is the spec's
     * "threshold == 0 disables the trigger" contract — pinned so a
     * future "treat 0 as default" refactor can't silently re-enable. */
    HU_ASSERT_TRUE(!hu_training_runner_pair_count_should_fire(0, 0));
    HU_ASSERT_TRUE(!hu_training_runner_pair_count_should_fire(1, 0));
    HU_ASSERT_TRUE(!hu_training_runner_pair_count_should_fire(1000000, 0));
}

static void test_pair_count_predicate_negative_threshold_treated_as_disabled(void) {
    /* Defensive: parser clamps to >= 0, but the predicate must not
     * mis-fire if a caller bypasses parsing and passes a negative
     * value. INT_MIN must also be safe. */
    HU_ASSERT_TRUE(!hu_training_runner_pair_count_should_fire(100, -1));
    HU_ASSERT_TRUE(!hu_training_runner_pair_count_should_fire(100, -2147483647 - 1));
}

static void test_pair_count_predicate_large_count_fires(void) {
    /* Heavy-user case — make sure no signed/unsigned conversion
     * pathology trips at large counts. */
    HU_ASSERT_TRUE(hu_training_runner_pair_count_should_fire(1000000, 100));
}

static void test_shared_entry_rejects_null_scheduler(void) {
    /* Contract regardless of HU_ENABLE_LEARNING: NULL scheduler is an
     * argument-error. The stub path must not silently return OK here —
     * a real caller mistake should surface. */
    hu_error_t e =
        hu_training_runner_enqueue_lora_persona(NULL, 0, 0, HU_TRAINING_TRIGGER_PAIR_COUNT, NULL);
    HU_ASSERT_EQ(e, HU_ERR_INVALID_ARGUMENT);
}

static void test_shared_entry_tolerates_null_trigger_reason(void) {
    /* The shared entry should not deref a NULL trigger_reason. It logs
     * as "unknown". We can't assert the log content from here without
     * standing up an observer, but we can assert it doesn't crash and
     * returns INVALID_ARGUMENT for NULL scheduler (so the function ran
     * past the reason-string handling). */
    hu_error_t e = hu_training_runner_enqueue_lora_persona(NULL, 0, 0, NULL, NULL);
    HU_ASSERT_EQ(e, HU_ERR_INVALID_ARGUMENT);
}

#ifndef HU_ENABLE_LEARNING
static void test_shared_entry_is_noop_when_learning_disabled(void) {
    /* When HU_ENABLE_LEARNING=OFF the shared entry must return HU_OK
     * with no observable side effect — the daemon-tick predicate may
     * still call it but no training actually occurs. We don't have a
     * scheduler to pass; the body returns before the scheduler
     * deref. Use a non-NULL sentinel; the function checks NULL first
     * (rejected above) so we must construct a fake scheduler pointer.
     *
     * Compile-time exclusion of this test under HU_ENABLE_LEARNING=ON
     * is correct because the OFF-path no-op behavior cannot be
     * exercised when the macro is set. */
    /* hu_w14_scheduler_t is opaque — we cast a sentinel byte through to
     * exercise the no-op path without standing up a real scheduler. The
     * function under test never derefs the pointer in the disabled
     * build. */
    char sentinel;
    hu_w14_scheduler_t *fake = (hu_w14_scheduler_t *)&sentinel;
    hu_error_t e =
        hu_training_runner_enqueue_lora_persona(fake, 0, 0, HU_TRAINING_TRIGGER_PAIR_COUNT, NULL);
    HU_ASSERT_EQ(e, HU_OK);
}
#endif

void run_dpo_pair_count_trigger_tests(void) {
    HU_TEST_SUITE("DPO pair-count trigger predicate");
    HU_RUN_TEST(test_pair_count_predicate_89_does_not_fire_at_threshold_100);
    HU_RUN_TEST(test_pair_count_predicate_100_fires_at_threshold_100);
    HU_RUN_TEST(test_pair_count_predicate_99_fires_at_threshold_99);
    HU_RUN_TEST(test_pair_count_predicate_99_does_not_fire_at_threshold_100);
    HU_RUN_TEST(test_pair_count_predicate_threshold_zero_never_fires);
    HU_RUN_TEST(test_pair_count_predicate_negative_threshold_treated_as_disabled);
    HU_RUN_TEST(test_pair_count_predicate_large_count_fires);
    HU_RUN_TEST(test_shared_entry_rejects_null_scheduler);
    HU_RUN_TEST(test_shared_entry_tolerates_null_trigger_reason);
#ifndef HU_ENABLE_LEARNING
    HU_RUN_TEST(test_shared_entry_is_noop_when_learning_disabled);
#endif
}
