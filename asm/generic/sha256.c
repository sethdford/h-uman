/*
 * SHA-256 — portable C fallback implementation.
 * Used on architectures without dedicated assembly (non-x86_64, non-aarch64).
 */
#include <stdint.h>
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define RR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(e, f, g)  (((e) & (f)) ^ (~(e) & (g)))
#define MAJ(a, b, c) (((a) & (b)) ^ ((a) & (c)) ^ ((b) & (c)))
#define EP0(a)  (RR(a, 2) ^ RR(a, 13) ^ RR(a, 22))
#define EP1(e)  (RR(e, 6) ^ RR(e, 11) ^ RR(e, 25))
#define SIG0(x) (RR(x, 7)  ^ RR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (RR(x, 17) ^ RR(x, 19) ^ ((x) >> 10))

typedef struct {
    uint32_t state[8];
    uint8_t buf[64];
    uint64_t total;
    size_t buflen;
} sha256_ctx_t;

static void sha256_transform(sha256_ctx_t *ctx) {
    uint32_t w[64];

    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)ctx->buf[i*4] << 24)
             | ((uint32_t)ctx->buf[i*4+1] << 16)
             | ((uint32_t)ctx->buf[i*4+2] << 8)
             | ((uint32_t)ctx->buf[i*4+3]);

    for (int i = 16; i < 64; i++)
        w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + EP1(e) + CH(e,f,g) + K[i] + w[i];
        uint32_t t2 = EP0(a) + MAJ(a,b,c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sc_sha256_init(sha256_ctx_t *ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->total = 0;
    ctx->buflen = 0;
}

void sc_sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        ctx->buf[ctx->buflen++] = data[i];
        if (ctx->buflen == 64) {
            sha256_transform(ctx);
            ctx->total += 512;
            ctx->buflen = 0;
        }
    }
}

void sc_sha256_final(sha256_ctx_t *ctx, uint8_t out[32]) {
    uint64_t total_bits = ctx->total + ctx->buflen * 8;

    ctx->buf[ctx->buflen++] = 0x80;
    if (ctx->buflen > 56) {
        while (ctx->buflen < 64) ctx->buf[ctx->buflen++] = 0;
        sha256_transform(ctx);
        ctx->buflen = 0;
    }
    while (ctx->buflen < 56) ctx->buf[ctx->buflen++] = 0;

    for (int i = 7; i >= 0; i--)
        ctx->buf[ctx->buflen++] = (uint8_t)(total_bits >> (i * 8));

    sha256_transform(ctx);

    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(ctx->state[i] >> 24);
        out[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        out[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
        out[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

void sc_sha256_generic(const uint8_t *data, size_t len, uint8_t out[32]) {
    sha256_ctx_t ctx;
    sc_sha256_init(&ctx);
    sc_sha256_update(&ctx, data, len);
    sc_sha256_final(&ctx, out);
}

void sc_hmac_sha256_generic(const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len,
                    uint8_t out[32]) {
    uint8_t k_pad[64];
    uint8_t k_ipad[64], k_opad[64];

    if (key_len > 64) {
        sc_sha256_generic(key, key_len, k_pad);
        memset(k_pad + 32, 0, 32);
    } else {
        memcpy(k_pad, key, key_len);
        memset(k_pad + key_len, 0, 64 - key_len);
    }

    for (int i = 0; i < 64; i++) {
        k_ipad[i] = k_pad[i] ^ 0x36;
        k_opad[i] = k_pad[i] ^ 0x5c;
    }

    sha256_ctx_t ctx;
    sc_sha256_init(&ctx);
    sc_sha256_update(&ctx, k_ipad, 64);
    sc_sha256_update(&ctx, msg, msg_len);
    uint8_t inner[32];
    sc_sha256_final(&ctx, inner);

    sc_sha256_init(&ctx);
    sc_sha256_update(&ctx, k_opad, 64);
    sc_sha256_update(&ctx, inner, 32);
    sc_sha256_final(&ctx, out);
}
