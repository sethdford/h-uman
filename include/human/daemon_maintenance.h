#ifndef HU_DAEMON_MAINTENANCE_H
#define HU_DAEMON_MAINTENANCE_H

#include "config.h"
#include "core/allocator.h"
#include "memory/consolidation.h"
#include <time.h>

/**
 * daemon_maintenance.h — Once-per-minute background maintenance ticks extracted
 * from the hu_service_run cron block in daemon.c (DDD Phase E2 daemon split,
 * chip 3 — see docs/plans/2026-05-29-ddd-bounded-contexts/phase-E2-daemon-service-lifecycle.md).
 *
 * Both entry points are PRODUCTION-ONLY and cron-build-only: they are compiled
 * out under HU_IS_TEST and without HU_HAS_CRON, mirroring the original inline
 * blocks. Callers in daemon.c remain under the same guards, so the link
 * surface is unchanged across build variants.
 */

struct hu_agent;

/** The consolidation settings the daemon uses for every hu_memory_consolidate
 *  call it makes (periodic tick and topic-switch): behavior.decay_days /
 *  behavior.dedup_threshold from config (30 / 0 when config is NULL), a fixed
 *  0.5 decay factor and 5000-entry cap, and the agent's provider + model.
 *  Unconditional (not cron/test gated) because the reactive prompt slice
 *  links against it in every build variant. */
hu_consolidation_config_t hu_daemon_consolidation_config(const hu_config_t *config,
                                                         struct hu_agent *agent);

#if defined(HU_HAS_CRON) && !defined(HU_IS_TEST)

/** Once-per-minute maintenance flush: W4 verifier-metrics snapshot (60s
 *  cadence), B3 prompt-budget snapshot (60s cadence), periodic memory
 *  consolidation (config->consolidation_interval_hours), and the Phase 8
 *  (F77-F82) scheduled reflection engine (daily 2-4 AM / weekly Sun 3 AM /
 *  monthly 1st 3 AM; HU_HAS_SKILLS builds only). Moved verbatim from
 *  daemon.c hu_service_run. */
void hu_daemon_maintenance_tick(hu_allocator_t *alloc, struct hu_agent *agent,
                                const hu_config_t *config, time_t t);

#ifdef HU_ENABLE_SQLITE
/** W14 sleep-time compute scheduler tick (once per minute): W13 outcome-bridge
 *  drain, LoRA auto-enqueue (pending-count + DPO pair-count triggers),
 *  training-data extraction cadence, nightly LoRA retrain enqueue, the
 *  scheduler tick itself, post-tick status save, and personal-model idle
 *  decay. Moved verbatim from daemon.c hu_service_run. */
void hu_daemon_learning_scheduler_tick(struct hu_agent *agent, const hu_config_t *config, time_t t);
#endif /* HU_ENABLE_SQLITE */

#endif /* HU_HAS_CRON && !HU_IS_TEST */

#endif /* HU_DAEMON_MAINTENANCE_H */
