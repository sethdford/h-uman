/* Spec 2026-05-19 — Shared training-runner entry for the reaction-loop
 * pair-count auto-training trigger. See header for the contract.
 *
 * The translation unit is unconditionally compiled (always in
 * `HU_CORE_SOURCES`); the actual enqueue body switches on
 * HU_ENABLE_LEARNING. This keeps callers free of `#ifdef` guards at
 * the call site and satisfies the gate-symmetry rule
 * (~/.claude/rules/test-source-gate-symmetry.md) by providing a stub
 * implementation in the disabled build. */

#include "human/agent/training_runner_shared.h"

#include <stdatomic.h>
#include <string.h>

/* Spec 2026-05-19 M3 closure / AC-M3-7 — process-wide single slot
 * for the most recent target enqueued. Tests read this to verify the
 * daemon dispatched with the expected target without standing up the
 * full subprocess pipeline. Not thread-safe; the daemon scheduler tick
 * is single-threaded so this is fine in production. */
static hu_training_target_model_t g_last_enqueued_target = HU_TRAINING_TARGET_HUML_REFERENCE;

bool hu_training_runner_pair_count_should_fire(size_t uncommitted_count, int threshold) {
    /* threshold == 0 → operator explicitly disabled. NEVER fire. */
    if (threshold <= 0)
        return false;
    return uncommitted_count >= (size_t)threshold;
}

hu_error_t hu_training_runner_enqueue_lora_persona(hu_w14_scheduler_t *scheduler, int64_t now_ms,
                                                   int budget_ms, const char *trigger_reason,
                                                   hu_observer_t *observer) {
    /* Backward-compatible wrapper — defaults to the existing in-process
     * HUML reference path. */
    return hu_training_runner_enqueue_lora_persona_target(
        scheduler, now_ms, budget_ms, trigger_reason, HU_TRAINING_TARGET_HUML_REFERENCE, observer);
}

hu_error_t hu_training_runner_enqueue_lora_persona_target(hu_w14_scheduler_t *scheduler,
                                                          int64_t now_ms, int budget_ms,
                                                          const char *trigger_reason,
                                                          hu_training_target_model_t target,
                                                          hu_observer_t *observer) {
    if (!scheduler)
        return HU_ERR_INVALID_ARGUMENT;

    const char *reason = (trigger_reason && trigger_reason[0]) ? trigger_reason : "unknown";
    const char *target_str =
        (target == HU_TRAINING_TARGET_FRONTIER_MLX) ? "frontier_mlx" : "huml_reference";

#if defined(HU_ENABLE_LEARNING)
    hu_error_t e = hu_w14_scheduler_enqueue_lora(scheduler, now_ms, budget_ms);
    if (e == HU_OK) {
        g_last_enqueued_target = target;
        hu_log_info("training-runner", observer,
                    "enqueued LoRA-persona training (trigger=%s, target=%s, now_ms=%lld, "
                    "budget_ms=%d)",
                    reason, target_str, (long long)now_ms, budget_ms);
    } else {
        hu_log_warn("training-runner", observer,
                    "failed to enqueue LoRA-persona training (trigger=%s, target=%s): %s", reason,
                    target_str, hu_error_string(e));
    }
    return e;
#else
    /* HU_ENABLE_LEARNING=OFF — no W13/W14 training pipeline available.
     * Treat as a no-op success so callers don't need conditional
     * compilation; the daemon-tick predicate still runs but never
     * triggers a real enqueue. The target slot still advances so
     * tests can verify dispatch intent even in the disabled build. */
    (void)now_ms;
    (void)budget_ms;
    (void)reason;
    (void)target_str;
    (void)observer;
    g_last_enqueued_target = target;
    return HU_OK;
#endif
}

hu_training_target_model_t hu_training_runner_last_enqueued_target(void) {
    return g_last_enqueued_target;
}

/* US-8 / M3 Phase B3 — post-train adapter swap counter. Process-wide
 * atomic, monotonic for the process lifetime. Lives in the always-
 * compiled TU so the disabled-learning build still satisfies the public
 * getter signature (returns 0 because the increment site is gated). */
static atomic_uint_least64_t g_post_train_swap_attempts;

uint64_t hu_training_runner_post_train_swap_attempts(void) {
    return (uint64_t)atomic_load_explicit(&g_post_train_swap_attempts, memory_order_relaxed);
}

void hu_training_runner_post_train_swap_attempts_increment_internal(void) {
    atomic_fetch_add_explicit(&g_post_train_swap_attempts, 1, memory_order_relaxed);
}

#ifdef HU_IS_TEST
void hu_training_runner_test_set_last_enqueued_target(hu_training_target_model_t target) {
    g_last_enqueued_target = target;
}
#endif
