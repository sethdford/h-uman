#include "human/core/string.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h> /* strncasecmp */

char *hu_strdup(hu_allocator_t *alloc, const char *s) {
    if (!s)
        return NULL;
    size_t len = strlen(s);
    char *dup = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!dup)
        return NULL;
    memcpy(dup, s, len + 1);
    return dup;
}

char *hu_strndup(hu_allocator_t *alloc, const char *s, size_t n) {
    if (!s)
        return NULL;
    const char *end = (const char *)memchr(s, '\0', n);
    size_t len = end ? (size_t)(end - s) : n;
    char *dup = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!dup)
        return NULL;
    memcpy(dup, s, len);
    dup[len] = '\0';
    return dup;
}

char *hu_str_dup(hu_allocator_t *alloc, hu_str_t s) {
    if (!s.ptr || s.len == 0) {
        char *empty = (char *)alloc->alloc(alloc->ctx, 1);
        if (empty)
            empty[0] = '\0';
        return empty;
    }
    char *dup = (char *)alloc->alloc(alloc->ctx, s.len + 1);
    if (!dup)
        return NULL;
    memcpy(dup, s.ptr, s.len);
    dup[s.len] = '\0';
    return dup;
}

char *hu_str_concat(hu_allocator_t *alloc, hu_str_t a, hu_str_t b) {
    if (a.len > SIZE_MAX - b.len)
        return NULL;
    size_t total = a.len + b.len;
    char *out = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!out)
        return NULL;
    if (a.ptr && a.len)
        memcpy(out, a.ptr, a.len);
    if (b.ptr && b.len)
        memcpy(out + a.len, b.ptr, b.len);
    out[total] = '\0';
    return out;
}

char *hu_str_join(hu_allocator_t *alloc, const hu_str_t *parts, size_t count, hu_str_t sep) {
    if (count == 0) {
        char *empty = (char *)alloc->alloc(alloc->ctx, 1);
        if (empty)
            empty[0] = '\0';
        return empty;
    }

    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (parts[i].len > SIZE_MAX - total)
            return NULL;
        total += parts[i].len;
        if (i + 1 < count) {
            if (sep.len > SIZE_MAX - total)
                return NULL;
            total += sep.len;
        }
    }
    if (total > SIZE_MAX - 1)
        return NULL;

    char *out = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!out)
        return NULL;

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

char *hu_sprintf(hu_allocator_t *alloc, const char *fmt, ...) {
    va_list args, args2;
    va_start(args, fmt);
    va_copy(args2, args);

    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (needed < 0) {
        va_end(args2);
        return NULL;
    }

    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)needed + 1);
    if (!buf) {
        va_end(args2);
        return NULL;
    }

    vsnprintf(buf, (size_t)needed + 1, fmt, args2);
    va_end(args2);
    return buf;
}

void hu_str_free(hu_allocator_t *alloc, char *s) {
    /* NB: passes strlen-derived size; safe with system/tracking allocators but
       would break a strict size-matching allocator (arena, slab). */
    if (s)
        alloc->free(alloc->ctx, s, strlen(s) + 1);
}

bool hu_str_contains(hu_str_t haystack, hu_str_t needle) {
    return hu_str_index_of(haystack, needle) >= 0;
}

int hu_str_index_of(hu_str_t haystack, hu_str_t needle) {
    if (!haystack.ptr || !needle.ptr)
        return -1;
    if (needle.len == 0)
        return 0;
    if (needle.len > haystack.len)
        return -1;

    for (size_t i = 0; i <= haystack.len - needle.len; i++) {
        if (memcmp(haystack.ptr + i, needle.ptr, needle.len) == 0)
            return (int)i;
    }
    return -1;
}

char *hu_strcasestr(const char *haystack, const char *needle) {
    if (!haystack || !needle)
        return NULL;
    if (!needle[0])
        return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++;
            n++;
        }
        if (!*n)
            return (char *)haystack;
    }
    return NULL;
}

bool hu_str_contains_ci(const char *hay, size_t hlen, const char *needle, size_t nlen) {
    if (!hay || !needle || nlen == 0 || nlen > hlen)
        return false;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        while (j < nlen && tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j]))
            j++;
        if (j == nlen)
            return true;
    }
    return false;
}

bool hu_str_contains_ci_cstr(const char *hay, size_t hlen, const char *needle) {
    if (!needle)
        return false;
    return hu_str_contains_ci(hay, hlen, needle, strlen(needle));
}

bool hu_str_contains_word_ci_n(const char *hay, size_t hlen, const char *needle) {
    if (!hay || !needle || !needle[0])
        return false;
    size_t nlen = strlen(needle);
    if (hlen < nlen)
        return false;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (strncasecmp(hay + i, needle, nlen) != 0)
            continue;
        bool left_ok = (i == 0) || !isalnum((unsigned char)hay[i - 1]);
        bool right_ok = (i + nlen == hlen) || !isalnum((unsigned char)hay[i + nlen]);
        if (left_ok && right_ok)
            return true;
    }
    return false;
}

bool hu_str_contains_word_ci(const char *s, const char *needle) {
    if (!s)
        return false;
    return hu_str_contains_word_ci_n(s, strlen(s), needle);
}

size_t hu_buf_appendf(char *buf, size_t cap, size_t off, const char *fmt, ...) {
    if (off >= cap)
        return off;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, cap - off, fmt, ap);
    va_end(ap);
    if (n < 0)
        return off;
    if ((size_t)n >= cap - off)
        return cap - 1;
    return off + (size_t)n;
}

hu_error_t hu_sql_quote_escape_into(const char *src, size_t src_len, char *dst, size_t dst_cap,
                                    size_t *out_len) {
    if (!dst || dst_cap == 0 || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = 0;
    /* Reserve one byte for the null terminator; every input char may expand
     * into two output chars (a single quote becomes two). The +2 guard below
     * mirrors the pattern shipped by every duplicate static copy this
     * function replaces. */
    size_t pos = 0;
    if (src && src_len > 0) {
        for (size_t i = 0; i < src_len && pos + 2 < dst_cap; i++) {
            if (src[i] == '\'') {
                dst[pos++] = '\'';
                dst[pos++] = '\'';
            } else {
                dst[pos++] = src[i];
            }
        }
    }
    dst[pos] = '\0';
    *out_len = pos;
    return HU_OK;
}
