#ifndef SC_SLICE_H
#define SC_SLICE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef struct sc_bytes {
    const uint8_t *ptr;
    size_t len;
} sc_bytes_t;

typedef struct sc_bytes_mut {
    uint8_t *ptr;
    size_t len;
} sc_bytes_mut_t;

typedef struct sc_str {
    const char *ptr;
    size_t len;
} sc_str_t;

#define SC_STR_LIT(s) ((sc_str_t){ .ptr = (s), .len = sizeof(s) - 1 })
#define SC_STR_NULL    ((sc_str_t){ .ptr = NULL, .len = 0 })
#define SC_BYTES_NULL  ((sc_bytes_t){ .ptr = NULL, .len = 0 })

static inline sc_str_t sc_str_from_cstr(const char *s) {
    return (sc_str_t){ .ptr = s, .len = s ? strlen(s) : 0 };
}

static inline bool sc_str_is_empty(sc_str_t s) {
    return s.len == 0 || s.ptr == NULL;
}

static inline bool sc_str_eq(sc_str_t a, sc_str_t b) {
    if (a.len != b.len) return false;
    if (a.len == 0) return true;
    if (!a.ptr || !b.ptr) return false;
    return memcmp(a.ptr, b.ptr, a.len) == 0;
}

static inline bool sc_str_eq_cstr(sc_str_t a, const char *b) {
    return sc_str_eq(a, sc_str_from_cstr(b));
}

static inline bool sc_str_starts_with(sc_str_t s, sc_str_t prefix) {
    if (prefix.len == 0) return true;
    if (prefix.len > s.len || !s.ptr || !prefix.ptr) return false;
    return memcmp(s.ptr, prefix.ptr, prefix.len) == 0;
}

static inline bool sc_str_ends_with(sc_str_t s, sc_str_t suffix) {
    if (suffix.len == 0) return true;
    if (suffix.len > s.len || !s.ptr || !suffix.ptr) return false;
    return memcmp(s.ptr + s.len - suffix.len, suffix.ptr, suffix.len) == 0;
}

static inline sc_str_t sc_str_trim(sc_str_t s) {
    if (!s.ptr || s.len == 0) return (sc_str_t){ .ptr = s.ptr, .len = 0 };
    while (s.len > 0 && (s.ptr[0] == ' ' || s.ptr[0] == '\t' ||
           s.ptr[0] == '\n' || s.ptr[0] == '\r')) {
        s.ptr++;
        s.len--;
    }
    while (s.len > 0 && (s.ptr[s.len - 1] == ' ' || s.ptr[s.len - 1] == '\t' ||
           s.ptr[s.len - 1] == '\n' || s.ptr[s.len - 1] == '\r')) {
        s.len--;
    }
    return s;
}

static inline sc_bytes_t sc_bytes_from(const uint8_t *p, size_t len) {
    return (sc_bytes_t){ .ptr = p, .len = len };
}

#endif
