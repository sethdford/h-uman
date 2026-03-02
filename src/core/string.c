#include "seaclaw/core/string.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

char *sc_strdup(sc_allocator_t *alloc, const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *dup = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!dup) return NULL;
    memcpy(dup, s, len + 1);
    return dup;
}

char *sc_strndup(sc_allocator_t *alloc, const char *s, size_t n) {
    if (!s) return NULL;
    const char *end = (const char *)memchr(s, '\0', n);
    size_t len = end ? (size_t)(end - s) : n;
    char *dup = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!dup) return NULL;
    memcpy(dup, s, len);
    dup[len] = '\0';
    return dup;
}

char *sc_str_dup(sc_allocator_t *alloc, sc_str_t s) {
    if (!s.ptr || s.len == 0) {
        char *empty = (char *)alloc->alloc(alloc->ctx, 1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    char *dup = (char *)alloc->alloc(alloc->ctx, s.len + 1);
    if (!dup) return NULL;
    memcpy(dup, s.ptr, s.len);
    dup[s.len] = '\0';
    return dup;
}

char *sc_str_concat(sc_allocator_t *alloc, sc_str_t a, sc_str_t b) {
    size_t total = a.len + b.len;
    char *out = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!out) return NULL;
    if (a.ptr && a.len) memcpy(out, a.ptr, a.len);
    if (b.ptr && b.len) memcpy(out + a.len, b.ptr, b.len);
    out[total] = '\0';
    return out;
}

char *sc_str_join(sc_allocator_t *alloc, const sc_str_t *parts, size_t count, sc_str_t sep) {
    if (count == 0) {
        char *empty = (char *)alloc->alloc(alloc->ctx, 1);
        if (empty) empty[0] = '\0';
        return empty;
    }

    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        total += parts[i].len;
        if (i + 1 < count) total += sep.len;
    }

    char *out = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!out) return NULL;

    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        if (parts[i].ptr && parts[i].len) {
            memcpy(out + pos, parts[i].ptr, parts[i].len);
            pos += parts[i].len;
        }
        if (i + 1 < count && sep.ptr && sep.len) {
            memcpy(out + pos, sep.ptr, sep.len);
            pos += sep.len;
        }
    }
    out[total] = '\0';
    return out;
}

char *sc_sprintf(sc_allocator_t *alloc, const char *fmt, ...) {
    va_list args, args2;
    va_start(args, fmt);
    va_copy(args2, args);

    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (needed < 0) { va_end(args2); return NULL; }

    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)needed + 1);
    if (!buf) { va_end(args2); return NULL; }

    vsnprintf(buf, (size_t)needed + 1, fmt, args2);
    va_end(args2);
    return buf;
}

void sc_str_free(sc_allocator_t *alloc, char *s) {
    if (s) alloc->free(alloc->ctx, s, strlen(s) + 1);
}

bool sc_str_contains(sc_str_t haystack, sc_str_t needle) {
    return sc_str_index_of(haystack, needle) >= 0;
}

int sc_str_index_of(sc_str_t haystack, sc_str_t needle) {
    if (!haystack.ptr || !needle.ptr) return -1;
    if (needle.len == 0) return 0;
    if (needle.len > haystack.len) return -1;

    for (size_t i = 0; i <= haystack.len - needle.len; i++) {
        if (memcmp(haystack.ptr + i, needle.ptr, needle.len) == 0)
            return (int)i;
    }
    return -1;
}
