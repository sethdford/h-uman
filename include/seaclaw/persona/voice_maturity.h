#ifndef SC_VOICE_MATURITY_H
#define SC_VOICE_MATURITY_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum sc_voice_stage {
    SC_VOICE_FORMAL,   /* new contact, professional */
    SC_VOICE_WARM,     /* established rapport */
    SC_VOICE_CANDID,   /* trusted relationship */
    SC_VOICE_INTIMATE, /* deep familiarity */
} sc_voice_stage_t;

typedef struct sc_voice_profile {
    sc_voice_stage_t stage;
    uint32_t interaction_count;
    uint32_t shared_topics;
    uint32_t emotional_exchanges;
    float warmth_score;        /* 0.0-1.0 */
    float humor_allowance;     /* 0.0-1.0 */
    float vulnerability_level; /* 0.0-1.0 */
} sc_voice_profile_t;

/* Initialize a voice profile for a new contact */
void sc_voice_profile_init(sc_voice_profile_t *profile);

/* Update profile after an interaction */
void sc_voice_profile_update(sc_voice_profile_t *profile, bool had_emotional_content,
                             bool had_shared_topic, bool had_humor);

/* Build prompt guidance for current voice maturity */
sc_error_t sc_voice_build_guidance(const sc_voice_profile_t *profile, sc_allocator_t *alloc,
                                   char **out, size_t *out_len);

/* Determine stage from interaction metrics */
sc_voice_stage_t sc_voice_compute_stage(uint32_t interactions, uint32_t emotional_exchanges,
                                        float warmth);

#endif
