#ifndef HU_BEHAVIOR_AFFECT_H
#define HU_BEHAVIOR_AFFECT_H

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* B3: Continuous valence/arousal/dominance affect state.
 *
 * Replaces keyword-only emotional routing in src/agent/model_router.c with
 * a small dimensional model. Lexicon-driven baseline; B12 will add audio
 * and video producers behind the same `hu_affect_state_t`.
 *
 * Coexists with `hu_affect_mirror_*` in include/human/persona.h, which
 * shape mirroring intensity. This type carries the underlying state that
 * those mirror helpers can read.
 */

typedef enum hu_affect_modality {
    HU_AFFECT_TEXT = 0,
    HU_AFFECT_AUDIO,
    HU_AFFECT_VIDEO,
    HU_AFFECT_FUSED
} hu_affect_modality_t;

typedef struct hu_affect_state {
    float valence;     /* -1..1 (negative..positive) */
    float arousal;     /*  0..1 (calm..energized)    */
    float dominance;   /*  0..1 (submissive..in control) */
    float uncertainty; /*  0..1 (high = use cautiously) */
    hu_affect_modality_t modality;
    uint64_t ts;       /* unix seconds */
} hu_affect_state_t;

void hu_affect_init(hu_affect_state_t *s);

/* Estimate VAD from a single text utterance.
 * `len == 0` → returns neutral state with high uncertainty.
 */
hu_error_t hu_affect_estimate_text(const char *text, size_t len, hu_affect_state_t *out);

/* Decay arousal and absolute valence toward zero with the given half life. */
hu_error_t hu_affect_decay(hu_affect_state_t *s, uint64_t now_ts, float half_life_s);

/* Uncertainty-weighted fuse: lower-uncertainty term dominates. */
hu_error_t hu_affect_fuse(const hu_affect_state_t *prior, const hu_affect_state_t *update,
                          hu_affect_state_t *out);

/* Map state to a model-router-friendly score.
 * Higher score recommends higher-tier (more deliberative) model:
 *   distress (low valence + high arousal) → escalate
 *   neutral chat → low score
 *   playful (positive valence, mid arousal) → low/mid score
 */
int hu_affect_route_tier_score(const hu_affect_state_t *s);

/* True when the user appears emotionally vulnerable / distressed enough
 * that the assistant should validate before answering. */
bool hu_affect_is_distress(const hu_affect_state_t *s);

#endif /* HU_BEHAVIOR_AFFECT_H */
