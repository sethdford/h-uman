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

/* Case-insensitive substring test, both sides explicitly length-bounded.
 * NULL-safe: returns false on any NULL pointer, on nlen == 0, or on
 * nlen > hlen. This is the canonical replacement for the ~20 private
 * `str_contains_ci` / `ci_contains` static copies that previously lived
 * scattered across src/ with divergent signatures and NULL-handling. */
bool hu_str_contains_ci(const char *hay, size_t hlen, const char *needle, size_t nlen);

/* Convenience wrapper for a NUL-terminated needle (computes strlen
 * internally). Same semantics as hu_str_contains_ci. NULL-safe. */
bool hu_str_contains_ci_cstr(const char *hay, size_t hlen, const char *needle);

/* Word-boundary case-insensitive match: the needle must appear bounded on
 * both sides by start/end-of-string or a non-[A-Za-z0-9] character. Both
 * args are NUL-terminated. NULL-safe (false on any NULL / empty needle).
 *
 * Use this — NOT hu_str_contains_ci — when classifying a user-configurable
 * string into mutually-exclusive buckets, so that e.g. "informal" does NOT
 * match "formal" and "lukewarm" does NOT match "warm". See
 * ~/.claude/rules/substring-classifier-pitfalls.md for the rationale and
 * the cases where naive substring matching is the correct choice instead. */
bool hu_str_contains_word_ci(const char *s, const char *needle);

/* Length-bounded variant of hu_str_contains_word_ci: the haystack is read
 * for exactly `hlen` bytes and need not be NUL-terminated (for callers that
 * pass a slice into a larger buffer). The needle is NUL-terminated. NULL-safe.
 * hu_str_contains_word_ci is the strlen(s) convenience wrapper over this. */
bool hu_str_contains_word_ci_n(const char *hay, size_t hlen, const char *needle);

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
