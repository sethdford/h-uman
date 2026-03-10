#ifndef HU_VOICE_MATURITY_H
#define HU_VOICE_MATURITY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_voice_stage {
    HU_VOICE_FORMAL,   /* new contact, professional */
    HU_VOICE_WARM,     /* established rapport */
    HU_VOICE_CANDID,   /* trusted relationship */
    HU_VOICE_INTIMATE, /* deep familiarity */
} hu_voice_stage_t;

typedef struct hu_voice_profile {
    hu_voice_stage_t stage;
    uint32_t interaction_count;
    uint32_t shared_topics;
    uint32_t emotional_exchanges;
    float warmth_score;        /* 0.0-1.0 */
    float humor_allowance;     /* 0.0-1.0 */
    float vulnerability_level; /* 0.0-1.0 */
} hu_voice_profile_t;

/* Initialize a voice profile for a new contact */
void hu_voice_profile_init(hu_voice_profile_t *profile);

/* Update profile after an interaction */
void hu_voice_profile_update(hu_voice_profile_t *profile, bool had_emotional_content,
                             bool had_shared_topic, bool had_humor);

/* Build prompt guidance for current voice maturity */
hu_error_t hu_voice_build_guidance(const hu_voice_profile_t *profile, hu_allocator_t *alloc,
                                   char **out, size_t *out_len);

/* Determine stage from interaction metrics */
hu_voice_stage_t hu_voice_compute_stage(uint32_t interactions, uint32_t emotional_exchanges,
                                        float warmth);

#endif
