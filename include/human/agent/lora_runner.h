#ifndef HU_AGENT_LORA_RUNNER_H
#define HU_AGENT_LORA_RUNNER_H

/* W14 — LoRA training scheduler runner.
 *
 * Bridges the W13 learner into the W14 sleep-time scheduler. Sleep-time
 * compute is the only sanctioned path for actual gradient updates: the
 * agent turn never trains inline, only emits signals via
 * `hu_learner_bridge_emit_*`, which queue them on the learner's pending
 * buffer. This runner drains that buffer and calls `hu_learner_train`
 * with a caller-pinned config template.
 *
 * After a successful adapter write, the runner clears the KV cache and
 * semantic cache (if provided) so the next user turn cannot serve a
 * cached completion produced by the old adapter. It also enqueues a
 * follow-up `HU_JOB_KV_CACHE_WARMING` so the new cache state is
 * pre-populated rather than lazily filled.
 *
 * `user_data` is `hu_lora_runner_ctx_t *`. The caller owns every pointer
 * inside it; the runner does not free anything in the context.
 *
 * Layer 4 of the v2 stack. Tested in tests/test_w14_runners.c. */

#include "human/agent/kv_cache.h"
#include "human/core/allocator.h"
#include "human/ml/learner.h"
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward only — do not include scheduler.h here: that pulls W7
 * `human/memory/memory.h`, which collides with legacy `human/memory.h`
 * in any TU that already includes `human/agent.h` (e.g. daemon.c). */
struct hu_scheduler;
struct hu_memory_facade;
struct hu_job_spec;
struct hu_provider;
struct hu_eval_gate;
struct hu_communication_style;
struct hu_persona;

/* Optional clear-callback for an opaque semantic cache. We can't depend
 * on the lifecycle/semantic_cache.h type from this header (kept as a
 * forward decl over there) so we let the caller bind whatever clear
 * primitive they have. NULL is fine. */
typedef void (*hu_semantic_cache_clear_fn)(void *cache);

typedef struct hu_lora_runner_ctx {
    hu_learner_t *learner;           /* required */
    struct hu_scheduler *scheduler;  /* optional; enables follow-up KV warm */
    hu_kv_cache_manager_t *kv_cache; /* optional; cleared on adapter swap */
    void *semantic_cache;            /* optional; passed verbatim to clear_fn */
    hu_semantic_cache_clear_fn semantic_cache_clear_fn;
    hu_allocator_t *alloc;               /* optional; system allocator if NULL */
    hu_learner_config_t config_template; /* must have adapter_output_path set */

    /* W13 adapter auto-load: when non-NULL, the runner calls
     * hu_provider_load_adapter on the active provider after a successful
     * train so the new adapter is hot-loaded without daemon restart. */
    struct hu_provider *provider; /* optional; NULL skips auto-load */
    const char *adapter_id;       /* optional; label for the loaded adapter */

    /* Phase 5 — promotion gate before hot-load (NULL skips). */
    struct hu_eval_gate *eval_gate;
    const char *rl_method_name; /* e.g. "dpo"; used for proof dir adapter id */
    size_t rl_step_index;

    /* CF-4 — measured persona scores for the gate (after train).
     * When non-NULL and gate_persona_after_n >= 10, used instead of rollout. */
    const double *gate_persona_after_scores;
    size_t gate_persona_after_n;
    double gate_candidate_p95_ms; /* 0 → use rollout p95 or default 100 ms */

    /* CF-4 — real rollout measurement (required when eval_gate is set in
     * production unless gate_persona_after_scores is pre-filled). */
    struct hu_provider *eval_provider;
    const char *eval_prompt_fixture_path;
    const struct hu_communication_style *eval_target;
    size_t eval_n_prompts;   /* default 20 when 0 */
    int64_t eval_timeout_ms; /* per-chat budget; default 5000 when 0 */

    /* CF-4 (finish) — score persona example bank + post-adapter probes. */
    const struct hu_persona *gate_persona; /* not owned */
    const char *gate_model_name;
    size_t gate_model_name_len;
#ifdef HU_IS_TEST
    bool eval_use_synthetic_for_test; /* explicit 0.75 array; never in production */
#endif
} hu_lora_runner_ctx_t;

hu_error_t hu_lora_training_runner(struct hu_memory_facade *m, const struct hu_job_spec *spec,
                                   int64_t budget_ms, void *user_data);

/* Minimum seconds between training ATTEMPTS (not successes).
 *
 * 2026-07-26: eleven frontier-MLX dispatches fired in one day, six inside 28
 * minutes (06:09-06:37), because the only trigger condition was a pair-count
 * threshold crossing with nothing rate-limiting the retry. Each dispatch loads
 * the 56 GB serving base for training, so the pile-up drove a 128 GB machine to
 * 154 MB free and four reboots. training_loop.py now REFUSES such runs, but the
 * daemon should not keep spawning subprocesses destined to refuse. */
#define HU_LORA_RUNNER_ATTEMPT_COOLDOWN_SECONDS (6 * 60 * 60)

/* True when `now` is inside the cooldown following `last_attempt`.
 *
 * Pure predicate over injected facts so the whole truth table is testable
 * without a clock, a filesystem, or a 56 GB model
 * (.claude/rules/security-predicate-extraction.md).
 *
 * last_attempt <= 0 means "never attempted" and is NEVER in cooldown, so a
 * missing or unreadable stamp file fails OPEN (allows the attempt) rather than
 * wedging training forever. A last_attempt in the future (clock moved
 * backwards) is treated as in-cooldown — the conservative direction. */
bool hu_lora_runner_attempt_cooldown_active(time_t last_attempt, time_t now, int cooldown_seconds);

#ifdef HU_IS_TEST
void hu_lora_runner_set_test_clock(time_t frozen);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_LORA_RUNNER_H */
