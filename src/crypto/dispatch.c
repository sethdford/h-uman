/*
 * Crypto runtime dispatch: selects optimal implementation by architecture.
 *
 * x86_64: SHA-256 uses runtime CPUID to pick SHA-NI vs generic.
 * ChaCha20 uses generic (SSE2 asm exists but not wired; could add if desired).
 */
#include "seaclaw/crypto.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern void sc_chacha20_encrypt_generic(const uint8_t key[32], const uint8_t nonce[12],
                                        uint32_t counter, const uint8_t *in, uint8_t *out,
                                        size_t len);
extern void sc_chacha20_decrypt_generic(const uint8_t key[32], const uint8_t nonce[12],
                                        uint32_t counter, const uint8_t *in, uint8_t *out,
                                        size_t len);
extern void sc_sha256_generic(const uint8_t *data, size_t len, uint8_t out[32]);
extern void sc_hmac_sha256_generic(const uint8_t *key, size_t key_len,
                                   const uint8_t *msg, size_t msg_len,
                                   uint8_t out[32]);

#if defined(__x86_64__)
extern void sc_sha256_x86_64(const uint8_t *data, size_t len, uint8_t out[32]);

static bool has_sha_ni(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(7), "c"(0));
    return (ebx >> 29) & 1;
}

static bool sha_ni_checked = false;
static bool sha_ni_available = false;
#endif

void sc_chacha20_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                         uint32_t counter, const uint8_t *in, uint8_t *out,
                         size_t len) {
    sc_chacha20_encrypt_generic(key, nonce, counter, in, out, len);
}

void sc_chacha20_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                         uint32_t counter, const uint8_t *in, uint8_t *out,
                         size_t len) {
    sc_chacha20_decrypt_generic(key, nonce, counter, in, out, len);
}

void sc_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
#if defined(__x86_64__) && !SC_IS_TEST
    if (!sha_ni_checked) {
        sha_ni_available = has_sha_ni();
        sha_ni_checked = true;
    }
    if (sha_ni_available) {
        sc_sha256_x86_64(data, len, out);
        return;
    }
#endif
    sc_sha256_generic(data, len, out);
}

void sc_hmac_sha256(const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len,
                    uint8_t out[32]) {
    sc_hmac_sha256_generic(key, key_len, msg, msg_len, out);
}
