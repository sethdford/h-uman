#ifndef SC_PERSONA_AUTO_PROFILE_H
#define SC_PERSONA_AUTO_PROFILE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/persona.h"
#include <stddef.h>

/* Build a contact-specific overlay from their iMessage conversation stats.
 * Reads their texting patterns and returns an overlay tuned to mirror their style.
 * Caller owns returned strings in overlay (typing_quirks, style_notes).
 * Returns SC_ERR_NOT_SUPPORTED on non-macOS or when SC_IS_TEST is set. */
sc_error_t sc_persona_auto_profile(sc_allocator_t *alloc, const char *contact_id,
                                  size_t contact_id_len, sc_persona_overlay_t *overlay);

/* Build a style description string from contact stats.
 * Returns a dynamically allocated string. Caller owns. */
char *sc_persona_profile_describe_style(sc_allocator_t *alloc,
                                        const sc_sampler_contact_stats_t *stats,
                                        const char *contact_id, size_t contact_id_len,
                                        size_t *out_len);

#endif
