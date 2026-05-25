/* src/agent/init_proposer.c
 *
 * Initiative Layer — T1 skeleton (governor-only, always SKIP).
 *
 * See docs/plans/2026-05-25-initiative-layer/{requirements,design,tasks}.md.
 * This file implements AC-1 (scheduler ticks), AC-6 (loud failure on silent
 * gating), and AC-7 (reversible kill switch). AC-2 (context bundle), AC-3
 * (governor with confidence), AC-4 (SKIP-default fast path), and AC-5
 * (delivery via existing channels) land in T2/T3/T4.
 */

#include "human/agent/init_proposer.h"
#include "human/agent.h"
#include "human/agent/governor.h"
#include "human/autoresponder.h"
#include "human/config.h"
#include "human/core/log.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Per ~/.claude/rules/silent-config-gated-subsystems.md: emit ONE
 * operator-visible log line per process when the subsystem is disabled or
 * enabled. Guards are process-scoped via atomic_bool. */
static atomic_bool g_warned_disabled = false;
static atomic_bool g_warned_enabled = false;

void hu_init_proposer_reset_warn_guards_for_test(void) {
#if HU_IS_TEST
    atomic_store(&g_warned_disabled, false);
    atomic_store(&g_warned_enabled, false);
#endif
}

hu_error_t hu_init_proposer_tick(const struct hu_initiative_config *cfg,
                                 const struct hu_autoresponder_config *ar_cfg,
                                 int32_t tz_offset_seconds, struct hu_proactive_budget *budget,
                                 int64_t last_inbound_unix, int64_t now_unix,
                                 int64_t *last_tick_unix_inout, uint64_t *tick_id_inout,
                                 hu_init_proposer_result_t *out_result) {
    if (!cfg || !last_tick_unix_inout || !tick_id_inout || !out_result)
        return HU_ERR_INVALID_ARGUMENT;

    /* AC-7: reversible kill switch. */
    if (!cfg->enabled) {
        hu_log_info_once(&g_warned_disabled, "init_proposer", NULL,
                         "initiative subsystem disabled by config "
                         "(cfg->initiative.enabled=false); set initiative.enabled=true "
                         "in config.json to activate");
        *out_result = HU_INIT_RESULT_SKIP;
        return HU_OK;
    }

    /* AC-6: announce activation exactly once so operators can see it's alive. */
    hu_log_info_once(&g_warned_enabled, "init_proposer", NULL,
                     "initiative subsystem activated by config "
                     "(cfg->initiative.enabled=true; tick_interval_sec=%d, threshold=%.2f, "
                     "model=%s)",
                     cfg->tick_interval_sec > 0 ? cfg->tick_interval_sec : 1800,
                     cfg->confidence_threshold > 0.0 ? cfg->confidence_threshold : 0.85,
                     (cfg->propose_model && cfg->propose_model[0]) ? cfg->propose_model
                                                                   : "gemini-3.5-flash");

    /* Interval gate (cheap — runs every outer loop). */
    int interval = cfg->tick_interval_sec > 0 ? cfg->tick_interval_sec : 1800;
    if (*last_tick_unix_inout > 0 && now_unix - *last_tick_unix_inout < interval) {
        *out_result = HU_INIT_RESULT_GATED_INTERVAL;
        return HU_OK;
    }

    /* From here on, this is a real "tick" — bump the id so the log line carries
     * a stable handle and so SKIP rate can be computed from log telemetry. */
    (*tick_id_inout)++;
    uint64_t tid = *tick_id_inout;

    /* AC-1 governor: quiet hours. */
    if (ar_cfg && hu_autoresponder_in_dnd_window(ar_cfg, now_unix, tz_offset_seconds)) {
        hu_log_info("init_proposer", NULL, "tick id=%llu phase=governor result=GATED_QUIET",
                    (unsigned long long)tid);
        *last_tick_unix_inout = now_unix;
        *out_result = HU_INIT_RESULT_GATED_QUIET;
        return HU_OK;
    }

    /* AC-3 governor: daily proactive budget. */
    if (budget && !hu_governor_has_budget(budget, (uint64_t)now_unix * 1000ULL)) {
        hu_log_info("init_proposer", NULL, "tick id=%llu phase=governor result=GATED_BUDGET",
                    (unsigned long long)tid);
        *last_tick_unix_inout = now_unix;
        *out_result = HU_INIT_RESULT_GATED_BUDGET;
        return HU_OK;
    }

    /* AC-3 governor: per-contact recency. Default 10 min if the field is 0
     * (h-uman never proposes if Seth just texted). */
    int recency_floor = cfg->per_contact_min_seconds > 0 ? cfg->per_contact_min_seconds : 600;
    if (last_inbound_unix > 0 && now_unix - last_inbound_unix < recency_floor) {
        hu_log_info("init_proposer", NULL,
                    "tick id=%llu phase=governor result=GATED_RECENCY (last_inbound=%llds_ago)",
                    (unsigned long long)tid, (long long)(now_unix - last_inbound_unix));
        *last_tick_unix_inout = now_unix;
        *out_result = HU_INIT_RESULT_GATED_RECENCY;
        return HU_OK;
    }

    /* T1 stub: every clean tick returns SKIP. T2 adds context bundle
     * assembly; T3 adds the LLM call + decision gate; T4 wires delivery. */
    hu_log_info("init_proposer", NULL,
                "tick id=%llu phase=propose result=SKIP (T1 stub; LLM call lands in T3)",
                (unsigned long long)tid);
    *last_tick_unix_inout = now_unix;
    *out_result = HU_INIT_RESULT_SKIP;
    return HU_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * T2 — Context bundle assembly + summary formatting.
 *
 * The bundle is a thin observation view over the agent's cached per-turn
 * context strings (memory, conversation, contact, etc.). It owns nothing —
 * pointers are tied to agent lifetime. The companion format function is a
 * pure predicate so the per-tick log line can be unit-tested without
 * spinning a real agent. */

/* Stable display names matching hu_init_field_t indices. Used by both
 * assemble + format, kept here so a single source of truth controls the
 * log-line schema. */
static const char *const s_field_names[HU_INIT_FIELD_COUNT] = {
    [HU_INIT_FIELD_PERSONA] = "persona",
    [HU_INIT_FIELD_CONTACT] = "contact",
    [HU_INIT_FIELD_CONVERSATION] = "conversation",
    [HU_INIT_FIELD_MEMORY] = "memory",
    [HU_INIT_FIELD_PERSONAL_MODEL] = "personal_model",
    [HU_INIT_FIELD_AWARENESS] = "awareness",
    [HU_INIT_FIELD_INSTRUCTION] = "instruction",
    [HU_INIT_FIELD_STM] = "stm",
};

hu_error_t hu_init_proposer_assemble_context(const struct hu_agent *agent, int64_t now_unix,
                                             int64_t last_inbound_unix,
                                             hu_init_context_bundle_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->now_unix = now_unix;
    out->last_inbound_unix = last_inbound_unix;

    if (!agent)
        return HU_OK; /* empty bundle is valid — caller (T3) decides SKIP. */

    /* Per agent.h: these cached strings are set by the daemon before
     * hu_agent_turn; lifetime is tied to the agent. We borrow pointers. */
    out->content[HU_INIT_FIELD_CONTACT] = agent->contact_context;
    out->bytes[HU_INIT_FIELD_CONTACT] = agent->contact_context_len;
    out->content[HU_INIT_FIELD_CONVERSATION] = agent->conversation_context;
    out->bytes[HU_INIT_FIELD_CONVERSATION] = agent->conversation_context_len;
    out->content[HU_INIT_FIELD_INSTRUCTION] = agent->custom_instructions;
    out->bytes[HU_INIT_FIELD_INSTRUCTION] = agent->custom_instructions_len;

    /* T2 stub: persona/memory/personal_model/awareness/stm fields land
     * when their corresponding extractor outputs are wired into a stable
     * agent-cached location. For now those slots stay zero — that's
     * meaningful telemetry by itself (the proposer can see what's
     * unwired). */

    for (size_t i = 0; i < (size_t)HU_INIT_FIELD_COUNT; i++) {
        out->total_bytes += out->bytes[i];
    }
    return HU_OK;
}

size_t hu_init_proposer_format_context_summary(const hu_init_context_bundle_t *bundle, char *out,
                                               size_t out_cap) {
    if (!out || out_cap == 0)
        return 0;
    out[0] = '\0';
    if (!bundle)
        return 0;

    /* Count populated fields (>0 bytes) for the "fields=N" leader. */
    size_t populated = 0;
    for (size_t i = 0; i < (size_t)HU_INIT_FIELD_COUNT; i++) {
        if (bundle->bytes[i] > 0)
            populated++;
    }

    int written = snprintf(out, out_cap, "fields=%zu total=%zu", populated, bundle->total_bytes);
    if (written < 0)
        return 0;
    size_t pos = (size_t)written < out_cap ? (size_t)written : out_cap - 1;

    for (size_t i = 0; i < (size_t)HU_INIT_FIELD_COUNT && pos + 1 < out_cap; i++) {
        int n = snprintf(out + pos, out_cap - pos, " %s=%zu", s_field_names[i], bundle->bytes[i]);
        if (n < 0)
            break;
        if ((size_t)n >= out_cap - pos) {
            pos = out_cap - 1;
            break;
        }
        pos += (size_t)n;
    }
    out[pos] = '\0';
    return pos;
}
