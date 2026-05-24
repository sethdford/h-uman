/* Spec 2026-05-19 — Shared training-runner entry for reaction-loop
 * pair-count auto-training trigger.
 *
 * Both the existing learner-pending trigger (≥10 W13 signals) AND the
 * new DPO pair-count trigger (≥ `learning.dpo_pair_training_threshold`
 * uncommitted pairs) route through `hu_training_runner_enqueue_lora_persona`.
 * This guarantees that the two triggers produce structurally identical
 * scheduler queue entries — only the `trigger_reason` differs (for
 * observability). See spec D-RL-2.
 *
 * The function is a thin wrapper over `hu_w14_scheduler_enqueue_lora`
 * (src/agent/world_model_bridge.c); it does not invent a new training
 * pipeline. The shared entry exists so callers don't drift apart.
 *
 * `trigger_reason` is logged at info level on success; intended values
 * are the constants below. NULL/empty is tolerated (logged as "unknown").
 *
 * When HU_ENABLE_LEARNING is OFF the function is a stub that returns
 * HU_OK without enqueueing, per
 * ~/.claude/rules/test-source-gate-symmetry.md. The shared header
 * surface stays identical across configurations so callers don't
 * `#ifdef`-guard call sites.
 *
 * Pair-count predicate `hu_training_runner_pair_count_should_fire` is
 * extracted so unit tests can pin the threshold-crossing semantic
 * without standing up a full scheduler. Pure function over
 * `uncommitted_count` and `threshold`; reads no globals. See
 * ~/.claude/rules/security-predicate-extraction.md. */
#ifndef HU_AGENT_TRAINING_RUNNER_SHARED_H
#define HU_AGENT_TRAINING_RUNNER_SHARED_H

#include "human/agent/world_model_bridge.h"
#include "human/core/error.h"
#include "human/core/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard `trigger_reason` constants. Plain strings; callers may pass
 * their own value but staying in this set keeps log analytics tidy. */
#define HU_TRAINING_TRIGGER_LEARNER_PENDING "learner_pending"
#define HU_TRAINING_TRIGGER_PAIR_COUNT      "pair_count_threshold"

/* Spec 2026-05-19 M3 closure / AC-M3-7 / D-M3-7 — training target.
 *
 * `huml_reference`: existing in-process HUML GPT path
 * (hu_lora_training_runner). Toy model, CPU, gradient-checkable.
 *
 * `frontier_mlx`: subprocess to scripts/m3_mlx_lora_bridge.py that
 * trains a real LoRA adapter against the served frontier model
 * (mlx-community/gemma-4-26b-a4b-it-4bit by default). Apple Silicon
 * required for real runs; tests use a fake shim
 * (tests/fixtures/m3/fake_mlx_lm_train.sh) under HU_IS_TEST. */
typedef enum hu_training_target_model {
    HU_TRAINING_TARGET_HUML_REFERENCE = 0,
    HU_TRAINING_TARGET_FRONTIER_MLX = 1,
} hu_training_target_model_t;

/* Enqueue a LoRA-persona training job. Both reaction-loop triggers MUST
 * call this function rather than `hu_w14_scheduler_enqueue_lora` directly,
 * so that future refactors of the enqueue contract land in one place.
 *
 * `scheduler` must be non-NULL. `now_ms` and `budget_ms` follow the same
 * convention as `hu_w14_scheduler_enqueue_lora` (now_ms = 0 → ASAP;
 * budget_ms = 0 → scheduler default). `trigger_reason` is borrowed; the
 * function does not retain or free it.
 *
 * `target` selects which training path: HUML_REFERENCE (existing
 * behavior, in-process HUML GPT) or FRONTIER_MLX (subprocess to
 * m3_mlx_lora_bridge.py against the served frontier model).
 *
 * Returns HU_OK on enqueue (or no-op stub when HU_ENABLE_LEARNING is OFF),
 * propagates any HU_ERR_* from the underlying scheduler enqueue. */
hu_error_t hu_training_runner_enqueue_lora_persona(hu_w14_scheduler_t *scheduler, int64_t now_ms,
                                                   int budget_ms, const char *trigger_reason,
                                                   hu_observer_t *observer);

/* Spec 2026-05-19 M3 closure / AC-M3-7 — same contract as
 * hu_training_runner_enqueue_lora_persona but takes an explicit target
 * model. The non-suffixed name is retained for backward compatibility
 * (defaults to HU_TRAINING_TARGET_HUML_REFERENCE).
 *
 * When target == HU_TRAINING_TARGET_FRONTIER_MLX, the underlying
 * scheduler enqueue still uses HU_JOB_LORA_TRAINING; the post-training
 * hook (registered by the daemon) is responsible for invoking the
 * subprocess training path AND the subsequent admin-swap call. The
 * shared entry's responsibility ends at "enqueue + log."
 *
 * For tests, the target selection AND the subprocess invocation are
 * gated by HU_IS_TEST so the fake mlx_lm shim runs instead of the real
 * bridge. See tests/test_m3_frontier_auto_invocation.c. */
hu_error_t hu_training_runner_enqueue_lora_persona_target(hu_w14_scheduler_t *scheduler,
                                                          int64_t now_ms, int budget_ms,
                                                          const char *trigger_reason,
                                                          hu_training_target_model_t target,
                                                          hu_observer_t *observer);

/* Spec 2026-05-19 M3 closure / AC-M3-7 — return the most recent target
 * enqueued via _target() (or HU_TRAINING_TARGET_HUML_REFERENCE if no
 * enqueue has happened). Lets tests verify that the daemon dispatched
 * with the expected target without standing up the full subprocess
 * pipeline.
 *
 * Process-wide single slot. Thread-safe for the daemon's
 * single-threaded scheduler tick; not designed for concurrent writers. */
hu_training_target_model_t hu_training_runner_last_enqueued_target(void);

/* Pure predicate. Returns true when (threshold > 0) AND (uncommitted_count
 * >= threshold). threshold == 0 is the operator-disabled state — always
 * false regardless of uncommitted_count. Negative inputs are treated as 0
 * (defensive — config parse clamps but this stays robust to caller bugs). */
bool hu_training_runner_pair_count_should_fire(size_t uncommitted_count, int threshold);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_TRAINING_RUNNER_SHARED_H */
