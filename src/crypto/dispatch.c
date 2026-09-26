/*
 * Crypto runtime dispatch: selects optimal implementation by architecture.
 *
 * x86_64: SHA-256 uses runtime CPUID to pick SHA-NI vs generic.
 * aarch64: ChaCha20 uses NEON-accelerated implementation.
 *          SHA-256 uses crypto extensions (when available).
 * ChaCha20: aarch64 uses NEON asm; x86_64 uses generic (SSE2 asm exists but not wired).
 */
#include "human/crypto.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern void hu_chacha20_encrypt_generic(const uint8_t key[32], const uint8_t nonce[12],
                                        uint32_t counter, const uint8_t *in, uint8_t *out,
                                        size_t len);
extern void hu_chacha20_decrypt_generic(const uint8_t key[32], const uint8_t nonce[12],
                                        uint32_t counter, const uint8_t *in, uint8_t *out,
                                        size_t len);
extern void hu_sha256_generic(const uint8_t *data, size_t len, uint8_t out[32]);
extern void hu_hmac_sha256_generic(const uint8_t *key, size_t key_len, const uint8_t *msg,
                                   size_t msg_len, uint8_t out[32]);

#if defined(__aarch64__) || defined(__arm64__)
extern void hu_chacha20_encrypt_aarch64(const uint8_t key[32], const uint8_t nonce[12],
                                        uint32_t counter, const uint8_t *in, uint8_t *out,
                                        size_t len);
extern void hu_chacha20_decrypt_aarch64(const uint8_t key[32], const uint8_t nonce[12],
                                        uint32_t counter, const uint8_t *in, uint8_t *out,
                                        size_t len);
#endif

#if (defined(__aarch64__) || defined(__arm64__)) && !defined(HU_IS_TEST)
extern void hu_sha256_aarch64(const uint8_t *data, size_t len, uint8_t out[32]);
#endif

#if defined(__x86_64__) && !defined(HU_IS_TEST)
extern void hu_sha256_x86_64(const uint8_t *data, size_t len, uint8_t out[32]);

static bool has_sha_ni(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
    return (ebx >> 29) & 1;
}

static bool sha_ni_checked = false;
static bool sha_ni_available = false;
#endif

void hu_chacha20_encrypt(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
                         const uint8_t *in, uint8_t *out, size_t len) {
#if (defined(__aarch64__) || defined(__arm64__)) && !defined(HU_IS_TEST)
    hu_chacha20_encrypt_aarch64(key, nonce, counter, in, out, len);
#else
    hu_chacha20_encrypt_generic(key, nonce, counter, in, out, len);
#endif
}

void hu_chacha20_decrypt(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
                         const uint8_t *in, uint8_t *out, size_t len) {
#if (defined(__aarch64__) || defined(__arm64__)) && !defined(HU_IS_TEST)
    hu_chacha20_decrypt_aarch64(key, nonce, counter, in, out, len);
#else
    hu_chacha20_decrypt_generic(key, nonce, counter, in, out, len);
#endif
}

void hu_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
#if defined(__x86_64__) && !defined(HU_IS_TEST)
    if (!sha_ni_checked) {
        sha_ni_available = has_sha_ni();
        sha_ni_checked = true;
    }
    if (sha_ni_available) {
        hu_sha256_x86_64(data, len, out);
        return;
    }
#endif
#if (defined(__aarch64__) || defined(__arm64__)) && !defined(HU_IS_TEST)
    hu_sha256_aarch64(data, len, out);
    return;
#endif
    hu_sha256_generic(data, len, out);
}

void hu_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                    uint8_t out[32]) {
    hu_hmac_sha256_generic(key, key_len, msg, msg_len, out);
}
