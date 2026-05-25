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
#include "human/agent/governor.h"
#include "human/autoresponder.h"
#include "human/config.h"
#include "human/core/log.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

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
