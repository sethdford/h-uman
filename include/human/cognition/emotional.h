#ifndef HU_COGNITION_EMOTIONAL_H
#define HU_COGNITION_EMOTIONAL_H

#include "human/context/conversation.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/stm.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bitmask: which detectors contributed to the fused emotional state */
typedef enum hu_emotion_source {
    HU_EMOTION_SRC_FAST_CAPTURE = 1 << 0,
    HU_EMOTION_SRC_CONVERSATION = 1 << 1,
    HU_EMOTION_SRC_STM_HISTORY = 1 << 2,
    HU_EMOTION_SRC_EGRAPH = 1 << 3,
    HU_EMOTION_SRC_VOICE = 1 << 4,
} hu_emotion_source_t;

/* Number of distinct HU_EMOTION_SRC_* bits (used when counting sources in fusion). */
#define HU_EMOTION_SRC_BIT_COUNT 5

#define HU_EMOTIONAL_TRAJECTORY_LEN 8

/* Fused per-turn emotional cognition produced by hu_emotional_cognition_perceive */
typedef struct hu_emotional_cognition {
    hu_emotional_state_t state;
    uint8_t source_mask;
    float confidence;       /* 0.0–1.0; higher when multiple sources agree */
    float trajectory_slope; /* linear trend over recent turns; >0 = improving */
    hu_emotion_tag_t secondary_emotion;
    bool needs_empathy_boost; /* intensity > 0.6 && concerning */
    bool escalation_detected;

    /* ring buffer of recent valences for trajectory computation */
    float valence_history[HU_EMOTIONAL_TRAJECTORY_LEN];
    size_t valence_count;
    size_t valence_idx;
} hu_emotional_cognition_t;

/* Input signals collected before fusion */
typedef struct hu_emotional_perception {
    const hu_emotional_state_t *fast_capture; /* from hu_fast_capture; NULL if unavailable */
    const hu_emotional_state_t *conversation; /* from hu_conversation_detect_emotion; NULL ok */
    const hu_stm_emotion_t *stm_emotions;     /* recent STM emotion tags */
    size_t stm_emotion_count;
    hu_emotion_tag_t egraph_dominant; /* from hu_egraph_query; HU_EMOTION_NEUTRAL if none */
    float egraph_intensity;
    float voice_valence; /* from voice channel metadata; NAN if unavailable */
} hu_emotional_perception_t;

/* Initialize emotional cognition state (zeroes everything). */
void hu_emotional_cognition_init(hu_emotional_cognition_t *ec);

/* Fuse perception signals into the cognition state. Safe to call every turn. */
void hu_emotional_cognition_perceive(hu_emotional_cognition_t *ec,
                                     const hu_emotional_perception_t *perception);

/* Build a markdown prompt block from the current emotional cognition.
 * Returns HU_OK with *out=NULL if no meaningful emotional content.
 * Caller owns returned string. */
hu_error_t hu_emotional_cognition_build_prompt(hu_allocator_t *alloc,
                                               const hu_emotional_cognition_t *ec, char **out,
                                               size_t *out_len);

/* Append current valence to trajectory ring buffer (called post-response). */
void hu_emotional_cognition_update_trajectory(hu_emotional_cognition_t *ec, float valence);

/* Sprint 6 US-17: Emotional contagion.
 * Apply a fraction of partner_emotion's valence/arousal signal to Seth's
 * hu_emotional_cognition_t. contagion_fraction controls how much of the
 * partner's mood bleeds into Seth's state (default 0.3 if <= 0).
 *
 * Valence mapping: sadness/grief -> -0.7, anger -> -0.5, fear -> -0.5,
 *                  joy -> +0.7, surprise -> +0.3, neutral/other -> 0.
 * Mutates self->state.valence and self->state.intensity (arousal proxy).
 * Returns HU_ERR_INVALID_ARGUMENT if self is NULL. */
hu_error_t hu_emotional_apply_contagion(hu_emotional_cognition_t *self,
                                        hu_emotion_tag_t partner_emotion, float partner_intensity,
                                        float contagion_fraction);

#endif /* HU_COGNITION_EMOTIONAL_H */
