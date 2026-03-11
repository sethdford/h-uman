#ifndef HU_CONTEXT_PROTECTIVE_H
#define HU_CONTEXT_PROTECTIVE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Check if memory content should be surfaced for contact.
 * Returns false (suppress) if: painful keywords + negative emotion or late hour,
 * or memory belongs to a different contact. */
bool hu_protective_memory_ok(hu_allocator_t *alloc, hu_memory_t *memory,
                            const char *contact_id, size_t contact_id_len,
                            const char *memory_content, size_t memory_len,
                            float emotional_valence, int hour_local);

/* Check if topic is a boundary for contact. Returns true = avoid. */
bool hu_protective_is_boundary(hu_memory_t *memory, const char *contact_id,
                               size_t contact_id_len, const char *topic,
                               size_t topic_len);

/* Premature advice guard: need 2+ venting messages before offering solutions. */
bool hu_protective_advice_ok(const char *const *messages, size_t count);

/* Add explicit boundary. */
hu_error_t hu_protective_add_boundary(hu_allocator_t *alloc, hu_memory_t *memory,
                                     const char *contact_id, size_t contact_id_len,
                                     const char *topic, size_t topic_len,
                                     const char *type, const char *source);

#endif /* HU_CONTEXT_PROTECTIVE_H */
