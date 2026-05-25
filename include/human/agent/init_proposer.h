#ifndef HU_AGENT_INIT_PROPOSER_H
#define HU_AGENT_INIT_PROPOSER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdint.h>

/* Initiative Layer — proposer subsystem (T1 skeleton).
 *
 * Per docs/plans/2026-05-25-initiative-layer/. Periodically asks
 * "given everything I know about Seth's life right now, should I
 * bring something up?" — even when no inbound event fired.
 *
 * T1 scope: skeleton tick + governor pre-checks ONLY. Always returns
 * SKIP. LLM call lands in T3.
 *
 * Design decisions (resolved 2026-05-25):
 *   - Cadence: 30 min during awake hours
 *   - Confidence gate: hard threshold 0.85
 *   - Model tier: gemini-3.5-flash (conversational)
 *   - Awake source: autoresponder.json schedules (NOT in any quiet window)
 */

/* Forward declarations to avoid pulling the entire daemon graph into the
 * public header. Definitions live in config.h / agent.h / governor.h. */
struct hu_initiative_config;
struct hu_autoresponder_config;
struct hu_proactive_budget;

/* Single per-tick outcome the daemon logs at INFO level. */
typedef enum hu_init_proposer_result {
    HU_INIT_RESULT_SKIP = 0,           /* tick fired, no proposal warranted */
    HU_INIT_RESULT_GATED_QUIET = 1,    /* in autoresponder quiet hours */
    HU_INIT_RESULT_GATED_BUDGET = 2,   /* daily proactive budget exhausted */
    HU_INIT_RESULT_GATED_RECENCY = 3,  /* Seth texted h-uman recently (<per_contact_min_seconds) */
    HU_INIT_RESULT_GATED_INTERVAL = 4, /* not enough time since last tick */
    HU_INIT_RESULT_FIRED = 5,          /* T3+: actually fired a proposal */
} hu_init_proposer_result_t;

/* Tick entry point.
 *
 * Called once per daemon outer loop. Internally rate-limits to
 * cfg->tick_interval_sec (default 1800). When gated, emits a single log
 * line naming the dominant reason; when SKIP, emits the same shape so
 * operators can see the system is alive.
 *
 * Parameters:
 *   cfg                   — initiative config; if cfg->enabled is false,
 *                           emits one-shot disabled-warning and returns OK.
 *   ar_cfg                — autoresponder config for quiet-hour gating.
 *                           If NULL, treated as "no quiet hours configured"
 *                           and the quiet check is skipped (operator
 *                           silently disabling not allowed — see AC-6).
 *   tz_offset_seconds     — local TZ offset for autoresponder window math.
 *   budget                — optional proactive budget for daily-cap check.
 *                           If NULL, the budget check is skipped (treated as
 *                           always-available; not recommended in prod).
 *   last_inbound_unix     — wall-clock seconds of the most recent inbound
 *                           message FROM the proposed recipient. 0 means
 *                           never (no recency gate).
 *   now_unix              — current wall-clock seconds.
 *   last_tick_unix_inout  — caller-owned watermark of the previous tick.
 *                           Updated to now_unix on a non-gated tick. The
 *                           interval check uses this.
 *   tick_id_inout         — monotonic tick counter; incremented per
 *                           non-gated tick. Logged.
 *   out_result            — written with the per-tick outcome (one of
 *                           HU_INIT_RESULT_*); never NULL.
 *
 * Returns HU_OK on a normal tick (including gated ticks).
 * Returns HU_ERR_INVALID_ARGUMENT on NULL required args. */
hu_error_t hu_init_proposer_tick(const struct hu_initiative_config *cfg,
                                 const struct hu_autoresponder_config *ar_cfg,
                                 int32_t tz_offset_seconds, struct hu_proactive_budget *budget,
                                 int64_t last_inbound_unix, int64_t now_unix,
                                 int64_t *last_tick_unix_inout, uint64_t *tick_id_inout,
                                 hu_init_proposer_result_t *out_result);

/* Test-only: reset the one-shot warn guards (enabled/disabled log lines)
 * so each test starts with a clean slate. No-op outside HU_IS_TEST. */
void hu_init_proposer_reset_warn_guards_for_test(void);

#endif /* HU_AGENT_INIT_PROPOSER_H */
