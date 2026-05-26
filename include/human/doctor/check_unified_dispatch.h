/* include/human/doctor/check_unified_dispatch.h
 *
 * M3 Dispatch — doctor check for unified-dispatch health.
 *
 * Surfaces the Sprint 41 follow-up #3 retry-outcome telemetry
 * (g9_retry_rescued/thrashed/starved counters) via `human doctor` so
 * operators don't have to grep logs to know whether the unified
 * proactive dispatch is healthy.
 *
 * Verdict semantics:
 *   PASS — rescue rate ≥ 80% AND total ≥ 50 rejections. Healthy.
 *   FAIL — rescue rate < 50% AND total ≥ 50. LoRA stuck or contacts
 *          starved — needs operator action (retrain, per-channel
 *          disable, or both).
 *   NA   — every other case, with the reason string distinguishing:
 *            (a) "no data yet" (all counters zero), OR
 *            (b) "low signal" (total < 50; rate too noisy to trust), OR
 *            (c) "middling" (rate 50-80%; G9 helping but adapter
 *                partially stuck — worth watching).
 *          NA counts as PASS in aggregate; the detail JSON carries
 *          the raw counts so operators can interpret without
 *          re-running the check.
 *
 * ctx contract: no ctx needed — the counters are process-wide atomics
 * read via hu_guard_reject_stats_snapshot(). NULL ctx is accepted.
 * This is a deviation from the prompt_budget pattern; documented here
 * because doctor harnesses may pass a ctx out of consistency. The
 * runner ignores it. */
#ifndef HU_DOCTOR_CHECK_UNIFIED_DISPATCH_H
#define HU_DOCTOR_CHECK_UNIFIED_DISPATCH_H

#include "human/core/error.h"
#include "human/doctor/check.h"

/* Public vtable — registered by registry.c::register_defaults. */
extern hu_doctor_check_t hu_doctor_check_unified_dispatch;

#endif /* HU_DOCTOR_CHECK_UNIFIED_DISPATCH_H */
