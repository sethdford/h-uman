#ifndef HU_MEMORY_OPINIONS_H
#define HU_MEMORY_OPINIONS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The pure opinion-challenge detector (roadmap #14) lives in
 * human/memory/opinion_challenge.h — it has no hu_opinion_t dependency and
 * must stay includable alongside human/memory/cognitive.h, whose legacy F65
 * hu_opinion_t definition collides with the one below. */

#ifdef HU_ENABLE_SQLITE

typedef struct hu_opinion {
    int64_t id;
    char *topic;
    size_t topic_len;
    char *position;
    size_t position_len;
    float confidence;
    int64_t first_expressed;
    int64_t last_expressed;
    int64_t superseded_by; /* 0 or id of newer opinion */
} hu_opinion_t;

/* Store or update opinion. If same position → update last_expressed, confidence.
   If different position → supersede old (set superseded_by = new_id), insert new. */
hu_error_t hu_opinions_upsert(hu_allocator_t *alloc, hu_memory_t *memory, const char *topic,
                              size_t topic_len, const char *position, size_t position_len,
                              float confidence, int64_t now_ts);

/* Get current (non-superseded) opinions for topic. */
hu_error_t hu_opinions_get(hu_allocator_t *alloc, hu_memory_t *memory, const char *topic,
                           size_t topic_len, hu_opinion_t **out, size_t *out_count);

/* Get superseded opinions for "I used to think X" references. */
hu_error_t hu_opinions_get_superseded(hu_allocator_t *alloc, hu_memory_t *memory, const char *topic,
                                      size_t topic_len, hu_opinion_t **out, size_t *out_count);

/* Check if topic matches any core value (case-insensitive). */
bool hu_opinions_is_core_value(const char *topic, size_t topic_len, const char *const *core_values,
                               size_t count);

/* Free array of opinions. */
void hu_opinions_free(hu_allocator_t *alloc, hu_opinion_t *ops, size_t count);

#endif /* HU_ENABLE_SQLITE */

#endif /* HU_MEMORY_OPINIONS_H */
