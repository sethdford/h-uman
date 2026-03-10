#ifndef HU_AGENT_PREFERENCES_H
#define HU_AGENT_PREFERENCES_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stdbool.h>
#include <stddef.h>

#define HU_PREF_KEY_PREFIX     "_pref:"
#define HU_PREF_KEY_PREFIX_LEN 6
#define HU_PREF_MAX_LOAD       20

/* Detect if a user message is a correction/preference statement.
 * prev_role should be HU_ROLE_ASSISTANT for correction detection. */
bool hu_preferences_is_correction(const char *message, size_t message_len);

/* Extract a preference rule from a correction message.
 * Returns a heap-allocated string the caller must free. */
char *hu_preferences_extract(hu_allocator_t *alloc, const char *user_msg, size_t user_msg_len,
                             size_t *out_len);

/* Store a preference in the memory backend. */
hu_error_t hu_preferences_store(hu_memory_t *memory, hu_allocator_t *alloc, const char *preference,
                                size_t preference_len);

/* Load all stored preferences into a single formatted string for prompt injection.
 * Caller owns the returned string. */
hu_error_t hu_preferences_load(hu_memory_t *memory, hu_allocator_t *alloc, char **out,
                               size_t *out_len);

#endif /* HU_AGENT_PREFERENCES_H */
