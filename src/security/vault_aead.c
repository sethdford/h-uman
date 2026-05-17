/*
 * Vault AEAD — implementation.
 *
 * See include/human/security/vault_aead.h for the full contract and
 * docs/plans/2026-05-17-vault-encryption-migration-plan.md for the
 * migration phasing rationale.
 *
 * Backend dispatch is COMPILE-TIME (defines below). All compiled-in
 * backends can DECRYPT (so a future libsodium build can still read v3
 * envelopes written by today's non-libsodium build); ENCRYPT always
 * uses the highest-priority backend linked into this binary.
 */

#include "human/security/vault_aead.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/crypto.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sys/syscall.h>
#if __has_include(<sys/random.h>)
#include <sys/random.h>
#endif
#endif

#ifdef HU_HAS_LIBSODIUM
#include <sodium.h>
#endif

#ifdef HU_ENABLE_FIPS_CRYPTO
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

/* ── envelope layout constants ──────────────────────────────────────── */

#define VA_MAGIC_LIBSODIUM   0x01u
#define VA_MAGIC_OPENSSL_GCM 0x02u
#define VA_MAGIC_CHACHA_HMAC 0x03u

#define VA_HMAC_LEN          32u
#define VA_CHACHA_NONCE_LEN  12u
#define VA_AES_GCM_NONCE_LEN 12u
#define VA_AES_GCM_TAG_LEN   16u

#ifdef HU_HAS_LIBSODIUM
#define VA_XCHACHA_NONCE_LEN crypto_aead_xchacha20poly1305_ietf_NPUBBYTES /* 24 */
#define VA_XCHACHA_TAG_LEN   crypto_aead_xchacha20poly1305_ietf_ABYTES    /* 16 */
#endif

/* ── secure zero ────────────────────────────────────────────────────── */

#if defined(__STDC_LIB_EXT1__)
#define va_secure_zero(p, n) memset_s((p), (n), 0, (n))
#elif defined(__GNUC__) || defined(__clang__)
static void va_secure_zero(void *p, size_t n) {
    memset(p, 0, n);
    __asm__ __volatile__("" : : "r"(p) : "memory");
}
#else
static void va_secure_zero(void *p, size_t n) {
    volatile unsigned char *vp = (volatile unsigned char *)p;
    while (n--)
        *vp++ = 0;
}
#endif

/* ── cryptographic randomness ───────────────────────────────────────── */

/* Returns 0 on success, -1 on failure. We never silently return
 * deterministic data: a same-key/same-nonce reuse is catastrophic in
 * any AEAD construction, so we'd rather fail encrypt outright. */
static int va_random_bytes(uint8_t *buf, size_t len) {
    if (len == 0)
        return 0;
#ifdef HU_HAS_LIBSODIUM
    /* libsodium handles its own init; randombytes_buf is thread-safe
     * and never blocks long-term. */
    if (sodium_init() >= 0) {
        randombytes_buf(buf, len);
        return 0;
    }
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(buf, len);
    return 0;
#elif defined(__linux__)
    size_t off = 0;
    while (off < len) {
#if defined(SYS_getrandom)
        long n = syscall(SYS_getrandom, buf + off, len - off, 0);
#else
        long n = -1;
        errno = ENOSYS;
#endif
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        off += (size_t)n;
    }
    if (off == len)
        return 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    while (off < len) {
        ssize_t n = read(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (n == 0) {
            close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    close(fd);
    return 0;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f)
        return -1;
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    return got == len ? 0 : -1;
#endif
}

/* ── constant-time compare ──────────────────────────────────────────── */

/* Constant-time memcmp returning 0 on equal, non-zero on differ.
 * Defends against timing side-channels in the EtM tag check. */
static int va_ct_memcmp(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff;
}

/* ── public: active backend ─────────────────────────────────────────── */

hu_vault_aead_backend_t hu_vault_aead_active_backend(void) {
#ifdef HU_HAS_LIBSODIUM
    return HU_VAULT_AEAD_BACKEND_LIBSODIUM;
#elif defined(HU_ENABLE_FIPS_CRYPTO)
    return HU_VAULT_AEAD_BACKEND_OPENSSL_AES_GCM;
#else
    return HU_VAULT_AEAD_BACKEND_CHACHA20_HMAC;
#endif
}

/* ── v1: libsodium XChaCha20-Poly1305-IETF ──────────────────────────── */

#ifdef HU_HAS_LIBSODIUM
static hu_error_t va_encrypt_v1(hu_allocator_t *alloc, const uint8_t key[HU_VAULT_AEAD_KEY_LEN],
                                const uint8_t *aad, size_t aad_len, const uint8_t *pt,
                                size_t pt_len, uint8_t **out, size_t *out_len) {
    if (sodium_init() < 0)
        return HU_ERR_CRYPTO_ENCRYPT;

    size_t env_len = 1u + VA_XCHACHA_NONCE_LEN + pt_len + VA_XCHACHA_TAG_LEN;
    uint8_t *env = (uint8_t *)alloc->alloc(alloc->ctx, env_len);
    if (!env)
        return HU_ERR_OUT_OF_MEMORY;

    env[0] = VA_MAGIC_LIBSODIUM;
    uint8_t *nonce = env + 1;
    if (va_random_bytes(nonce, VA_XCHACHA_NONCE_LEN) != 0) {
        alloc->free(alloc->ctx, env, env_len);
        return HU_ERR_CRYPTO_ENCRYPT;
    }

    unsigned long long ct_with_tag_len = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            env + 1 + VA_XCHACHA_NONCE_LEN, &ct_with_tag_len, pt, (unsigned long long)pt_len, aad,
            (unsigned long long)aad_len, NULL, nonce, key) != 0) {
        alloc->free(alloc->ctx, env, env_len);
        return HU_ERR_CRYPTO_ENCRYPT;
    }

    *out = env;
    *out_len = 1u + VA_XCHACHA_NONCE_LEN + (size_t)ct_with_tag_len;
    return HU_OK;
}

static hu_error_t va_decrypt_v1(hu_allocator_t *alloc, const uint8_t key[HU_VAULT_AEAD_KEY_LEN],
                                const uint8_t *aad, size_t aad_len, const uint8_t *env,
                                size_t env_len, uint8_t **out, size_t *out_len) {
    if (sodium_init() < 0)
        return HU_ERR_CRYPTO_DECRYPT;

    if (env_len < 1u + VA_XCHACHA_NONCE_LEN + VA_XCHACHA_TAG_LEN)
        return HU_ERR_CRYPTO_DECRYPT;

    const uint8_t *nonce = env + 1;
    const uint8_t *ct_with_tag = env + 1 + VA_XCHACHA_NONCE_LEN;
    unsigned long long ct_with_tag_len = (unsigned long long)(env_len - 1u - VA_XCHACHA_NONCE_LEN);

    /* Worst-case pt_len = ct_with_tag_len - tag_len. Could be 0. */
    size_t pt_max = (size_t)ct_with_tag_len - VA_XCHACHA_TAG_LEN;
    uint8_t *pt = (uint8_t *)alloc->alloc(alloc->ctx, pt_max == 0 ? 1 : pt_max);
    if (!pt)
        return HU_ERR_OUT_OF_MEMORY;

    unsigned long long pt_len = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(pt, &pt_len, NULL, ct_with_tag, ct_with_tag_len,
                                                   aad, (unsigned long long)aad_len, nonce,
                                                   key) != 0) {
        va_secure_zero(pt, pt_max == 0 ? 1 : pt_max);
        alloc->free(alloc->ctx, pt, pt_max == 0 ? 1 : pt_max);
        return HU_ERR_CRYPTO_DECRYPT;
    }
    *out = pt;
    *out_len = (size_t)pt_len;
    return HU_OK;
}
#endif /* HU_HAS_LIBSODIUM */

/* ── v2: OpenSSL AES-256-GCM ────────────────────────────────────────── */

#ifdef HU_ENABLE_FIPS_CRYPTO
static hu_error_t va_encrypt_v2(hu_allocator_t *alloc, const uint8_t key[HU_VAULT_AEAD_KEY_LEN],
                                const uint8_t *aad, size_t aad_len, const uint8_t *pt,
                                size_t pt_len, uint8_t **out, size_t *out_len) {
    size_t env_len = 1u + VA_AES_GCM_NONCE_LEN + pt_len + VA_AES_GCM_TAG_LEN;
    uint8_t *env = (uint8_t *)alloc->alloc(alloc->ctx, env_len);
    if (!env)
        return HU_ERR_OUT_OF_MEMORY;

    env[0] = VA_MAGIC_OPENSSL_GCM;
    uint8_t *nonce = env + 1;
    if (va_random_bytes(nonce, VA_AES_GCM_NONCE_LEN) != 0) {
        alloc->free(alloc->ctx, env, env_len);
        return HU_ERR_CRYPTO_ENCRYPT;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        alloc->free(alloc->ctx, env, env_len);
        return HU_ERR_OUT_OF_MEMORY;
    }

    hu_error_t rc = HU_OK;
    int len = 0;
    int total = 0;
    uint8_t *ct_out = env + 1 + VA_AES_GCM_NONCE_LEN;
    uint8_t *tag_out = ct_out; /* updated below */

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
        rc = HU_ERR_CRYPTO_ENCRYPT;
        goto done;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)VA_AES_GCM_NONCE_LEN, NULL) != 1) {
        rc = HU_ERR_CRYPTO_ENCRYPT;
        goto done;
    }
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) {
        rc = HU_ERR_CRYPTO_ENCRYPT;
        goto done;
    }
    if (aad && aad_len > 0) {
        if (EVP_EncryptUpdate(ctx, NULL, &len, aad, (int)aad_len) != 1) {
            rc = HU_ERR_CRYPTO_ENCRYPT;
            goto done;
        }
    }
    if (pt_len > 0) {
        if (EVP_EncryptUpdate(ctx, ct_out, &len, pt, (int)pt_len) != 1) {
            rc = HU_ERR_CRYPTO_ENCRYPT;
            goto done;
        }
        total = len;
    }
    if (EVP_EncryptFinal_ex(ctx, ct_out + total, &len) != 1) {
        rc = HU_ERR_CRYPTO_ENCRYPT;
        goto done;
    }
    total += len;

    tag_out = ct_out + total;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, (int)VA_AES_GCM_TAG_LEN, tag_out) != 1) {
        rc = HU_ERR_CRYPTO_ENCRYPT;
        goto done;
    }
    *out = env;
    *out_len = 1u + VA_AES_GCM_NONCE_LEN + (size_t)total + VA_AES_GCM_TAG_LEN;

done:
    EVP_CIPHER_CTX_free(ctx);
    if (rc != HU_OK)
        alloc->free(alloc->ctx, env, env_len);
    return rc;
}

static hu_error_t va_decrypt_v2(hu_allocator_t *alloc, const uint8_t key[HU_VAULT_AEAD_KEY_LEN],
                                const uint8_t *aad, size_t aad_len, const uint8_t *env,
                                size_t env_len, uint8_t **out, size_t *out_len) {
    if (env_len < 1u + VA_AES_GCM_NONCE_LEN + VA_AES_GCM_TAG_LEN)
        return HU_ERR_CRYPTO_DECRYPT;

    const uint8_t *nonce = env + 1;
    size_t ct_len = env_len - 1u - VA_AES_GCM_NONCE_LEN - VA_AES_GCM_TAG_LEN;
    const uint8_t *ct_in = env + 1 + VA_AES_GCM_NONCE_LEN;
    const uint8_t *tag = ct_in + ct_len;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;

    /* Allocate at least one byte even for zero-length plaintexts so we
     * never call alloc(0) (implementation-defined). */
    size_t alloc_len = ct_len == 0 ? 1 : ct_len;
    uint8_t *pt = (uint8_t *)alloc->alloc(alloc->ctx, alloc_len);
    if (!pt) {
        EVP_CIPHER_CTX_free(ctx);
        return HU_ERR_OUT_OF_MEMORY;
    }

    hu_error_t rc = HU_OK;
    int len = 0;
    int total = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) {
        rc = HU_ERR_CRYPTO_DECRYPT;
        goto done;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)VA_AES_GCM_NONCE_LEN, NULL) != 1) {
        rc = HU_ERR_CRYPTO_DECRYPT;
        goto done;
    }
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) {
        rc = HU_ERR_CRYPTO_DECRYPT;
        goto done;
    }
    if (aad && aad_len > 0) {
        if (EVP_DecryptUpdate(ctx, NULL, &len, aad, (int)aad_len) != 1) {
            rc = HU_ERR_CRYPTO_DECRYPT;
            goto done;
        }
    }
    if (ct_len > 0) {
        if (EVP_DecryptUpdate(ctx, pt, &len, ct_in, (int)ct_len) != 1) {
            rc = HU_ERR_CRYPTO_DECRYPT;
            goto done;
        }
        total = len;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, (int)VA_AES_GCM_TAG_LEN, (void *)tag) != 1) {
        rc = HU_ERR_CRYPTO_DECRYPT;
        goto done;
    }
    if (EVP_DecryptFinal_ex(ctx, pt + total, &len) != 1) {
        rc = HU_ERR_CRYPTO_DECRYPT;
        goto done;
    }
    total += len;
    *out = pt;
    *out_len = (size_t)total;

done:
    EVP_CIPHER_CTX_free(ctx);
    if (rc != HU_OK) {
        va_secure_zero(pt, alloc_len);
        alloc->free(alloc->ctx, pt, alloc_len);
    }
    return rc;
}
#endif /* HU_ENABLE_FIPS_CRYPTO */

/* ── v3: ChaCha20 + HMAC-SHA256 (Encrypt-then-MAC) ──────────────────── */

/* HMAC input: nonce || ciphertext || aad_len_le64 || aad
 *
 * Binding aad_len into the MAC prevents canonicalization attacks where
 * an attacker shifts bytes between aad and ciphertext. We use a fixed
 * 8-byte little-endian length tag (matches RFC 7539 section 2.8). */
static void va_etm_compute_tag(const uint8_t mac_key[VA_HMAC_LEN], const uint8_t *nonce,
                               const uint8_t *ct, size_t ct_len, const uint8_t *aad, size_t aad_len,
                               uint8_t out_tag[VA_HMAC_LEN]) {
    /* Build the message in a heap-free way using two HMAC passes is
     * possible, but for simplicity (and because vault values are small)
     * we concatenate up to a bounded buffer. The vault hard-caps single
     * values at 8KiB so this stays well under stack-frame limits. */
    /* Concatenate in chunks to avoid one giant stack array — but for
     * 8K-bounded values, one buffer is fine. */
    /* Cap: nonce(12) + ct + 8 + aad. The encryption path enforces
     * pt_len and aad_len; this helper trusts the caller. */
    /* Use heap allocation through caller? Simpler: caller-side bound.
     * To keep this helper self-contained, malloc here and free at end. */
    size_t msg_len = VA_CHACHA_NONCE_LEN + ct_len + 8u + aad_len;
    uint8_t *msg = (uint8_t *)malloc(msg_len == 0 ? 1 : msg_len);
    if (!msg) {
        /* Best-effort: hash an empty buffer; caller's ct check will
         * fail since the tag won't match either side. */
        hu_hmac_sha256(mac_key, VA_HMAC_LEN, NULL, 0, out_tag);
        return;
    }
    size_t off = 0;
    memcpy(msg + off, nonce, VA_CHACHA_NONCE_LEN);
    off += VA_CHACHA_NONCE_LEN;
    if (ct_len > 0) {
        memcpy(msg + off, ct, ct_len);
        off += ct_len;
    }
    /* aad length, little-endian 64-bit */
    uint64_t aad_len_u64 = (uint64_t)aad_len;
    for (int i = 0; i < 8; i++) {
        msg[off++] = (uint8_t)((aad_len_u64 >> (8 * i)) & 0xff);
    }
    if (aad_len > 0) {
        memcpy(msg + off, aad, aad_len);
        off += aad_len;
    }
    hu_hmac_sha256(mac_key, VA_HMAC_LEN, msg, off, out_tag);
    va_secure_zero(msg, msg_len == 0 ? 1 : msg_len);
    free(msg);
}

/* Derive a 32-byte MAC subkey from the encryption key via a one-shot
 * HKDF-like extraction: mac_key = HMAC-SHA256(key, "vault-aead-v3-mac").
 * This prevents the same byte sequence from acting as both stream-cipher
 * key and HMAC key (best practice; see RFC 5869 motivation). */
static void va_etm_derive_mac_key(const uint8_t key[HU_VAULT_AEAD_KEY_LEN],
                                  uint8_t out_mac_key[VA_HMAC_LEN]) {
    static const char info[] = "vault-aead-v3-mac";
    hu_hmac_sha256(key, HU_VAULT_AEAD_KEY_LEN, (const uint8_t *)info, sizeof(info) - 1,
                   out_mac_key);
}

static hu_error_t va_encrypt_v3(hu_allocator_t *alloc, const uint8_t key[HU_VAULT_AEAD_KEY_LEN],
                                const uint8_t *aad, size_t aad_len, const uint8_t *pt,
                                size_t pt_len, uint8_t **out, size_t *out_len) {
    size_t env_len = 1u + VA_CHACHA_NONCE_LEN + pt_len + VA_HMAC_LEN;
    uint8_t *env = (uint8_t *)alloc->alloc(alloc->ctx, env_len);
    if (!env)
        return HU_ERR_OUT_OF_MEMORY;

    env[0] = VA_MAGIC_CHACHA_HMAC;
    uint8_t *nonce = env + 1;
    if (va_random_bytes(nonce, VA_CHACHA_NONCE_LEN) != 0) {
        alloc->free(alloc->ctx, env, env_len);
        return HU_ERR_CRYPTO_ENCRYPT;
    }

    uint8_t *ct = env + 1 + VA_CHACHA_NONCE_LEN;
    /* Counter starts at 1 to match the keystore v0 convention; RFC 7539
     * allows any starting counter since nonce+counter form the unique
     * input. */
    hu_chacha20_encrypt(key, nonce, 1, pt, ct, pt_len);

    uint8_t mac_key[VA_HMAC_LEN];
    va_etm_derive_mac_key(key, mac_key);
    uint8_t tag[VA_HMAC_LEN];
    va_etm_compute_tag(mac_key, nonce, ct, pt_len, aad, aad_len, tag);
    memcpy(env + 1 + VA_CHACHA_NONCE_LEN + pt_len, tag, VA_HMAC_LEN);
    va_secure_zero(mac_key, sizeof(mac_key));
    va_secure_zero(tag, sizeof(tag));

    *out = env;
    *out_len = env_len;
    return HU_OK;
}

static hu_error_t va_decrypt_v3(hu_allocator_t *alloc, const uint8_t key[HU_VAULT_AEAD_KEY_LEN],
                                const uint8_t *aad, size_t aad_len, const uint8_t *env,
                                size_t env_len, uint8_t **out, size_t *out_len) {
    if (env_len < 1u + VA_CHACHA_NONCE_LEN + VA_HMAC_LEN)
        return HU_ERR_CRYPTO_DECRYPT;

    const uint8_t *nonce = env + 1;
    size_t ct_len = env_len - 1u - VA_CHACHA_NONCE_LEN - VA_HMAC_LEN;
    const uint8_t *ct = env + 1 + VA_CHACHA_NONCE_LEN;
    const uint8_t *tag_stored = ct + ct_len;

    uint8_t mac_key[VA_HMAC_LEN];
    va_etm_derive_mac_key(key, mac_key);
    uint8_t tag_computed[VA_HMAC_LEN];
    va_etm_compute_tag(mac_key, nonce, ct, ct_len, aad, aad_len, tag_computed);
    int tag_diff = va_ct_memcmp(tag_computed, tag_stored, VA_HMAC_LEN);
    va_secure_zero(mac_key, sizeof(mac_key));
    va_secure_zero(tag_computed, sizeof(tag_computed));
    if (tag_diff != 0)
        return HU_ERR_CRYPTO_DECRYPT;

    /* MAC verified — safe to decrypt. */
    size_t alloc_len = ct_len == 0 ? 1 : ct_len;
    uint8_t *pt = (uint8_t *)alloc->alloc(alloc->ctx, alloc_len);
    if (!pt)
        return HU_ERR_OUT_OF_MEMORY;
    hu_chacha20_decrypt(key, nonce, 1, ct, pt, ct_len);
    *out = pt;
    *out_len = ct_len;
    return HU_OK;
}

/* ── public: encrypt ────────────────────────────────────────────────── */

hu_error_t hu_vault_aead_encrypt(hu_allocator_t *alloc, const uint8_t key[HU_VAULT_AEAD_KEY_LEN],
                                 const uint8_t *aad, size_t aad_len, const uint8_t *plaintext,
                                 size_t pt_len, uint8_t **out_ciphertext, size_t *out_len) {
    if (!alloc || !key || !out_ciphertext || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (pt_len > 0 && !plaintext)
        return HU_ERR_INVALID_ARGUMENT;
    if (aad_len > 0 && !aad)
        return HU_ERR_INVALID_ARGUMENT;
    *out_ciphertext = NULL;
    *out_len = 0;

#ifdef HU_HAS_LIBSODIUM
    return va_encrypt_v1(alloc, key, aad, aad_len, plaintext, pt_len, out_ciphertext, out_len);
#elif defined(HU_ENABLE_FIPS_CRYPTO)
    return va_encrypt_v2(alloc, key, aad, aad_len, plaintext, pt_len, out_ciphertext, out_len);
#else
    return va_encrypt_v3(alloc, key, aad, aad_len, plaintext, pt_len, out_ciphertext, out_len);
#endif
}

/* ── public: decrypt (dispatches by magic byte) ─────────────────────── */

hu_error_t hu_vault_aead_decrypt(hu_allocator_t *alloc, const uint8_t key[HU_VAULT_AEAD_KEY_LEN],
                                 const uint8_t *aad, size_t aad_len, const uint8_t *ciphertext,
                                 size_t ct_len, uint8_t **out_plaintext, size_t *out_len) {
    if (!alloc || !key || !out_plaintext || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (ct_len < 1 || !ciphertext)
        return HU_ERR_INVALID_ARGUMENT;
    if (aad_len > 0 && !aad)
        return HU_ERR_INVALID_ARGUMENT;
    *out_plaintext = NULL;
    *out_len = 0;

    uint8_t magic = ciphertext[0];
    switch (magic) {
#ifdef HU_HAS_LIBSODIUM
    case VA_MAGIC_LIBSODIUM:
        return va_decrypt_v1(alloc, key, aad, aad_len, ciphertext, ct_len, out_plaintext, out_len);
#endif
#ifdef HU_ENABLE_FIPS_CRYPTO
    case VA_MAGIC_OPENSSL_GCM:
        return va_decrypt_v2(alloc, key, aad, aad_len, ciphertext, ct_len, out_plaintext, out_len);
#endif
    case VA_MAGIC_CHACHA_HMAC:
        return va_decrypt_v3(alloc, key, aad, aad_len, ciphertext, ct_len, out_plaintext, out_len);
    default:
        /* Unknown magic, or magic for a backend not compiled in. */
        return HU_ERR_CRYPTO_DECRYPT;
    }
}
