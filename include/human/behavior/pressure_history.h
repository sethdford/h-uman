#ifndef HU_BEHAVIOR_PRESSURE_HISTORY_H
#define HU_BEHAVIOR_PRESSURE_HISTORY_H

#include "human/behavior/trust.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* B11 cross-turn pressure tracking.
 *
 * Single-message pressure detection (`hu_pressure_detect`) catches authority
 * cues, shouting, and reassertion phrasing. But sycophancy attacks are
 * frequently *cumulative* — the user reasserts the same wrong claim three or
 * four turns in a row, and a poorly-calibrated assistant will eventually
 * collapse. This module records a small fixed-size window of recent user
 * messages and the last trust action the assistant took, so callers can ask:
 *
 *   "Is the current user message a reassertion of a recent claim that I
 *    already pushed back on?"
 *
 * When the answer is yes, callers should bump `hu_trust_input_t.user_pressure_count`
 * and set `user_reasserted_after_pushback = true` before calling
 * `hu_trust_calibrate`.
 *
 * The data structure is intentionally tiny (≤ 1 KB) and deterministic — no
 * allocation, no hashing dependency, no time. Pure functions.
 *
 * Similarity is measured by trigram-Jaccard on lowercased ASCII letters and
 * digits (whitespace + punctuation collapsed). Threshold defaults to 0.6.
 */

#ifndef HU_PRESSURE_HISTORY_CAP
#define HU_PRESSURE_HISTORY_CAP 6
#endif
#ifndef HU_PRESSURE_HISTORY_MSG_BYTES
#define HU_PRESSURE_HISTORY_MSG_BYTES 192
#endif

typedef struct hu_pressure_entry {
    uint32_t turn_index;
    char normalized[HU_PRESSURE_HISTORY_MSG_BYTES]; /* normalized + truncated */
    size_t normalized_len;
    hu_trust_action_t last_action; /* what the assistant did in response */
} hu_pressure_entry_t;

typedef struct hu_pressure_history {
    hu_pressure_entry_t entries[HU_PRESSURE_HISTORY_CAP];
    size_t count;       /* total entries observed (saturates at UINT32_MAX) */
    size_t head;        /* next write slot (mod cap) */
} hu_pressure_history_t;

void hu_pressure_history_init(hu_pressure_history_t *h);

/* Append a new (user_message, last_assistant_trust_action) pair.
 * Truncates the message to `HU_PRESSURE_HISTORY_MSG_BYTES - 1` bytes after
 * normalization. NULL/empty messages are stored as empty entries. */
void hu_pressure_history_observe(hu_pressure_history_t *h, uint32_t turn_index,
                                 const char *user_message, size_t user_message_len,
                                 hu_trust_action_t last_action);

/* Inspect a candidate next-turn user message against the recorded history.
 *
 *   `*out_reasserted_after_pushback` is true when:
 *     - some recent entry is similar (Jaccard ≥ threshold), AND
 *     - that entry's `last_action` was PUSH_BACK / REFUSE_TO_AGREE.
 *
 *   `*out_reassertion_count` is the number of similar prior entries (regardless
 *     of last_action) — i.e. how many times this claim has come up already.
 *
 * Both outputs may be NULL. Returns HU_OK; never fails. */
hu_error_t hu_pressure_history_inspect(const hu_pressure_history_t *h,
                                       const char *user_message, size_t user_message_len,
                                       bool *out_reasserted_after_pushback,
                                       uint32_t *out_reassertion_count);

/* Convenience: enrich an existing trust input with the cross-turn signals.
 * Adds, never lowers. */
void hu_pressure_history_apply_to_trust_input(const hu_pressure_history_t *h,
                                              const char *user_message,
                                              size_t user_message_len,
                                              hu_trust_input_t *trust_in);

#endif /* HU_BEHAVIOR_PRESSURE_HISTORY_H */
