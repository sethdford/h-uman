#ifndef HU_SLICE_H
#define HU_SLICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct hu_bytes {
    const uint8_t *ptr;
    size_t len;
} hu_bytes_t;

typedef struct hu_bytes_mut {
    uint8_t *ptr;
    size_t len;
} hu_bytes_mut_t;

typedef struct hu_str {
    const char *ptr;
    size_t len;
} hu_str_t;

#define HU_STR_LIT(s) ((hu_str_t){.ptr = (s), .len = sizeof(s) - 1})
#define HU_STR_NULL   ((hu_str_t){.ptr = NULL, .len = 0})
#define HU_BYTES_NULL ((hu_bytes_t){.ptr = NULL, .len = 0})

static inline hu_str_t hu_str_from_cstr(const char *s) {
    return (hu_str_t){.ptr = s, .len = s ? strlen(s) : 0};
}

static inline bool hu_str_is_empty(hu_str_t s) {
    return s.len == 0 || s.ptr == NULL;
}

static inline bool hu_str_eq(hu_str_t a, hu_str_t b) {
    if (a.len != b.len)
        return false;
    if (a.len == 0)
        return true;
    if (!a.ptr || !b.ptr)
        return false;
    return memcmp(a.ptr, b.ptr, a.len) == 0;
}

static inline bool hu_str_eq_cstr(hu_str_t a, const char *b) {
    return hu_str_eq(a, hu_str_from_cstr(b));
}

static inline bool hu_str_starts_with(hu_str_t s, hu_str_t prefix) {
    if (prefix.len == 0)
        return true;
    if (prefix.len > s.len || !s.ptr || !prefix.ptr)
        return false;
    return memcmp(s.ptr, prefix.ptr, prefix.len) == 0;
}

static inline bool hu_str_ends_with(hu_str_t s, hu_str_t suffix) {
    if (suffix.len == 0)
        return true;
    if (suffix.len > s.len || !s.ptr || !suffix.ptr)
        return false;
    return memcmp(s.ptr + s.len - suffix.len, suffix.ptr, suffix.len) == 0;
}

static inline hu_str_t hu_str_trim(hu_str_t s) {
    if (!s.ptr || s.len == 0)
        return (hu_str_t){.ptr = s.ptr, .len = 0};
    while (s.len > 0 &&
           (s.ptr[0] == ' ' || s.ptr[0] == '\t' || s.ptr[0] == '\n' || s.ptr[0] == '\r')) {
        s.ptr++;
        s.len--;
    }
    while (s.len > 0 && (s.ptr[s.len - 1] == ' ' || s.ptr[s.len - 1] == '\t' ||
                         s.ptr[s.len - 1] == '\n' || s.ptr[s.len - 1] == '\r')) {
        s.len--;
    }
    return s;
}

static inline hu_bytes_t hu_bytes_from(const uint8_t *p, size_t len) {
    return (hu_bytes_t){.ptr = p, .len = len};
}

#endif
