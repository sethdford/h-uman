#ifndef HU_DOCTOR_CHECK_OUTBOUND_STATS_H
#define HU_DOCTOR_CHECK_OUTBOUND_STATS_H

/* Sprint 60 (sprint-59 STATUS.md item #5) — doctor check that
 * exposes the outbound pipeline's per-stage × per-verdict counters
 * for `human doctor` output.
 *
 * The check is INFORMATIONAL — verdict is always HU_DOCTOR_PASS
 * (with detail_json containing the snapshot). Operators get
 * visibility into how often each stage fires REJECT / REGENERATE
 * without grepping log lines.
 *
 * Reads from the static counter table in
 * src/agent/outbound/stats.c via hu_outbound_stats_snapshot. The
 * check is process-wide; counters reset only on daemon restart.
 *
 * detail_json shape:
 *   {
 *     "stages": [
 *       {"name": "strip",      "send": N, "rewrite": N, "regenerate": N, "reject": N},
 *       {"name": "shape",      "send": N, "rewrite": N, "regenerate": N, "reject": N},
 *       ...
 *       {"name": "other",      "send": N, "rewrite": N, "regenerate": N, "reject": N}
 *     ],
 *     "total_send": N, "total_rewrite": N, "total_regenerate": N, "total_reject": N
 *   }
 */

#include "human/core/error.h"
#include "human/doctor/check.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The registered check vtable entry. Doctor registry adapters borrow
 * this; the user_data slot is NULL (the check reads from process-wide
 * static state, no per-instance context needed). */
extern const hu_doctor_check_t hu_doctor_check_outbound_stats;

/* Test seam: build the JSON detail string from a snapshot and write
 * it into the caller's buffer. Returns the number of bytes written
 * (excluding NUL terminator), or 0 on error / overflow. */
struct hu_outbound_stats_snapshot;
size_t hu_doctor_check_outbound_stats_render_json(
    const struct hu_outbound_stats_snapshot *snap, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HU_DOCTOR_CHECK_OUTBOUND_STATS_H */
