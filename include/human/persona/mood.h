#ifndef HU_PERSONA_MOOD_H
#define HU_PERSONA_MOOD_H

/* Mood functionality requires SQLite (HU_ENABLE_SQLITE). Without it,
 * hu_mood_get_current and hu_mood_set return HU_ERR_NOT_SUPPORTED. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stddef.h>
#include <stdint.h>

typedef enum hu_mood_enum {
    HU_MOOD_NEUTRAL,
    HU_MOOD_HAPPY,
    HU_MOOD_STRESSED,
    HU_MOOD_TIRED,
    HU_MOOD_ENERGIZED,
    HU_MOOD_IRRITABLE,
    HU_MOOD_CONTEMPLATIVE,
    HU_MOOD_EXCITED,
    HU_MOOD_SAD,
    HU_MOOD_COUNT
} hu_mood_enum_t;

typedef struct hu_mood_state {
    hu_mood_enum_t mood;
    float intensity;  /* 0-1 */
    char cause[128];  /* optional */
    float decay_rate; /* per hour toward neutral */
    int64_t set_at;
} hu_mood_state_t;

/* Pure name<->enum mapping. Returns "neutral" for out-of-range. Lives in the
 * domain module (src/persona/mood.c); the persistence repo consumes it so the
 * MOOD_NAMES table is defined once. */
const char *hu_mood_name(hu_mood_enum_t mood);

/* Get current mood (decayed). In-memory cache + fallback to mood_log. */
hu_error_t hu_mood_get_current(hu_allocator_t *alloc, hu_memory_t *memory, hu_mood_state_t *out);

/* Set mood. Logs to mood_log. */
hu_error_t hu_mood_set(hu_allocator_t *alloc, hu_memory_t *memory, hu_mood_enum_t mood,
                       float intensity, const char *cause, size_t cause_len);

/* Build prompt directive. Returns NULL if intensity < 0.2. */
char *hu_mood_build_directive(hu_allocator_t *alloc, const hu_mood_state_t *state, size_t *out_len);

#endif /* HU_PERSONA_MOOD_H */
