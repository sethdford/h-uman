#ifndef HU_AGENT_BEST_OF_N_H
#define HU_AGENT_BEST_OF_N_H

/*
 * US-7.7 (Sprint 7, P1) — Test-time persona scoring (best-of-N at inference).
 *
 * Higher-level decorator that wraps a provider's `chat` vtable method,
 * issuing up to N completions per agent turn and returning the candidate
 * with the highest `hu_communication_style_fidelity_score`. Lives ABOVE
 * the provider vtable (NOT inside any provider implementation) so:
 *
 *   - The provider vtable contract stays untouched (US-7.10 vtable
 *     stability policy).
 *   - The decorator stays agnostic across linked/unlinked llamacpp builds.
 *   - The existing daemon best-of-N (driven by `agent.best_of_n` +
 *     turing/persona heuristic) and this inference best-of-N (driven by
 *     `inference.best_of_n` + style fidelity) compose multiplicatively
 *     instead of fighting each other.
 *
 * MoLoRA adapter selection (US-7.8, when enabled) fires before best-of-N
 * sampling: the router picks an adapter, then this decorator samples N
 * completions against that adapter.
 *
 * Eligibility (gated at the call site in src/agent/agent_turn.c):
 *   - cfg->inference.best_of_n >= 2
 *   - provider->vtable->get_name() returns "llamacpp"
 *   - personal_model->style.sample_count > 0 (cold-start agents are
 *     skipped; with no fingerprint to score against, all candidates
 *     score -1.0 and best-of-N degenerates to "use first").
 *
 * Telemetry: one `best_of_n_pick` log line per invocation (and one
 * `best_of_n_cost_cap_hit` line when the cap fires). Four optional
 * counters propagated via `hu_best_of_n_stats_t` for the caller to
 * fold into agent telemetry without coupling the decorator to
 * `hu_agent_t`.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/personal_model.h"
#include "human/observer.h"
#include "human/provider.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-call statistics filled by the decorator. Caller may pass NULL. */
typedef struct hu_best_of_n_stats {
    uint32_t n_requested;  /* candidates requested (clamped to >= 2) */
    uint32_t n_completed;  /* candidates that actually completed (<= n_requested) */
    uint32_t picked_index; /* 0-based index of the returned candidate */
    bool cost_cap_hit;     /* true if the cost cap truncated the loop */
    bool all_unscored;     /* true if every candidate scored -1.0 (no fingerprint) */
    float picked_score;    /* fidelity score of the returned candidate; -1.0 if unscored */
    float min_score;       /* min fidelity across scored candidates; 1.0 when none scored */
    float max_score;       /* max fidelity across scored candidates; 0.0 when none scored */
} hu_best_of_n_stats_t;

/* Input bundle for the decorator. Mirrors the arg shape of
 * `provider->vtable->chat` (which is what the call site already uses)
 * plus the persona-style fingerprint used for scoring. */
typedef struct hu_best_of_n_config {
    hu_provider_t *provider;               /* required; must support `chat` */
    const hu_communication_style_t *style; /* required; scoring target */
    const hu_chat_request_t *request;      /* required; passed through unchanged each call */
    const char *model;                     /* may be NULL */
    size_t model_len;
    double temperature;
    uint32_t n;                      /* candidates to attempt; values <2 are clamped to 1 */
    uint32_t cost_cap_ms;            /* 0 = unlimited */
    hu_observer_t *observer;         /* optional; receives log lines */
    hu_best_of_n_stats_t *stats_out; /* optional */
} hu_best_of_n_config_t;

/* Decorator. Issues up to `cfg->n` calls into `cfg->provider->vtable->chat`,
 * scoring each result via `hu_communication_style_fidelity_score`. Writes
 * the best response into `*out` (ownership transfers to the caller via
 * `hu_chat_response_free`). Non-best responses are freed internally.
 *
 * Behavior:
 *   - cfg->n < 2  → single call, no scoring (passthrough).
 *   - First-call hard error (HU_ERR_*) → propagated.
 *   - Mid-loop error → return best-so-far; emit `best_of_n_partial_failure`.
 *   - All candidates score -1.0 → return first; emit `best_of_n_unscored_fallback`.
 *   - Cost cap exceeded after K completions → return best of K; emit
 *     `best_of_n_cost_cap_hit`. */
hu_error_t hu_best_of_n_chat(const hu_best_of_n_config_t *cfg, hu_allocator_t *alloc,
                             hu_chat_response_t *out);

/* Monotonic clock seam (function pointer). Production default uses
 * `clock_gettime(CLOCK_MONOTONIC)`. Tests inject a deterministic stepper
 * via `hu_best_of_n_set_clock_fn_for_test`. Returns time in nanoseconds. */
typedef uint64_t (*hu_best_of_n_clock_fn_t)(void);

#ifdef HU_IS_TEST
/* Test-only: replace the monotonic-clock backend with a deterministic
 * function pointer. Pass NULL to restore the production default. */
void hu_best_of_n_set_clock_fn_for_test(hu_best_of_n_clock_fn_t fn);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_BEST_OF_N_H */
