#ifndef HU_STRING_H
#define HU_STRING_H

#include "allocator.h"
#include "error.h"
#include "slice.h"
#include <stdarg.h>

char *hu_strdup(hu_allocator_t *alloc, const char *s);
char *hu_strndup(hu_allocator_t *alloc, const char *s, size_t n);
char *hu_str_dup(hu_allocator_t *alloc, hu_str_t s);
char *hu_str_concat(hu_allocator_t *alloc, hu_str_t a, hu_str_t b);
char *hu_str_join(hu_allocator_t *alloc, const hu_str_t *parts, size_t count, hu_str_t sep);
char *hu_sprintf(hu_allocator_t *alloc, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void hu_str_free(hu_allocator_t *alloc, char *s);

bool hu_str_contains(hu_str_t haystack, hu_str_t needle);
int hu_str_index_of(hu_str_t haystack, hu_str_t needle);

/* Portable case-insensitive substring search (like GNU strcasestr). */
char *hu_strcasestr(const char *haystack, const char *needle);

/* Bounded buffer append — returns new offset, clamped to cap on truncation.
   Prevents the pos+=snprintf overflow pattern. */
size_t hu_buf_appendf(char *buf, size_t cap, size_t off, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* SQL single-quote escape — doubles each `'` and copies the rest verbatim, a la
 * SQLite's `'` quoting (`O'Brien` -> `O''Brien`). Writes a null-terminated
 * string into `dst` and stores the byte count (excluding the null) into
 * `*out_len`. Truncates silently when `dst_cap` is exhausted; *out_len always
 * matches what was actually written. Returns HU_ERR_INVALID_ARGUMENT if any
 * pointer is NULL or `dst_cap == 0`.
 *
 * This is the canonical implementation. ~18 modules in src/ each shipped a
 * private static copy of this function with two slightly different
 * signatures; see docs/dedupe-debt.md for the migration plan. */
hu_error_t hu_sql_quote_escape_into(const char *src, size_t src_len, char *dst, size_t dst_cap,
                                    size_t *out_len);

#endif
