#ifndef HU_CONTEXT_ANTICIPATORY_H
#define HU_CONTEXT_ANTICIPATORY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stddef.h>
#include <stdint.h>

/* Emotional prediction from temporal patterns, life events, and personality. */
typedef struct hu_emotional_prediction {
    char contact_id[128];
    char predicted_emotion[64];
    float confidence;
    char basis[64];
    int64_t target_date;
} hu_emotional_prediction_t;

/* Provider-aware variant: uses local model for emotion classification when available. */
struct hu_provider;
hu_error_t hu_anticipatory_predict_with_provider(hu_allocator_t *alloc, hu_memory_t *memory,
                                                 struct hu_provider *provider, const char *model,
                                                 size_t model_len, const char *contact_id,
                                                 size_t contact_id_len, int64_t now_ts,
                                                 hu_emotional_prediction_t **out,
                                                 size_t *out_count);

/* Build directive string for prompt injection.
 * Format: "[ANTICIPATORY: [Name]'s kid has a game tomorrow — she may be nervous. Consider checking
 * in.]" Only includes predictions with confidence > 0.5. Caller owns returned string. */
char *hu_anticipatory_build_directive(hu_allocator_t *alloc, const hu_emotional_prediction_t *preds,
                                      size_t count, const char *contact_name, size_t name_len,
                                      size_t *out_len);

/* Free predictions array allocated by hu_anticipatory_predict_with_provider. */
void hu_anticipatory_predictions_free(hu_allocator_t *alloc, hu_emotional_prediction_t *preds,
                                      size_t count);

/* Model-based emotion classification from text. Uses a local provider when
 * available, falls back to keyword matching. Writes emotion/confidence into out. */
struct hu_provider;
hu_error_t hu_anticipatory_classify_emotion(hu_allocator_t *alloc, struct hu_provider *provider,
                                            const char *model, size_t model_len, const char *text,
                                            size_t text_len, char *emotion_out, size_t emotion_cap,
                                            float *confidence_out);

#endif /* HU_CONTEXT_ANTICIPATORY_H */
