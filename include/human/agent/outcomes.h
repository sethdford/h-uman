#ifndef HU_AGENT_OUTCOMES_H
#define HU_AGENT_OUTCOMES_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Outcome Tracker — records tool results and user corrections to enable
 * continuous learning. Feeds into persona feedback and pattern detection.
 * ────────────────────────────────────────────────────────────────────────── */

#define HU_OUTCOME_MAX_RECENT 64

typedef enum hu_outcome_type {
    HU_OUTCOME_TOOL_SUCCESS,
    HU_OUTCOME_TOOL_FAILURE,
    HU_OUTCOME_USER_CORRECTION,
    HU_OUTCOME_USER_POSITIVE, /* explicit approval or thanks */
} hu_outcome_type_t;

typedef struct hu_outcome_entry {
    hu_outcome_type_t type;
    uint64_t timestamp_ms;
    char tool_name[64];
    char summary[256];
} hu_outcome_entry_t;

typedef struct hu_outcome_tracker {
    hu_outcome_entry_t entries[HU_OUTCOME_MAX_RECENT]; /* circular buffer */
    size_t write_idx;
    size_t total;
    uint64_t tool_successes;
    uint64_t tool_failures;
    uint64_t corrections;
    uint64_t positives;
    bool auto_apply_feedback;
} hu_outcome_tracker_t;

/* Initialize an outcome tracker. */
void hu_outcome_tracker_init(hu_outcome_tracker_t *tracker, bool auto_apply_feedback);

/* Record a tool execution outcome. */
void hu_outcome_record_tool(hu_outcome_tracker_t *tracker, const char *tool_name, bool success,
                            const char *summary);

/* Record a user correction (detected from message content). */
void hu_outcome_record_correction(hu_outcome_tracker_t *tracker, const char *original,
                                  const char *correction);

/* Record positive feedback (thanks, approval). */
void hu_outcome_record_positive(hu_outcome_tracker_t *tracker, const char *context);

/* Get recent entries for pattern analysis. Returns pointer to internal buffer.
 * count is set to number of valid entries (up to HU_OUTCOME_MAX_RECENT). */
const hu_outcome_entry_t *hu_outcome_get_recent(const hu_outcome_tracker_t *tracker, size_t *count);

/* Build a summary string of outcome stats. Caller owns returned string. */
char *hu_outcome_build_summary(const hu_outcome_tracker_t *tracker, hu_allocator_t *alloc,
                               size_t *out_len);

/* Detect repeated patterns: returns true if the same tool has failed >= threshold times recently.
 */
bool hu_outcome_detect_repeated_failure(const hu_outcome_tracker_t *tracker, const char *tool_name,
                                        size_t threshold);

#endif /* HU_AGENT_OUTCOMES_H */
