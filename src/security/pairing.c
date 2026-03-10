#include "human/core/error.h"
#include "human/crypto.h"
#include "human/security.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HU_MAX_PAIR_ATTEMPTS 5
#define HU_PAIR_LOCKOUT_SECS 600

static void hu_secure_zero(void *p, size_t n) {
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
#define HU_TOKEN_PREFIX       "zc_"
#define HU_TOKEN_PREFIX_LEN   3
#define HU_TOKEN_RANDOM_BYTES 32
#define HU_TOKEN_HEX_LEN      64
#define HU_TOKEN_TOTAL_LEN    (HU_TOKEN_PREFIX_LEN + HU_TOKEN_HEX_LEN)
#define HU_PAIRING_CODE_LEN   8

struct hu_pairing_guard {
    bool require_pairing;
    char pairing_code[HU_PAIRING_CODE_LEN + 1]; /* 8 digits + null, or empty */
    char **paired_token_hashes;                 /* SHA-256 hex hashes */
    size_t token_count;
    size_t token_cap;
    uint32_t failed_count;
    time_t lockout_until; /* 0 = not locked */
    hu_allocator_t *alloc;
};

static int dev_urandom_bytes(uint8_t *buf, size_t len) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    return (n == len) ? 0 : -1;
}

static int generate_code(char *out) {
    uint8_t buf[4];
    if (dev_urandom_bytes(buf, 4) != 0) {
        out[0] = '\0';
        hu_secure_zero(buf, sizeof(buf));
        return -1;
    }
    uint32_t val = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) |
                   ((uint32_t)buf[3] << 24);
    hu_secure_zero(buf, sizeof(buf));
    snprintf(out, HU_PAIRING_CODE_LEN + 1, "%08u", val % 100000000);
    return 0;
}

static int generate_token(char *out) {
    uint8_t buf[HU_TOKEN_RANDOM_BYTES];
    if (dev_urandom_bytes(buf, sizeof(buf)) != 0) {
        out[0] = '\0';
        hu_secure_zero(buf, sizeof(buf));
        return -1;
    }
    memcpy(out, HU_TOKEN_PREFIX, HU_TOKEN_PREFIX_LEN);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < HU_TOKEN_RANDOM_BYTES; i++) {
        out[HU_TOKEN_PREFIX_LEN + i * 2] = hex[buf[i] >> 4];
        out[HU_TOKEN_PREFIX_LEN + i * 2 + 1] = hex[buf[i] & 0x0f];
    }
    out[HU_TOKEN_TOTAL_LEN] = '\0';
    hu_secure_zero(buf, sizeof(buf));
    return 0;
}

static void hash_token_sha256(const char *token, char *out_hex) {
    uint8_t hash[32];
    hu_sha256((const uint8_t *)token, strlen(token), hash);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2] = hex[hash[i] >> 4];
        out_hex[i * 2 + 1] = hex[hash[i] & 0x0f];
    }
    out_hex[64] = '\0';
}

static bool is_token_hash(const char *s) {
    if (!s || strlen(s) != 64)
        return false;
    for (int i = 0; i < 64; i++) {
        if (!isxdigit((unsigned char)s[i]))
            return false;
    }
    return true;
}

bool hu_pairing_guard_constant_time_eq(const char *a, const char *b) {
    size_t la = a ? strlen(a) : 0;
    size_t lb = b ? strlen(b) : 0;
    if (la != lb)
        return false;
    size_t max_len = la > lb ? la : lb;
    unsigned char diff = 0;
    for (size_t i = 0; i < max_len; i++) {
        unsigned char x = (i < la && a) ? (unsigned char)a[i] : 0;
        unsigned char y = (i < lb && b) ? (unsigned char)b[i] : 0;
        diff |= x ^ y;
    }
    return diff == 0;
}

hu_pairing_guard_t *hu_pairing_guard_create(hu_allocator_t *alloc, bool require_pairing,
                                            const char **existing_tokens, size_t tokens_len) {
    if (!alloc)
        return NULL;

    hu_pairing_guard_t *guard =
        (hu_pairing_guard_t *)alloc->alloc(alloc->ctx, sizeof(hu_pairing_guard_t));
    if (!guard)
        return NULL;
    memset(guard, 0, sizeof(*guard));
    guard->require_pairing = require_pairing;
    guard->alloc = alloc;
    guard->pairing_code[0] = '\0';

    if (existing_tokens && tokens_len > 0) {
        guard->token_cap = tokens_len < 8 ? 8 : tokens_len;
        guard->paired_token_hashes =
            (char **)alloc->alloc(alloc->ctx, guard->token_cap * sizeof(char *));
        if (guard->paired_token_hashes) {
            for (size_t i = 0; i < tokens_len; i++) {
                if (!existing_tokens[i])
                    continue;
                char hash_buf[65];
                char *slot = (char *)alloc->alloc(alloc->ctx, 65);
                if (!slot) {
                    for (size_t j = 0; j < guard->token_count; j++)
                        alloc->free(alloc->ctx, guard->paired_token_hashes[j], 65);
                    alloc->free(alloc->ctx, guard->paired_token_hashes,
                                guard->token_cap * sizeof(char *));
                    alloc->free(alloc->ctx, guard, sizeof(*guard));
                    return NULL;
                }
                if (is_token_hash(existing_tokens[i])) {
                    memcpy(slot, existing_tokens[i], 64);
                    slot[64] = '\0';
                } else {
                    hash_token_sha256(existing_tokens[i], hash_buf);
                    memcpy(slot, hash_buf, 65);
                }
                guard->paired_token_hashes[guard->token_count++] = slot;
            }
        }
    } else {
        guard->paired_token_hashes = NULL;
        guard->token_cap = 0;
    }

    if (require_pairing && guard->token_count == 0) {
        if (generate_code(guard->pairing_code) != 0) {
            if (guard->paired_token_hashes)
                alloc->free(alloc->ctx, guard->paired_token_hashes,
                            guard->token_cap * sizeof(char *));
            alloc->free(alloc->ctx, guard, sizeof(*guard));
            return NULL;
        }
    }

    return guard;
}

void hu_pairing_guard_destroy(hu_pairing_guard_t *guard) {
    if (!guard)
        return;
    if (guard->paired_token_hashes) {
        for (size_t i = 0; i < guard->token_count; i++)
            guard->alloc->free(guard->alloc->ctx, guard->paired_token_hashes[i],
                               strlen(guard->paired_token_hashes[i]) + 1);
        guard->alloc->free(guard->alloc->ctx, guard->paired_token_hashes,
                           guard->token_cap * sizeof(char *));
    }
    guard->alloc->free(guard->alloc->ctx, guard, sizeof(hu_pairing_guard_t));
}

const char *hu_pairing_guard_pairing_code(hu_pairing_guard_t *guard) {
    if (!guard || !guard->pairing_code[0])
        return NULL;
    return guard->pairing_code;
}

hu_pair_attempt_result_t hu_pairing_guard_attempt_pair(hu_pairing_guard_t *guard, const char *code,
                                                       char **out_token) {
    if (!guard || !out_token)
        return HU_PAIR_INTERNAL_ERROR;
    *out_token = NULL;

    if (!guard->require_pairing)
        return HU_PAIR_DISABLED;
    if (guard->token_count > 0 && !guard->pairing_code[0])
        return HU_PAIR_ALREADY_PAIRED;
    if (!code || !code[0])
        return HU_PAIR_MISSING_CODE;

    if (guard->failed_count >= HU_MAX_PAIR_ATTEMPTS && guard->lockout_until > 0) {
        time_t now = time(NULL);
        if (now < guard->lockout_until)
            return HU_PAIR_LOCKED_OUT;
        guard->failed_count = 0;
        guard->lockout_until = 0;
    }

    if (!guard->pairing_code[0])
        return HU_PAIR_ALREADY_PAIRED;

    /* Trim code to 6 digits */
    char trimmed[16];
    size_t j = 0;
    for (size_t i = 0; code[i] && j < sizeof(trimmed) - 1; i++) {
        if (code[i] != ' ' && code[i] != '\t' && code[i] != '\r' && code[i] != '\n') {
            if (isdigit((unsigned char)code[i]))
                trimmed[j++] = code[i];
        }
    }
    trimmed[j] = '\0';

    if (strlen(trimmed) != HU_PAIRING_CODE_LEN) {
        guard->failed_count++;
        if (guard->failed_count >= HU_MAX_PAIR_ATTEMPTS)
            guard->lockout_until = time(NULL) + HU_PAIR_LOCKOUT_SECS;
        return HU_PAIR_INVALID_CODE;
    }

    if (!hu_pairing_guard_constant_time_eq(trimmed, guard->pairing_code)) {
        guard->failed_count++;
        if (guard->failed_count >= HU_MAX_PAIR_ATTEMPTS)
            guard->lockout_until = time(NULL) + HU_PAIR_LOCKOUT_SECS;
        return HU_PAIR_INVALID_CODE;
    }

    guard->failed_count = 0;
    guard->lockout_until = 0;
    guard->pairing_code[0] = '\0';

    char token[HU_TOKEN_TOTAL_LEN + 1];
    if (generate_token(token) != 0)
        return HU_PAIR_INTERNAL_ERROR;

    char hash_buf[65];
    hash_token_sha256(token, hash_buf);

    if (guard->token_count >= guard->token_cap) {
        size_t new_cap = guard->token_cap ? guard->token_cap * 2 : 8;
        char **n = (char **)guard->alloc->realloc(guard->alloc->ctx, guard->paired_token_hashes,
                                                  guard->token_cap * sizeof(char *),
                                                  new_cap * sizeof(char *));
        if (!n)
            return HU_PAIR_INTERNAL_ERROR;
        guard->paired_token_hashes = n;
        guard->token_cap = new_cap;
    }
    char *hash_slot = (char *)guard->alloc->alloc(guard->alloc->ctx, 65);
    if (!hash_slot)
        return HU_PAIR_INTERNAL_ERROR;
    memcpy(hash_slot, hash_buf, 65);

    *out_token = (char *)guard->alloc->alloc(guard->alloc->ctx, strlen(token) + 1);
    if (!*out_token) {
        guard->alloc->free(guard->alloc->ctx, hash_slot, 65);
        return HU_PAIR_INTERNAL_ERROR;
    }
    memcpy(*out_token, token, strlen(token) + 1);

    guard->paired_token_hashes[guard->token_count] = hash_slot;
    guard->token_count++;
    hu_secure_zero(token, sizeof(token));
    hu_secure_zero(hash_buf, sizeof(hash_buf));
    return HU_PAIR_PAIRED;
}

bool hu_pairing_guard_is_authenticated(const hu_pairing_guard_t *guard, const char *token) {
    if (!guard)
        return false;
    if (!guard->require_pairing)
        return true;
    if (!token)
        return false;

    char hash_buf[65];
    hash_token_sha256(token, hash_buf);

    for (size_t i = 0; i < guard->token_count; i++) {
        if (guard->paired_token_hashes[i] &&
            hu_pairing_guard_constant_time_eq(hash_buf, guard->paired_token_hashes[i])) {
            hu_secure_zero(hash_buf, sizeof(hash_buf));
            return true;
        }
    }
    hu_secure_zero(hash_buf, sizeof(hash_buf));
    return false;
}

bool hu_pairing_guard_is_paired(const hu_pairing_guard_t *guard) {
    return guard && guard->token_count > 0;
}
