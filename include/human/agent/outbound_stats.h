#ifndef HU_AGENT_OUTBOUND_STATS_H
#define HU_AGENT_OUTBOUND_STATS_H

/* Sprint 60 — outbound pipeline stats (item #5 of sprint-59 STATUS.md).
 *
 * The pipeline runner at src/agent/outbound/pipeline.c emits a
 * structured INFO log per stage invocation but does NOT aggregate
 * verdict counts. Operators want to know "how often does the
 * crosstalk stage REJECT?" without grepping JSON-shaped log lines.
 *
 * This subsystem keeps per-stage × per-verdict counters in atomic
 * static storage. The pipeline calls hu_outbound_stats_record after
 * each stage runs; the doctor check at
 * src/doctor/check_outbound_stats.c snapshots them for `human doctor`
 * output.
 *
 * Process-wide, not per-pipeline. Counts are cumulative across the
 * daemon's lifetime — there is no per-tick rollover (operators can
 * diff snapshots themselves if they want rate).
 *
 * Why a stable integer boundary type (int *)? Same rationale as
 * include/human/agent/burst_egress.h: keeps callers decoupled from
 * outbound_pipeline.h to sidestep the `struct hu_outbound_stage` vs
 * `enum hu_outbound_stage` (channel.h) tag collision. The .c uses the
 * real enum internally.
 */

#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stage IDs — order is stable wire-level: doctor JSON output uses
 * these indices to layout the per-stage counts array. Adding a new
 * stage MUST append, not reorder. */
typedef enum hu_outbound_stats_stage {
    HU_OUTBOUND_STATS_STAGE_STRIP = 0,
    HU_OUTBOUND_STATS_STAGE_SHAPE,
    HU_OUTBOUND_STATS_STAGE_ECHO,
    HU_OUTBOUND_STATS_STAGE_CROSSTALK,
    HU_OUTBOUND_STATS_STAGE_PERSONA,
    HU_OUTBOUND_STATS_STAGE_MODERATION,
    HU_OUTBOUND_STATS_STAGE_OTHER, /* fallback for unknown stage names */
    HU_OUTBOUND_STATS_STAGE_COUNT
} hu_outbound_stats_stage_t;

/* Verdict kinds — must match the integer values of
 * hu_outbound_verdict_kind_t (SEND=0, REWRITE=1, REGENERATE=2,
 * REJECT=3). The pipeline passes its enum value as int through
 * hu_outbound_stats_record; we don't include outbound_pipeline.h to
 * avoid the tag collision. */
#define HU_OUTBOUND_STATS_VERDICT_COUNT 4

/* Snapshot of the counter table. Caller-allocated; populated by
 * hu_outbound_stats_snapshot. Layout: counts[stage][verdict_kind].
 * Total cells: HU_OUTBOUND_STATS_STAGE_COUNT * 4 = 28 (one cache
 * line) for the foreseeable future. */
typedef struct hu_outbound_stats_snapshot {
    uint64_t counts[HU_OUTBOUND_STATS_STAGE_COUNT][HU_OUTBOUND_STATS_VERDICT_COUNT];
} hu_outbound_stats_snapshot_t;

/* Map a stage name string ("strip", "shape", "echo", "crosstalk",
 * "persona", "moderation") to the enum id. NULL or unknown names
 * map to HU_OUTBOUND_STATS_STAGE_OTHER (fallback bucket so the
 * pipeline never silently drops a count). Case-sensitive — stage
 * names in pipeline_configs.c are already lowercase. */
hu_outbound_stats_stage_t hu_outbound_stats_stage_from_name(const char *stage_name);

/* Stable string for a stage id, useful for doctor JSON output.
 * Returns "other" / "?" for unknown ids. NEVER returns NULL. */
const char *hu_outbound_stats_stage_name(hu_outbound_stats_stage_t stage);

/* Increment the (stage, verdict_kind) counter. Thread-safe; uses
 * atomic increment with relaxed ordering (counts are monotonic
 * and operators read approximate values).
 *
 * verdict_kind values are the int representation of
 * hu_outbound_verdict_kind_t (SEND=0..REJECT=3). Out-of-range
 * values are clamped to OTHER + last bucket. */
void hu_outbound_stats_record(const char *stage_name, int verdict_kind);

/* Read the current counter table into the caller's snapshot. The
 * read is lock-free per-cell; the snapshot may not be globally
 * consistent across cells if records fire during the read, but
 * each cell's value reflects a real atomic load. */
hu_error_t hu_outbound_stats_snapshot(hu_outbound_stats_snapshot_t *out);

/* Reset all counters to zero. Test-only — production has no
 * reset path. */
void hu_outbound_stats_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_OUTBOUND_STATS_H */
