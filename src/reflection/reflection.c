/* src/reflection/reflection.c — Orchestration (T5).
 *
 * Spec: docs/plans/2026-05-26-reflection-loop/{design.md, tasks.md}
 * Task 5.
 *
 * Two pieces:
 *
 *   hu_reflection_should_run — pure gate, mirror of
 *     init_proposer.c::hu_init_proposer_tick. Truth table:
 *       disabled         → DISABLED       (cfg gate, never bypassable)
 *       force=true       → RUN_FORCED     (operator override; disabled still wins)
 *       no prior run     → idle/daily-floor still apply, just measured from epoch 0
 *       <min_interval    → INTERVAL
 *       idle < threshold AND time-since-last-run < daily_floor
 *                        → NOT_IDLE
 *       idle ≥ threshold (and min_interval already passed)
 *                        → RUN_IDLE
 *       daily_floor exceeded (regardless of idle)
 *                        → RUN_FORCED
 *
 *   hu_reflection_run — I/O wrapper. Drives the inputs struct through
 *     T4's build_input → provider's chat_with_system → T1's parse →
 *     T2's upsert + complete_run. All five subsystems converge here.
 *
 * Failure-mode policy: the four-layer error model from design.md is
 * encoded in the run status enum + which storage row gets written.
 * Test cases below cover each branch.
 *
 * One-shot disabled-log discipline: per silent-config-gated-subsystems.md
 * we emit ONE info-level log line when the subsystem is first seen
 * disabled, and one when it's first seen enabled. atomic_bool flags
 * gate this so concurrent ticks don't double-fire. */

#include "human/reflection.h"

#include "human/config_types.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/provider.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── One-shot disabled/enabled-warning flags ─────────────────────── */

static atomic_bool g_warned_disabled = false;
static atomic_bool g_warned_enabled = false;

/* Test hook — resets one-shot flags so multiple test cases can each
 * exercise the "first time disabled" + "first time enabled" log paths
 * without leaking state across tests. */
void hu_reflection_reset_warn_guards_for_test(void) {
    atomic_store(&g_warned_disabled, false);
    atomic_store(&g_warned_enabled, false);
}

/* ── Pure gate ───────────────────────────────────────────────────── */

hu_reflection_gate_result_t hu_reflection_should_run(const hu_reflection_loop_config_t *cfg,
                                                     uint64_t last_completed_ms,
                                                     uint64_t last_user_activity_ms,
                                                     uint64_t now_ms, bool force) {
    /* Disabled gate is unconditional — operator config wins. Force
     * cannot bypass it because we don't want a runtime poke to
     * resurrect a subsystem the operator explicitly turned off. */
    if (!cfg || !cfg->enabled)
        return HU_REFLECTION_GATE_DISABLED;

    /* Force bypasses both interval and idle. Use this for tests and
     * manual operator triggers ("reflect NOW"). */
    if (force)
        return HU_REFLECTION_GATE_RUN_FORCED;

    /* Read interval gates as ms — config stores hours, multiply once
     * here so the rest of the function is uniform. */
    uint64_t min_interval_ms = (uint64_t)cfg->min_interval_hours * 3600ULL * 1000ULL;
    uint64_t idle_threshold_ms = (uint64_t)cfg->idle_threshold_hours * 3600ULL * 1000ULL;
    uint64_t daily_floor_ms = (uint64_t)cfg->daily_floor_hours * 3600ULL * 1000ULL;

    /* min_interval — even the daily floor must respect this. If we're
     * inside the interval window after a recent run, skip outright.
     * Special case: last_completed_ms == 0 means "no prior run yet"
     * → interval gate passes (cold start). */
    if (last_completed_ms > 0 && now_ms - last_completed_ms < min_interval_ms)
        return HU_REFLECTION_GATE_INTERVAL;

    /* daily_floor override: if it's been longer than daily_floor since
     * the last run, fire regardless of idle. Without this, a user who
     * never actually idles (heavy day) would never get reflection. */
    if (last_completed_ms > 0 && now_ms - last_completed_ms >= daily_floor_ms)
        return HU_REFLECTION_GATE_RUN_FORCED;

    /* Idle check — time since last user message ≥ threshold? Treat
     * last_user_activity_ms == 0 as "infinitely idle" so a fresh
     * daemon (no inbound traffic yet) still reflects when the floor
     * is hit. */
    uint64_t idle_ms = (last_user_activity_ms == 0) ? UINT64_MAX : (now_ms - last_user_activity_ms);
    if (idle_ms < idle_threshold_ms)
        return HU_REFLECTION_GATE_NOT_IDLE;

    return HU_REFLECTION_GATE_RUN_IDLE;
}

/* ── Run wrapper ─────────────────────────────────────────────────── */

/* Generate a run_id from a wall-clock timestamp. Format: "refl_<ms>".
 * Collisions only happen if two runs fire in the same ms, which the
 * interval gate prevents — but as defense in depth we also append a
 * monotonic counter that survives this process's lifetime. */
static void format_run_id(uint64_t now_ms, char *out, size_t cap) {
    static atomic_uint_fast32_t g_counter = 0;
    uint32_t seq = atomic_fetch_add(&g_counter, 1) + 1;
    snprintf(out, cap, "refl_%llu_%u", (unsigned long long)now_ms, seq);
}

/* Read config provider name with sensible fallback. Used in both the
 * insert_run row AND the model param to chat_with_system. */
static const char *effective_provider_name(const hu_reflection_loop_config_t *cfg) {
    if (cfg && cfg->provider[0])
        return cfg->provider;
    return "gemini-3.5-flash";
}

hu_error_t hu_reflection_run(const hu_reflection_run_inputs_t *inputs, bool force,
                             hu_reflection_run_status_t *out_status, int *out_patterns_kept,
                             int *out_patterns_dropped) {
    /* Defensive-initialise the OUT params so callers that pass them
     * but ignore the return code still see sane values. */
    if (out_status)
        *out_status = HU_REFLECTION_RUN_GATED;
    if (out_patterns_kept)
        *out_patterns_kept = 0;
    if (out_patterns_dropped)
        *out_patterns_dropped = 0;

    if (!inputs || !inputs->db || !inputs->cfg || !inputs->provider || !inputs->alloc ||
        !inputs->iter_fn) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (!inputs->provider->vtable || !inputs->provider->vtable->chat_with_system) {
        /* Treat as provider error rather than INVALID_ARGUMENT — the
         * inputs are syntactically fine; this provider just doesn't
         * implement what we need. */
        if (out_status)
            *out_status = HU_REFLECTION_RUN_PROVIDER_ERROR;
        return HU_OK;
    }

    /* One-shot enabled-log: announce activation exactly once. The
     * gate's DISABLED return will emit the disabled-log; we don't
     * duplicate it here. */
    if (inputs->cfg->enabled) {
        hu_log_info_once(&g_warned_enabled, "reflection", NULL,
                         "reflection subsystem activated by config "
                         "(provider=%s, min_interval=%dh, idle=%dh, daily_floor=%dh)",
                         effective_provider_name(inputs->cfg), inputs->cfg->min_interval_hours,
                         inputs->cfg->idle_threshold_hours, inputs->cfg->daily_floor_hours);
    } else {
        hu_log_info_once(&g_warned_disabled, "reflection", NULL,
                         "reflection subsystem disabled by config "
                         "(cfg->reflection_loop.enabled=false); set reflection.enabled=true "
                         "in config.json to activate");
    }

    /* Gate. */
    uint64_t last_completed_ms = hu_reflection_storage_last_completed_ms(inputs->db);
    hu_reflection_gate_result_t gate = hu_reflection_should_run(
        inputs->cfg, last_completed_ms, inputs->last_user_activity_ms, inputs->now_ms, force);
    if (gate != HU_REFLECTION_GATE_RUN_IDLE && gate != HU_REFLECTION_GATE_RUN_FORCED) {
        if (out_status)
            *out_status = HU_REFLECTION_RUN_GATED;
        return HU_OK;
    }

    /* Build input transcript. */
    size_t max_chars = inputs->max_input_chars > 0 ? inputs->max_input_chars : (size_t)100 * 1024;
    char *user_msg = NULL;
    int turn_count = 0;
    hu_error_t be = hu_reflection_build_input(inputs->iter_fn, inputs->iter_ctx, max_chars,
                                              &user_msg, &turn_count);
    if (be != HU_OK) {
        if (out_status)
            *out_status = HU_REFLECTION_RUN_STORAGE_ERROR; /* OOM treated like a storage failure */
        return HU_OK;
    }
    if (turn_count == 0) {
        /* No turns → no run row inserted. We don't want a wall of
         * "in_progress" stubs when the daemon idles for days; only
         * runs that actually consult the model deserve a row. */
        free(user_msg);
        if (out_status)
            *out_status = HU_REFLECTION_RUN_NO_INPUT;
        return HU_OK;
    }

    /* Insert in-progress run row. */
    char run_id[64];
    format_run_id(inputs->now_ms, run_id, sizeof(run_id));
    const char *provider_name = effective_provider_name(inputs->cfg);
    hu_error_t ir = hu_reflection_storage_insert_run(inputs->db, run_id, provider_name,
                                                     inputs->now_ms, turn_count);
    if (ir != HU_OK) {
        free(user_msg);
        if (out_status)
            *out_status = HU_REFLECTION_RUN_STORAGE_ERROR;
        return HU_OK;
    }

    /* Provider call. The system prompt is the static template; the
     * user message is the assembled transcript. Temperature is low
     * (0.2) to nudge the model toward stable JSON shape. */
    const char *system_prompt = hu_reflection_system_prompt();
    char *response = NULL;
    size_t response_len = 0;
    hu_error_t ce = inputs->provider->vtable->chat_with_system(
        inputs->provider->ctx, inputs->alloc, system_prompt, strlen(system_prompt), user_msg,
        strlen(user_msg), provider_name, strlen(provider_name), 0.2, &response, &response_len);
    free(user_msg);

    if (ce != HU_OK || !response) {
        hu_reflection_storage_complete_run(inputs->db, run_id, "provider_error", 0, NULL, NULL,
                                           hu_error_string(ce), 0);
        if (response && inputs->alloc && inputs->alloc->free)
            inputs->alloc->free(inputs->alloc->ctx, response, response_len);
        if (out_status)
            *out_status = HU_REFLECTION_RUN_PROVIDER_ERROR;
        return HU_OK;
    }

    /* Parse. */
    hu_reflection_pattern_t *patterns = NULL;
    int pattern_count = 0;
    char *prose = NULL;
    char *parse_err = NULL;
    hu_error_t pe = hu_reflection_parse(response, &patterns, &pattern_count, &prose, &parse_err);
    /* Free provider response via its allocator — chat_with_system
     * allocated it through inputs->alloc per the vtable contract. */
    if (inputs->alloc && inputs->alloc->free && response)
        inputs->alloc->free(inputs->alloc->ctx, response, response_len);
    if (pe != HU_OK) {
        hu_reflection_storage_complete_run(inputs->db, run_id, "schema_invalid", 0, NULL, NULL,
                                           parse_err ? parse_err : "parse failed", 0);
        free(patterns);
        free(prose);
        free(parse_err);
        if (out_status)
            *out_status = HU_REFLECTION_RUN_SCHEMA_INVALID;
        return HU_OK;
    }

    /* UPSERT each pattern. Confidence floor (< 0.5) is applied INSIDE
     * the storage layer; we count drops by comparing against the
     * parsed count. The parser already filled in `id` via
     * hu_reflection_compute_id. */
    int kept = 0;
    int dropped = 0;
    for (int i = 0; i < pattern_count; i++) {
        if (patterns[i].confidence < 0.5) {
            dropped++;
            continue;
        }
        /* Make sure created_at/last_observed/expires are set —
         * defensive in case the parser didn't fill them. The half-life
         * is 30 days per the design. */
        if (patterns[i].created_at_ms == 0)
            patterns[i].created_at_ms = inputs->now_ms;
        if (patterns[i].last_observed_at_ms == 0)
            patterns[i].last_observed_at_ms = inputs->now_ms;
        if (patterns[i].expires_at_ms == 0)
            patterns[i].expires_at_ms = inputs->now_ms + 30ULL * 86400ULL * 1000ULL;
        hu_error_t ue = hu_reflection_storage_upsert(inputs->db, run_id, &patterns[i]);
        if (ue == HU_OK)
            kept++;
        else
            dropped++;
    }

    /* Complete run row. output_tokens is unknown here (the provider's
     * chat_with_system flavor doesn't return token counts); pass 0 —
     * T4-streaming follow-up can plumb it. */
    hu_reflection_storage_complete_run(inputs->db, run_id, "ok", 0, prose, NULL, NULL, dropped);

    free(patterns);
    free(prose);
    free(parse_err);

    if (out_status)
        *out_status = HU_REFLECTION_RUN_OK;
    if (out_patterns_kept)
        *out_patterns_kept = kept;
    if (out_patterns_dropped)
        *out_patterns_dropped = dropped;
    return HU_OK;
}
