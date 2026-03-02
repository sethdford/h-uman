/*
 * ChaCha20-Poly1305 — portable C fallback implementation.
 * Used on architectures without dedicated assembly (non-x86_64, non-aarch64).
 * Optimized assembly versions live in asm/x86_64/ and asm/aarch64/.
 */
#include <stdint.h>
#include <string.h>

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

#define QR(a, b, c, d) \
    a += b; d ^= a; d = ROTL32(d, 16); \
    c += d; b ^= c; b = ROTL32(b, 12); \
    a += b; d ^= a; d = ROTL32(d, 8);  \
    c += d; b ^= c; b = ROTL32(b, 7)

static void chacha20_block(uint32_t out[16], const uint32_t in[16]) {
    uint32_t x[16];
    memcpy(x, in, 64);

    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[ 8], x[12]);
        QR(x[1], x[5], x[ 9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);

        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[ 8], x[13]);
        QR(x[3], x[4], x[ 9], x[14]);
    }

    for (int i = 0; i < 16; i++)
        out[i] = x[i] + in[i];
}

static void u32_to_le(uint8_t *dst, uint32_t val) {
    dst[0] = (uint8_t)(val);
    dst[1] = (uint8_t)(val >> 8);
    dst[2] = (uint8_t)(val >> 16);
    dst[3] = (uint8_t)(val >> 24);
}

static uint32_t le_to_u32(const uint8_t *src) {
    return (uint32_t)src[0]
         | ((uint32_t)src[1] << 8)
         | ((uint32_t)src[2] << 16)
         | ((uint32_t)src[3] << 24);
}

void sc_chacha20_encrypt_generic(const uint8_t key[32], const uint8_t nonce[12],
                                 uint32_t counter, const uint8_t *in, uint8_t *out,
                                 size_t len) {
    uint32_t state[16];
    state[0] = 0x61707865; /* "expa" */
    state[1] = 0x3320646e; /* "nd 3" */
    state[2] = 0x79622d32; /* "2-by" */
    state[3] = 0x6b206574; /* "te k" */

    for (int i = 0; i < 8; i++)
        state[4 + i] = le_to_u32(key + i * 4);

    state[12] = counter;
    state[13] = le_to_u32(nonce);
    state[14] = le_to_u32(nonce + 4);
    state[15] = le_to_u32(nonce + 8);

    uint8_t keystream[64];
    size_t off = 0;

    while (off < len) {
        uint32_t block[16];
        chacha20_block(block, state);
        for (int i = 0; i < 16; i++)
            u32_to_le(keystream + i * 4, block[i]);

        size_t chunk = len - off;
        if (chunk > 64) chunk = 64;

        for (size_t i = 0; i < chunk; i++)
            out[off + i] = in[off + i] ^ keystream[i];

        off += chunk;
        state[12]++;
    }
}

void sc_chacha20_decrypt_generic(const uint8_t key[32], const uint8_t nonce[12],
                                 uint32_t counter, const uint8_t *in, uint8_t *out,
                                 size_t len) {
    sc_chacha20_encrypt_generic(key, nonce, counter, in, out, len);
}
