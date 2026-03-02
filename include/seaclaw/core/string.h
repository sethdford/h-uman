#ifndef SC_STRING_H
#define SC_STRING_H

#include "allocator.h"
#include "slice.h"
#include "error.h"
#include <stdarg.h>

char *sc_strdup(sc_allocator_t *alloc, const char *s);
char *sc_strndup(sc_allocator_t *alloc, const char *s, size_t n);
char *sc_str_dup(sc_allocator_t *alloc, sc_str_t s);
char *sc_str_concat(sc_allocator_t *alloc, sc_str_t a, sc_str_t b);
char *sc_str_join(sc_allocator_t *alloc, const sc_str_t *parts, size_t count, sc_str_t sep);
char *sc_sprintf(sc_allocator_t *alloc, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void sc_str_free(sc_allocator_t *alloc, char *s);

bool sc_str_contains(sc_str_t haystack, sc_str_t needle);
int sc_str_index_of(sc_str_t haystack, sc_str_t needle);

#endif
