#ifndef HU_SCRUB_H
#define HU_SCRUB_H

#include "human/core/allocator.h"
#include <stddef.h>

/* Scrub secret-like patterns from text (sk-, ghp_, Bearer tokens, api_key=VALUE, etc).
 * Returns owned string. */
char *hu_scrub_secret_patterns(hu_allocator_t *alloc, const char *input, size_t input_len);

/* Sanitize API error: scrub secrets and truncate to ~200 chars. */
char *hu_scrub_sanitize_api_error(hu_allocator_t *alloc, const char *input, size_t input_len);

#endif /* HU_SCRUB_H */
