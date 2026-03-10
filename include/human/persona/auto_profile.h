#ifndef HU_PERSONA_AUTO_PROFILE_H
#define HU_PERSONA_AUTO_PROFILE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona.h"
#include <stddef.h>

/* Build a contact-specific overlay from their iMessage conversation stats.
 * Reads their texting patterns and returns an overlay tuned to mirror their style.
 * Caller owns returned strings in overlay (typing_quirks, style_notes).
 * Returns HU_ERR_NOT_SUPPORTED on non-macOS or when HU_IS_TEST is set. */
hu_error_t hu_persona_auto_profile(hu_allocator_t *alloc, const char *contact_id,
                                  size_t contact_id_len, hu_persona_overlay_t *overlay);

/* Build a style description string from contact stats.
 * Returns a dynamically allocated string. Caller owns. */
char *hu_persona_profile_describe_style(hu_allocator_t *alloc,
                                        const hu_sampler_contact_stats_t *stats,
                                        const char *contact_id, size_t contact_id_len,
                                        size_t *out_len);

#endif
