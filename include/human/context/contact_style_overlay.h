#ifndef HU_CONTEXT_CONTACT_STYLE_OVERLAY_H
#define HU_CONTEXT_CONTACT_STYLE_OVERLAY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stddef.h>
#include <stdint.h>

/* Build a natural-language per-contact style overlay from memory.db.
 * Queries contact_identities, contact_relationships, style_fingerprints,
 * and recent emotional_moments for this contact_id.
 *
 * Returns a string like:
 *   "You're texting Dermot, a close friend. Be chatty — you two roast
 *    each other. His laugh style is 'haha', longer messages ok (avg 45 chars)."
 *
 * Returns HU_OK and sets *out on success (caller frees).
 * Returns HU_OK with *out=NULL if no data is found for this contact.
 * Gracefully skips any missing tables or empty columns. */
hu_error_t hu_contact_style_overlay_build(hu_allocator_t *alloc, hu_memory_t *memory,
                                          const char *contact_id, size_t contact_id_len,
                                          char **out, size_t *out_len);

/* Build a personal temporal mood string based on local hour (0-23).
 * Writes into caller-provided stack buffer. Returns bytes written (0 on error).
 *
 * Example: "[Temporal context] It's late night — you're winding down, reflective." */
size_t hu_temporal_mood_build(uint8_t hour, char *buf, size_t cap);

/* Build recent emotional context for a contact from emotional_moments table.
 * Returns up to max_entries recent moments as a formatted string.
 * Returns HU_OK with *out=NULL if no emotional context exists. */
hu_error_t hu_contact_emotional_context_build(hu_allocator_t *alloc, hu_memory_t *memory,
                                              const char *contact_id, size_t contact_id_len,
                                              size_t max_entries, char **out, size_t *out_len);

#endif /* HU_CONTEXT_CONTACT_STYLE_OVERLAY_H */
