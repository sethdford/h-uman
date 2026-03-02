#ifndef SC_UTIL_H
#define SC_UTIL_H

#include <stddef.h>
#include <stdbool.h>

/* ──────────────────────────────────────────────────────────────────────────
 * General utilities
 * ────────────────────────────────────────────────────────────────────────── */

/* Trim leading and trailing whitespace in-place. Returns new length. */
size_t sc_util_trim(char *s, size_t len);

/* Duplicate string (caller frees with exact size). Returns NULL on OOM. */
char *sc_util_strdup(void *ctx, void *(*alloc)(void *, size_t), const char *s);

/* Free string allocated with sc_util_strdup (must pass exact size). */
void sc_util_strfree(void *ctx, void (*free_fn)(void *, void *, size_t),
                    char *s);

/* Compare strings case-insensitively. Returns 0 if equal. */
int sc_util_strcasecmp(const char *a, const char *b);

/* Generate a simple session ID (hex timestamp + random suffix). Caller frees. */
char *sc_util_gen_session_id(void *ctx, void *(*alloc)(void *, size_t));

#endif /* SC_UTIL_H */
