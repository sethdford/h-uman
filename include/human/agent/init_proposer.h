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

/* ──────────────────────────────────────────────────────────────────────────
 * T2 — Context bundle assembly (AC-2 partial)
 *
 * The proposer needs the SAME rich context the agent_turn prompt builder
 * uses, plus initiative-specific signals (recent messages, F30/F31/F129
 * affordances — when those are wired by the proactive-ext-completion plan).
 *
 * T2 ships a thin observation struct + an assembly helper. T3 will pass
 * this to the analytical-tier LLM as the "propose-or-skip" prompt input. */

/* Per-source byte counts. Indexed by HU_INIT_FIELD_*. */
typedef enum hu_init_field {
    HU_INIT_FIELD_PERSONA = 0,
    HU_INIT_FIELD_CONTACT,
    HU_INIT_FIELD_CONVERSATION,
    HU_INIT_FIELD_MEMORY,
    HU_INIT_FIELD_PERSONAL_MODEL,
    HU_INIT_FIELD_AWARENESS,
    HU_INIT_FIELD_INSTRUCTION,
    HU_INIT_FIELD_STM,
    HU_INIT_FIELD_COUNT, /* sentinel */
} hu_init_field_t;

/* Lightweight bundle: pointers + byte counts into agent-owned strings.
 * The bundle itself owns nothing — caller must not free pointers. Lifetime
 * is tied to the calling agent_turn (don't store between ticks). */
typedef struct hu_init_context_bundle {
    /* Per-source content pointers (any may be NULL if the field is empty). */
    const char *content[HU_INIT_FIELD_COUNT];
    size_t bytes[HU_INIT_FIELD_COUNT];
    size_t total_bytes;
    /* Per-tick metadata. */
    int64_t now_unix;
    int64_t last_inbound_unix; /* 0 if never */
} hu_init_context_bundle_t;

/* Forward-declared so we don't pull include/human/agent.h into this header
 * (avoids transitive dep cycles). Defined in include/human/agent.h. */
struct hu_agent;

/* Assemble the proposer's context bundle from the agent's current cached
 * context strings. Cheap — no allocation, no LLM call. Caller-owned out;
 * function memsets to zero before populating. */
hu_error_t hu_init_proposer_assemble_context(const struct hu_agent *agent, int64_t now_unix,
                                             int64_t last_inbound_unix,
                                             hu_init_context_bundle_t *out);

/* Format a one-line operator-visible summary of the bundle into a caller-
 * owned buffer:
 *
 *   "fields=N total=X persona=A contact=B conversation=C memory=D ..."
 *
 * Pure predicate over the bundle — no I/O, no allocation. Returns the
 * number of bytes written (excluding NUL). On out_cap=0, returns 0. */
size_t hu_init_proposer_format_context_summary(const hu_init_context_bundle_t *bundle, char *out,
                                               size_t out_cap);

#endif /* HU_AGENT_INIT_PROPOSER_H */
