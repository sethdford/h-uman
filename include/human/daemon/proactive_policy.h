#ifndef HU_DAEMON_PROACTIVE_POLICY_H
#define HU_DAEMON_PROACTIVE_POLICY_H

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Proactive check-in policy — pure leaf predicates extracted from the giant
 * hu_service_run_proactive_checkins (DDD Phase 2b characterization scaffold).
 *
 * WHY THIS MODULE EXISTS: hu_service_run_proactive_checkins is ~1,485 LOC and
 * compiled only in non-test builds (#ifndef HU_IS_TEST), so the test suite
 * cannot exercise it. Decomposing it safely requires first carving its
 * decision logic into pure, side-effect-free predicates that LIVE OUTSIDE the
 * HU_IS_TEST guard and are pinned by tests. The giant then calls these, the
 * suite verifies them directly, and each extracted leaf shrinks the giant's
 * untested surface. (security-predicate-extraction.md applied to a god-fn.)
 *
 * Each function here is a pure function of its arguments: no globals, no I/O,
 * no allocation. */

/* Proactive check-ins only fire during social hours: 09:00–21:00 local,
 * inclusive on both ends. `hour` is a tm_hour value (0–23). Returns true iff
 * the hour is within the window. Pinned: replaces the inline
 * `if (hour < 9 || hour > 21) return;` gate. */
bool hu_daemon_proactive_is_social_hour(int hour);

/* Pack a calendar date into the YYYYMMDD integer used as the per-day dedup key
 * (e.g. 2026-05-29 → 20260529). Reads tm_year (years since 1900), tm_mon
 * (0–11), and tm_mday (1–31). Returns 0 if `tm` is NULL (defensive; the
 * production call site always passes a valid struct tm). */
int hu_daemon_proactive_ymd_from_tm(const struct tm *tm);

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_PROACTIVE_POLICY_H */
