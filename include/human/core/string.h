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

#endif
