#ifndef HU_RELATIONSHIP_H
#define HU_RELATIONSHIP_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_relationship_stage {
    HU_REL_NEW,      /* quality < 0.25 */
    HU_REL_FAMILIAR, /* quality 0.25-0.55 */
    HU_REL_TRUSTED,  /* quality 0.55-0.80 */
    HU_REL_DEEP,     /* quality >= 0.80 */
} hu_relationship_stage_t;

/* Per-session quality signals */
typedef struct hu_session_quality {
    float emotional_exchanges;  /* 0.0-1.0: depth of emotional sharing */
    float topic_diversity;      /* 0.0-1.0: breadth of subjects covered */
    float vulnerability_events; /* 0.0-1.0: moments of genuine openness */
    float humor_shared;         /* 0.0-1.0: shared laughter moments */
    float repair_survived;      /* 0.0-1.0: recovered from misunderstandings */
} hu_session_quality_t;

/* Quality score tracking across sessions */
typedef struct hu_relationship_quality_score {
    float cumulative_quality;  /* running weighted average */
    float recent_quality;      /* quality of last session */
    uint32_t quality_sessions; /* sessions with quality data */
} hu_relationship_quality_score_t;

typedef struct hu_relationship_state {
    hu_relationship_stage_t stage;
    uint32_t session_count;
    uint32_t total_turns;
    hu_relationship_quality_score_t quality;
} hu_relationship_state_t;

/* Compute weighted quality score from a single session's signals.
 * Weights: emotional 0.3, topic_diversity 0.2, vulnerability 0.3,
 *          humor 0.1, repair 0.1 */
float hu_session_quality_score(const hu_session_quality_t *q);

/* Legacy: session-count-only progression (still increments session_count) */
void hu_relationship_new_session(hu_relationship_state_t *state);

/* Quality-weighted session progression. Updates quality tracking and may
 * regress stage if quality drops. velocity_factor from rel_dynamics
 * accelerates/decelerates transitions (1.0 = neutral). */
void hu_relationship_new_session_quality(hu_relationship_state_t *state,
                                         const hu_session_quality_t *quality,
                                         float velocity_factor);

void hu_relationship_update(hu_relationship_state_t *state, uint32_t turn_count);
hu_error_t hu_relationship_build_prompt(hu_allocator_t *alloc, const hu_relationship_state_t *state,
                                        char **out, size_t *out_len);

#endif
