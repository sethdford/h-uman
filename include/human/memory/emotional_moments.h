#ifndef HU_EMOTIONAL_MOMENTS_H
#define HU_EMOTIONAL_MOMENTS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Emotional moment: someone shared something stressful/sad/difficult.
 * Used to schedule follow-up check-ins 1–3 days later. */

typedef struct hu_emotional_moment {
    int64_t id;
    char contact_id[128];
    char topic[256];
    char emotion[64];
    float intensity;
    int64_t created_at;
    int64_t follow_up_date;
    bool followed_up;
} hu_emotional_moment_t;

/* Record an emotional moment. Sets follow_up_date = created_at + random(1–3 days).
 * Skips if same contact_id + topic within last 7 days with followed_up=0.
 * Requires SQLite-backed memory. */
hu_error_t hu_emotional_moment_record(hu_allocator_t *alloc, hu_memory_t *memory,
                                      const char *contact_id, size_t contact_id_len,
                                      const char *topic, size_t topic_len, const char *emotion,
                                      size_t emotion_len, float intensity);

/* Get moments due for check-in (follow_up_date <= now_ts AND followed_up = 0).
 * Caller frees returned array via alloc->free. */
hu_error_t hu_emotional_moment_get_due(hu_allocator_t *alloc, hu_memory_t *memory, int64_t now_ts,
                                       hu_emotional_moment_t **out, size_t *out_count);

/* Mark a moment as followed up. */
hu_error_t hu_emotional_moment_mark_followed_up(hu_memory_t *memory, int64_t id);

/* Pure predicate: select a SAFE topic string for emotional-moment storage.
 *
 * Inputs:
 *   primary_topic    — extracted noun phrase (may be NULL or empty)
 *   emotion          — dominant emotion keyword (may be NULL or empty)
 *   out / out_cap    — destination buffer (predicate writes a NUL-terminated
 *                      lowercase token).
 *
 * Returns the length written (0 = "do not record"). The caller MUST treat a
 * 0 return as "skip the record" — the old daemon path fell back to the raw
 * user message ("combined") which has shipped first-person confessions to
 * family contacts (see docs/research/2026-05-16-proactive-audit/findings.md
 * row P2-1).
 *
 * Rules (in this order):
 *   1. If primary_topic is non-empty and non-whitespace, copy it (clipped).
 *   2. Else if emotion is non-empty, copy it.
 *   3. Else return 0 — caller must SKIP. NEVER use raw user text. */
size_t hu_emotional_moment_select_topic(const char *primary_topic, const char *emotion, char *out,
                                        size_t out_cap);

#endif /* HU_EMOTIONAL_MOMENTS_H */
