/*
 * secure_mem.h — Secure memory utilities
 *
 * Provides compiler-barrier-protected zero and constant-time comparison
 * for use across the security subsystem. Prefer these over raw memset/memcmp
 * when handling secrets, tokens, or HMAC digests.
 */
#ifndef HU_SECURITY_SECURE_MEM_H
#define HU_SECURITY_SECURE_MEM_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline void hu_secure_zero(void *p, size_t n) {
#if defined(__STDC_LIB_EXT1__)
    memset_s(p, n, 0, n);
#elif defined(__GNUC__) || defined(__clang__)
    memset(p, 0, n);
    __asm__ __volatile__("" : : "r"(p) : "memory");
#else
    volatile unsigned char *vp = (volatile unsigned char *)p;
    while (n--)
        *vp++ = 0;
#endif
}

static inline bool hu_constant_time_eq(const void *a, const void *b, size_t len) {
    const volatile unsigned char *xa = (const volatile unsigned char *)a;
    const volatile unsigned char *xb = (const volatile unsigned char *)b;
    volatile unsigned char diff = 0;
    for (size_t i = 0; i < len; i++)
        diff |= xa[i] ^ xb[i];
    return diff == 0;
}

#endif /* HU_SECURITY_SECURE_MEM_H */
