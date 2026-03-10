#ifndef HU_UTIL_H
#define HU_UTIL_H

#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * General utilities
 * ────────────────────────────────────────────────────────────────────────── */

/* Trim leading and trailing whitespace in-place. Returns new length. */
size_t hu_util_trim(char *s, size_t len);

/* Duplicate string (caller frees with exact size). Returns NULL on OOM. */
char *hu_util_strdup(void *ctx, void *(*alloc)(void *, size_t), const char *s);

/* Free string allocated with hu_util_strdup (must pass exact size). */
void hu_util_strfree(void *ctx, void (*free_fn)(void *, void *, size_t), char *s);

/* Compare strings case-insensitively. Returns 0 if equal. */
int hu_util_strcasecmp(const char *a, const char *b);

/* Generate a simple session ID (hex timestamp + random suffix). Caller frees. */
char *hu_util_gen_session_id(void *ctx, void *(*alloc)(void *, size_t));

#endif /* HU_UTIL_H */
