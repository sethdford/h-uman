#ifndef SC_SCRUB_H
#define SC_SCRUB_H

#include "seaclaw/core/allocator.h"
#include <stddef.h>

/* Scrub secret-like patterns from text (sk-, ghp_, Bearer tokens, api_key=VALUE, etc).
 * Returns owned string. */
char *sc_scrub_secret_patterns(sc_allocator_t *alloc, const char *input, size_t input_len);

/* Sanitize API error: scrub secrets and truncate to ~200 chars. */
char *sc_scrub_sanitize_api_error(sc_allocator_t *alloc, const char *input, size_t input_len);

#endif /* SC_SCRUB_H */
