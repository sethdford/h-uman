/* W14 AutoDream scheduler runner (FIX 14).
 *
 * Bridges the W2 AutoDream consolidation cycle into the W14 scheduler so
 * `daemon.c` can drive idle-time housekeeping through a single uniform
 * job queue instead of three hand-rolled crons.
 *
 * `hu_autodream_runner` matches the `hu_job_runner_fn` signature; the
 * job's `kind` decides which AutoDream phases fire:
 *
 *   HU_JOB_AUTODREAM_QUARANTINE → review the quarantine table only
 *   HU_JOB_AUTODREAM_COMMUNITY  → community summaries + derived facts
 *   HU_JOB_AUTODREAM_DECAY      → Ebbinghaus edge reweighting only
 *
 * The runner is intentionally allocator-free in its argument list — the
 * scheduler signature does not carry one, so we use the system allocator
 * for autodream's own scratch buffers (matching the v1 cron path).
 *
 * The 3 AM v1 cron in `daemon.c` is unchanged in this commit so the
 * production behavior is strictly additive: any consumer who enqueues an
 * AutoDream job will run it through the scheduler, but the daily 3 AM
 * fallback still fires through the legacy path. The cron will be removed
 * in a follow-up once at least one consumer routes through the queue. */

#include "human/agent/autodream.h"
#include "human/agent/scheduler.h"
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"

#include <string.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE

hu_error_t hu_autodream_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec, int64_t budget_ms,
                               void *user_data) {
    (void)user_data;
    if (!m || !spec)
        return HU_ERR_INVALID_ARGUMENT;
    hu_graph_t *g = hu_memory_facade_graph_handle(m);
    if (!g)
        return HU_OK;

    hu_autodream_config_t cfg = hu_autodream_default_config();
    /* Compose the scheduler's per-job budget with autodream's max_runtime_ms.
     * If budget_ms > 0 we honor it (it's the tighter of the two); else stick
     * with autodream's default. */
    if (budget_ms > 0)
        cfg.max_runtime_ms = budget_ms;

    /* Phase selection: only the requested phases fire. The other flags
     * are turned OFF so a QUARANTINE-only job doesn't quietly run
     * communities + decay too. */
    cfg.enable_quarantine_review = false;
    cfg.enable_community_summaries = false;
    cfg.enable_edge_reweight = false;
    cfg.enable_derived_facts = false;
    switch (spec->kind) {
    case HU_JOB_AUTODREAM_QUARANTINE:
        cfg.enable_quarantine_review = true;
        break;
    case HU_JOB_AUTODREAM_COMMUNITY:
        cfg.enable_community_summaries = true;
        cfg.enable_derived_facts = true;
        break;
    case HU_JOB_AUTODREAM_DECAY:
        cfg.enable_edge_reweight = true;
        break;
    default:
        /* Caller registered us against a non-autodream kind; treat as
         * a no-op success rather than failing the whole tick. */
        return HU_OK;
    }

    /* If the job spec carries an explicit `earliest_at` in unix-ms we
     * forward it as autodream's `now_ms` so tests can pin the wall
     * clock. Otherwise default 0 means "ask the OS". */
    if (spec->earliest_at > 0)
        cfg.now_ms = spec->earliest_at;

    hu_autodream_report_t report;
    memset(&report, 0, sizeof(report));
    hu_allocator_t alloc = hu_system_allocator();
    return hu_autodream_run(&alloc, g, &cfg, &report);
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t hu_autodream_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec, int64_t budget_ms,
                               void *user_data) {
    (void)m;
    (void)spec;
    (void)budget_ms;
    (void)user_data;
    return HU_OK;
}

#endif /* HU_ENABLE_SQLITE */
