#ifndef HU_CONTEXT_EMOTIONAL_STATE_H
#define HU_CONTEXT_EMOTIONAL_STATE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stddef.h>
#include <stdint.h>

/* Cross-conversation emotional state tracking.
 *
 * Persists mood and emotional context across sessions so the agent
 * remembers how previous conversations with a contact ended.
 *
 * Data flows:
 *   End of turn  → hu_emotional_state_record    → mood_log + emotional_moments tables
 *   Start of turn → hu_emotional_state_get_recent → carry-over string for prompt
 *   Start of turn → hu_emotional_state_get_seth_mood → Seth's aggregate mood baseline
 */

/* Record the emotional tone of recent conversation text.
 * Analyzes `text` for emotional signals via keyword heuristics and writes
 * to both the `emotional_moments` and `mood_log` tables in memory.db.
 * `contact_id` scopes the record to a specific person. */
hu_error_t hu_emotional_state_record(hu_allocator_t *alloc, hu_memory_t *memory,
                                     const char *contact_id, size_t contact_id_len,
                                     const char *text, size_t text_len);

/* Load recent emotional context for a contact.
 * Returns a formatted carry-over string describing how the last conversation
 * ended and the emotional tone, including time-since-last-conversation.
 * *out is heap-allocated; caller frees via alloc->free(*out, *out_len + 1).
 * Returns HU_OK with *out=NULL when no emotional history exists. */
hu_error_t hu_emotional_state_get_recent(hu_allocator_t *alloc, hu_memory_t *memory,
                                         const char *contact_id, size_t contact_id_len,
                                         char **out, size_t *out_len);

/* Aggregate Seth's own emotional state across all recent conversations.
 * Returns a mood baseline string like "Seth's been in good spirits today"
 * or "Seth's had a rough day — 2 heavy conversations".
 * *out is heap-allocated; caller frees via alloc->free(*out, *out_len + 1).
 * Returns HU_OK with *out=NULL when no recent mood data exists. */
hu_error_t hu_emotional_state_get_seth_mood(hu_allocator_t *alloc, hu_memory_t *memory,
                                            char **out, size_t *out_len);

#endif /* HU_CONTEXT_EMOTIONAL_STATE_H */
