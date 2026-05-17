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

/* Optional clear-callback for an opaque semantic cache. We can't depend
 * on the lifecycle/semantic_cache.h type from this header (kept as a
 * forward decl over there) so we let the caller bind whatever clear
 * primitive they have. NULL is fine. */
typedef void (*hu_semantic_cache_clear_fn)(void *cache);

typedef struct hu_lora_runner_ctx {
    hu_learner_t *learner;          /* required */
    struct hu_scheduler *scheduler; /* optional; enables follow-up KV warm */
    hu_kv_cache_manager_t *kv_cache;/* optional; cleared on adapter swap */
    void *semantic_cache;           /* optional; passed verbatim to clear_fn */
    hu_semantic_cache_clear_fn semantic_cache_clear_fn;
    hu_allocator_t *alloc;          /* optional; system allocator if NULL */
    hu_learner_config_t config_template; /* must have adapter_output_path set */

    /* W13 adapter auto-load: when non-NULL, the runner calls
     * hu_provider_load_adapter on the active provider after a successful
     * train so the new adapter is hot-loaded without daemon restart. */
    struct hu_provider *provider;   /* optional; NULL skips auto-load */
    const char *adapter_id;         /* optional; label for the loaded adapter */

    /* Phase 5 — promotion gate before hot-load (NULL skips). */
    struct hu_eval_gate *eval_gate;
    const char *rl_method_name; /* e.g. "dpo"; used for proof dir adapter id */
    size_t rl_step_index;

    /* CF-4 — persona example bank used to score the candidate adapter
     * through `hu_communication_style_fidelity_score_v2` before promotion.
     * Required when `eval_gate` is non-NULL. */
    const char *persona_name;
    const char *gate_model; /* optional provider model id for gate eval chats */
} hu_lora_runner_ctx_t;

hu_error_t hu_lora_training_runner(struct hu_memory_facade *m, const struct hu_job_spec *spec,
                                   int64_t budget_ms, void *user_data);

#ifdef HU_IS_TEST
void hu_lora_runner_set_test_clock(time_t frozen);

/* Last measured gate inputs fed to `hu_eval_gate_decide_from_arrays_for_test`
 * (CF-4). NULL / zero count when the runner has not run a gate yet. */
void hu_lora_runner_gate_capture_reset_for_test(void);
size_t hu_lora_runner_gate_capture_n_for_test(void);
double hu_lora_runner_gate_capture_persona_score_for_test(size_t index);
double hu_lora_runner_gate_capture_p95_ms_for_test(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_LORA_RUNNER_H */
