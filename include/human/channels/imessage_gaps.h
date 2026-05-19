/* include/human/channels/imessage_gaps.h
 *
 * Conversation-gap detection (Tier 1 #3 from docs/plans/2026-05-19-better-than-human.md).
 *
 * "You haven't messaged Bob in 3 weeks. Want to check in?" — humans notice
 * this slowly and often forget. This module walks chat.db to identify
 * previously-active contacts who have gone silent, so the proactive
 * outreach system can surface them.
 *
 * Two-layer API: a pure classifier (testable without SQLite) and a
 * SQL-backed scanner (gated by !HU_IS_TEST same as reaction_poll). */

#ifndef HU_CHANNELS_IMESSAGE_GAPS_H
#define HU_CHANNELS_IMESSAGE_GAPS_H

#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HU_IMESSAGE_GAP_HANDLE_MAX 128

typedef struct {
    char contact_handle[HU_IMESSAGE_GAP_HANDLE_MAX];
    int64_t last_message_unix; /* most recent message timestamp from chat.db */
    int64_t days_since_last;   /* (now_unix - last_message_unix) / 86400 */
    uint32_t historical_count; /* total messages exchanged historically */
} hu_imessage_stale_contact_t;

/* Pure classifier: given last message timestamp + historical-count +
 * thresholds, returns true if the contact qualifies as "stale" —
 * previously-active enough that going silent is meaningful.
 *
 * Default thresholds (recommended):
 *   min_history     = 10  (don't flag one-off contacts)
 *   min_gap_days    = 14  (two weeks of silence is the smallest signal)
 *   max_gap_days    = 365 (older than this is "dormant", not stale)
 *
 * Returns false on impossible inputs (negative timestamps, etc.). */
bool hu_imessage_gap_classify_stale(int64_t last_message_unix, int64_t now_unix,
                                    uint32_t historical_count, uint32_t min_history,
                                    int32_t min_gap_days, int32_t max_gap_days);

/* SQL-backed scanner: walk chat.db for distinct contact handles meeting
 * the staleness thresholds. Returns top-`cap` results sorted by
 * days_since_last DESC (most-stale first).
 *
 * On test/non-Apple/no-SQLite builds returns HU_ERR_NOT_SUPPORTED with
 * *out_n = 0 (mirrors hu_imessage_poll_reactions's stub pattern).
 *
 * Caller owns nothing; output struct is value-typed. */
hu_error_t hu_imessage_scan_stale_contacts(const char *db_path, int64_t now_unix,
                                           uint32_t min_history, int32_t min_gap_days,
                                           int32_t max_gap_days, hu_imessage_stale_contact_t *out,
                                           size_t cap, size_t *out_n);

#ifdef __cplusplus
}
#endif

#endif /* HU_CHANNELS_IMESSAGE_GAPS_H */
