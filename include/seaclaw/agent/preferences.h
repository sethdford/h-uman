#ifndef SC_AGENT_PREFERENCES_H
#define SC_AGENT_PREFERENCES_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"
#include <stdbool.h>
#include <stddef.h>

#define SC_PREF_KEY_PREFIX "_pref:"
#define SC_PREF_KEY_PREFIX_LEN 6
#define SC_PREF_MAX_LOAD 20

/* Detect if a user message is a correction/preference statement.
 * prev_role should be SC_ROLE_ASSISTANT for correction detection. */
bool sc_preferences_is_correction(const char *message, size_t message_len);

/* Extract a preference rule from a correction message.
 * Returns a heap-allocated string the caller must free. */
char *sc_preferences_extract(sc_allocator_t *alloc, const char *user_msg, size_t user_msg_len,
                             size_t *out_len);

/* Store a preference in the memory backend. */
sc_error_t sc_preferences_store(sc_memory_t *memory, sc_allocator_t *alloc, const char *preference,
                                size_t preference_len);

/* Load all stored preferences into a single formatted string for prompt injection.
 * Caller owns the returned string. */
sc_error_t sc_preferences_load(sc_memory_t *memory, sc_allocator_t *alloc, char **out,
                               size_t *out_len);

#endif /* SC_AGENT_PREFERENCES_H */
