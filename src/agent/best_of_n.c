/*
 * US-7.7 (Sprint 7, P1) — Test-time persona scoring (best-of-N at inference).
 *
 * Higher-level decorator above the provider vtable. See
 * include/human/agent/best_of_n.h for the contract.
 *
 * Design notes:
 *   - We invoke `provider->vtable->chat(...)` N times rather than
 *     `chat_with_system(...)`. The call site in src/agent/agent_turn.c
 *     already uses `chat()` with a `hu_chat_request_t`; calling `chat()`
 *     keeps `hu_chat_response_t` (and `hu_chat_response_free`) as the
 *     ownership/free surface and avoids re-implementing the role-walking
 *     loop from llamacpp.c:370–378. The llamacpp `chat()` impl already
 *     splits sys/user internally.
 *   - The cost cap is a soft cap measured after each completion returns.
 *     We do not preempt an in-flight `llama_decode` (no thread
 *     cancellation introduced by this story).
 *   - The clock seam (`hu_best_of_n_clock_fn_t`) keeps tests deterministic
 *     without paying a virtual-dispatch cost in production.
 *   - MoLoRA adapter selection (US-7.8, when enabled) fires BEFORE this
 *     decorator: the router picks the adapter; we then sample N candidates
 *     against it.
 */

#include "human/agent/best_of_n.h"

#include "human/core/log.h"
#include "human/core/string.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Monotonic-clock seam ───────────────────────────────────────────── */

static uint64_t default_monotonic_ns(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
#else
    /* Portable fallback when CLOCK_MONOTONIC is unavailable. */
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
#endif
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static hu_best_of_n_clock_fn_t s_clock_fn = default_monotonic_ns;

#ifdef HU_IS_TEST
void hu_best_of_n_set_clock_fn_for_test(hu_best_of_n_clock_fn_t fn) {
    s_clock_fn = fn ? fn : default_monotonic_ns;
}
#endif

/* ── Helpers ────────────────────────────────────────────────────────── */

static void stats_init(hu_best_of_n_stats_t *s) {
    if (!s)
        return;
    s->n_requested = 0;
    s->n_completed = 0;
    s->picked_index = 0;
    s->cost_cap_hit = false;
    s->all_unscored = false;
    s->picked_score = -1.0f;
    s->min_score = 1.0f;
    s->max_score = 0.0f;
}

static void release_response(hu_allocator_t *alloc, hu_chat_response_t *r) {
    if (!r)
        return;
    hu_chat_response_free(alloc, r);
    memset(r, 0, sizeof(*r));
}

/* ── Decorator ──────────────────────────────────────────────────────── */

hu_error_t hu_best_of_n_chat(const hu_best_of_n_config_t *cfg, hu_allocator_t *alloc,
                             hu_chat_response_t *out) {
    if (!cfg || !alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!cfg->provider || !cfg->provider->vtable || !cfg->provider->vtable->chat)
        return HU_ERR_INVALID_ARGUMENT;
    if (!cfg->request)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    if (cfg->stats_out)
        stats_init(cfg->stats_out);

    uint32_t n = cfg->n;
    /* Defensive clamp: anything < 2 should not have reached us (eligibility
     * gate at the call site), but if it does, behave as plain passthrough. */
    if (n < 2) {
        hu_error_t e =
            cfg->provider->vtable->chat(cfg->provider->ctx, alloc, cfg->request, cfg->model,
                                        cfg->model_len, cfg->temperature, out);
        if (cfg->stats_out && e == HU_OK) {
            cfg->stats_out->n_requested = 1;
            cfg->stats_out->n_completed = 1;
            cfg->stats_out->picked_index = 0;
        }
        return e;
    }
    /* Hard upper bound: AC-7.7 doesn't pin this, but
     * config_validate clamps best_of_n to 8. Mirror the bound so a
     * pathological caller can't blow the stack/allocator. */
    if (n > 8)
        n = 8;

    if (cfg->stats_out)
        cfg->stats_out->n_requested = n;

    /* Per-candidate state, stack-allocated (bounded by clamp above). */
    hu_chat_response_t resps[8];
    float scores[8];
    bool valid[8];
    memset(resps, 0, sizeof(resps));
    for (uint32_t i = 0; i < n; ++i) {
        scores[i] = -1.0f;
        valid[i] = false;
    }

    uint64_t t_start = s_clock_fn();
    uint64_t cap_ns = (uint64_t)cfg->cost_cap_ms * 1000000ull; /* 0 ⇒ unlimited */

    uint32_t completed = 0;
    bool cost_cap_hit = false;
    bool partial_failure = false;
    hu_error_t first_err = HU_OK;

    for (uint32_t i = 0; i < n; ++i) {
        hu_chat_response_t r;
        memset(&r, 0, sizeof(r));
        hu_error_t e =
            cfg->provider->vtable->chat(cfg->provider->ctx, alloc, cfg->request, cfg->model,
                                        cfg->model_len, cfg->temperature, &r);
        if (e != HU_OK) {
            release_response(alloc, &r);
            if (i == 0) {
                /* First-call hard error → propagate; nothing useful to return. */
                first_err = e;
                goto fail;
            }
            /* Mid-loop error: keep best-so-far. */
            partial_failure = true;
            hu_log_info("best_of_n", cfg->observer,
                        "best_of_n_partial_failure index=%u err=%d n_completed=%u", (unsigned)i,
                        (int)e, (unsigned)completed);
            break;
        }

        resps[i] = r;
        valid[i] = true;
        completed++;

        /* Score this candidate. Fidelity scorer returns -1.0 when target has
         * no fingerprint (sample_count == 0) or inputs are NULL/empty. */
        float s = hu_communication_style_fidelity_score(cfg->style, r.content, r.content_len);
        scores[i] = s;

        /* Cost cap check (soft: fires after the completion returns). */
        if (cap_ns > 0 && i + 1 < n) {
            uint64_t now = s_clock_fn();
            if (now - t_start >= cap_ns) {
                cost_cap_hit = true;
                break;
            }
        }
    }

    if (completed == 0) {
        /* Shouldn't reach here — first-call failure paths above goto fail —
         * but defend against a clock-seam misuse that returns 0 candidates. */
        first_err = HU_ERR_PROVIDER_RESPONSE;
        goto fail;
    }

    /* Pick: argmax over scores; ties broken by lowest index (stable).
     * Unscored (score < 0) candidates lose to any scored one; if every
     * candidate is unscored, pick index 0. */
    uint32_t picked = 0;
    float picked_score = scores[0];
    float min_score = 1.0f;
    float max_score = 0.0f;
    uint32_t scored_count = 0;
    for (uint32_t i = 0; i < completed; ++i) {
        if (!valid[i])
            continue;
        if (scores[i] >= 0.0f) {
            if (scored_count == 0) {
                min_score = scores[i];
                max_score = scores[i];
            } else {
                if (scores[i] < min_score)
                    min_score = scores[i];
                if (scores[i] > max_score)
                    max_score = scores[i];
            }
            scored_count++;
            if (picked_score < 0.0f || scores[i] > picked_score) {
                picked = i;
                picked_score = scores[i];
            }
        }
    }

    bool all_unscored = (scored_count == 0);

    if (all_unscored) {
        /* Cold-start fallback: every candidate scored -1.0. Return index 0
         * for determinism. */
        picked = 0;
        picked_score = -1.0f;
        min_score = -1.0f;
        max_score = -1.0f;
        hu_log_info("best_of_n", cfg->observer, "best_of_n_unscored_fallback n=%u n_completed=%u",
                    (unsigned)n, (unsigned)completed);
    }

    /* Move ownership of the winning response to `out`; free the rest. */
    *out = resps[picked];
    memset(&resps[picked], 0, sizeof(resps[picked]));
    valid[picked] = false;
    for (uint32_t i = 0; i < completed; ++i) {
        if (valid[i])
            release_response(alloc, &resps[i]);
    }

    /* Telemetry: one pick line per turn (aggregate; no per-candidate spam). */
    if (!all_unscored) {
        hu_log_info("best_of_n", cfg->observer,
                    "best_of_n_pick n=%u n_completed=%u picked_index=%u picked_score=%.2f "
                    "min_score=%.2f max_score=%.2f%s%s",
                    (unsigned)n, (unsigned)completed, (unsigned)picked, (double)picked_score,
                    (double)min_score, (double)max_score, cost_cap_hit ? " cost_cap=hit" : "",
                    partial_failure ? " partial_failure=1" : "");
    }
    if (cost_cap_hit) {
        hu_log_info("best_of_n", cfg->observer,
                    "best_of_n_cost_cap_hit n=%u n_completed=%u cost_cap_ms=%u", (unsigned)n,
                    (unsigned)completed, (unsigned)cfg->cost_cap_ms);
    }

    if (cfg->stats_out) {
        cfg->stats_out->n_completed = completed;
        cfg->stats_out->picked_index = picked;
        cfg->stats_out->cost_cap_hit = cost_cap_hit;
        cfg->stats_out->all_unscored = all_unscored;
        cfg->stats_out->picked_score = picked_score;
        cfg->stats_out->min_score = all_unscored ? -1.0f : min_score;
        cfg->stats_out->max_score = all_unscored ? -1.0f : max_score;
    }

    return HU_OK;

fail:
    for (uint32_t i = 0; i < n; ++i) {
        if (valid[i])
            release_response(alloc, &resps[i]);
    }
    memset(out, 0, sizeof(*out));
    return first_err;
}
