#ifndef HU_CONTEXT_STYLE_TRACKER_H
#define HU_CONTEXT_STYLE_TRACKER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stdbool.h>
#include <stddef.h>

/* Style fingerprint: per-contact texting style learned from the contact's
 * RECEIVED messages (update_inbound, fed by the daemon's inbound batch path)
 * so replies can mirror how each contact actually texts. The reserved
 * "__self__" row tracks the persona's own outgoing style for drift detection
 * (update_self).
 *
 * Thread safety: NOT THREAD SAFE. All functions that write to the database
 *   (update, update_inbound, update_self, drift_check) must be called from a
 *   single thread. Debug builds assert non-reentrancy on mutating functions. */
typedef struct hu_style_fingerprint {
    bool uses_lowercase;
    bool uses_periods;
    char laugh_style[32];
    int avg_message_length;
    char common_phrases[512];
    char distinctive_words[512];
} hu_style_fingerprint_t;

/* Update fingerprint from a message. Merges with existing row. */
hu_error_t hu_style_fingerprint_update(hu_memory_t *memory, hu_allocator_t *alloc,
                                       const char *contact_id, size_t contact_id_len,
                                       const char *message, size_t message_len);

/* Inbound-path entry point: update the SENDER's fingerprint from a message
 * they sent us. Rejects the reserved "__self__" contact id; an empty message
 * is a no-op (HU_OK). */
hu_error_t hu_style_fingerprint_update_inbound(hu_memory_t *memory, hu_allocator_t *alloc,
                                               const char *contact_id, size_t contact_id_len,
                                               const char *message, size_t message_len);

/* Fetch fingerprint for contact. Returns HU_OK and fills out on success. */
hu_error_t hu_style_fingerprint_get(hu_memory_t *memory, hu_allocator_t *alloc,
                                    const char *contact_id, size_t contact_id_len,
                                    hu_style_fingerprint_t *out);

/* Build directive string for LLM injection. Returns bytes written, 0 if empty. */
size_t hu_style_fingerprint_build_directive(const hu_style_fingerprint_t *fp, char *buf,
                                            size_t cap);

/* ── Self-tracking for drift detection ────────────────────────────── */

/* Drift detection result */
typedef struct hu_style_drift_result {
    double score;    /* 0.0 = identical to baseline, 1.0 = completely different */
    bool corrective; /* true if score exceeds threshold */
    char directive[256];
} hu_style_drift_result_t;

/* Track the persona's OWN outgoing message style (not contact's incoming).
 * Uses contact_id "__self__" internally. */
hu_error_t hu_style_fingerprint_update_self(hu_memory_t *memory, hu_allocator_t *alloc,
                                            const char *message, size_t message_len);

/* Compare recent self-output fingerprint against a baseline persona style.
 * Returns drift score (0.0-1.0). If drift > threshold, sets corrective=true
 * and fills directive with recalibration guidance. */
hu_error_t hu_style_drift_check(hu_memory_t *memory, hu_allocator_t *alloc,
                                const hu_style_fingerprint_t *baseline,
                                hu_style_drift_result_t *result);

/* Default drift threshold */
#define HU_STYLE_DRIFT_THRESHOLD 0.5

#endif /* HU_CONTEXT_STYLE_TRACKER_H */
