#ifndef HU_CONTEXT_HUMOR_H
#define HU_CONTEXT_HUMOR_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Build full humor directive from persona humor profile.
 * Returns NULL if emotion matches never_during or conversation not playful. */
char *hu_humor_build_persona_directive(hu_allocator_t *alloc, const hu_humor_profile_t *humor,
                                       const char *dominant_emotion, size_t emotion_len,
                                       bool conversation_playful, size_t *out_len);

/* ---------- Humor types ---------- */

typedef enum hu_humor_type {
    HU_HUMOR_CALLBACK = 0,
    HU_HUMOR_MISDIRECTION,
    HU_HUMOR_UNDERSTATEMENT,
    HU_HUMOR_SELF_DEPRECATION,
    HU_HUMOR_WORDPLAY,
    HU_HUMOR_OBSERVATIONAL,
    HU_HUMOR_TYPE_COUNT
} hu_humor_type_t;

const char *hu_humor_type_name(hu_humor_type_t type);
hu_humor_type_t hu_humor_type_from_name(const char *name);

/* ---------- Task 1: Audience model ---------- */

typedef struct hu_humor_audience {
    int success_count[HU_HUMOR_TYPE_COUNT];
    int attempt_count[HU_HUMOR_TYPE_COUNT];
    int64_t last_success[HU_HUMOR_TYPE_COUNT];
} hu_humor_audience_t;

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

hu_error_t hu_humor_audience_init(sqlite3 *db);
hu_error_t hu_humor_audience_record(sqlite3 *db, const char *contact_id, hu_humor_type_t type,
                                    bool success);
hu_error_t hu_humor_audience_load(sqlite3 *db, const char *contact_id, hu_humor_audience_t *out);
hu_humor_type_t hu_humor_audience_preferred_type(const hu_humor_audience_t *audience);

#endif /* HU_ENABLE_SQLITE */

/* ---------- Task 2: Timing + appropriateness ---------- */

typedef struct hu_humor_timing_result {
    bool allowed;
    char reason[128];
} hu_humor_timing_result_t;

hu_humor_timing_result_t hu_humor_check_timing(int hour, float emotional_valence,
                                               bool crisis_active, const char *relationship_stage);

bool hu_humor_check_appropriate(hu_humor_type_t type, const char *topic,
                                const char *relationship_stage);

/* ---------- Task 3: Generation strategy ---------- */

char *hu_humor_generate_strategy(hu_allocator_t *alloc, const hu_humor_audience_t *audience,
                                 const char *topic, float mood_valence,
                                 const char *relationship_stage, const hu_humor_profile_t *humor,
                                 size_t *out_len);

/* ---------- Task 4: Failed humor recovery ---------- */

bool hu_humor_detect_failure(const char *user_response, size_t response_len);

char *hu_humor_recover(hu_allocator_t *alloc, size_t *out_len);

#endif /* HU_CONTEXT_HUMOR_H */
