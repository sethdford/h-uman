/*
 * W15 — Cryptographic keystore implementation.
 *
 * PLACEHOLDER CRYPTO — safe for testing, NOT production-secure.
 * See TODO(W15-secure) markers throughout. Replace all marked sections
 * when libsodium is integrated (follow-up PR within W15).
 *
 * Current AEAD: ChaCha20 (ciphertext) + HMAC-SHA256 (authentication tag).
 * Format: [nonce:12][ciphertext:N][hmac:32]
 * Nonce: fixed zeros — a random nonce is required for production security.
 *
 * KDF: SHA-256(passphrase || '\x01' || user_id)
 * This is deterministic and NOT resistant to dictionary attacks. Replace with
 * libsodium argon2id before shipping to users.
 *
 * Tombstone: hu_keystore_destroy_master_key writes a zero-byte file
 * <key_dir>/<user_id>.tomb. Future unlock attempts check for this file and
 * return HU_ERR_CRYPTO_DECRYPT, preventing key re-derivation.
 */

#include "human/security/keystore.h"
#include "human/core/error.h"
#include "human/crypto.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/stat.h>
#endif

/* ── constants ─────────────────────────────────────────────────────────── */

#define KS_KEY_LEN      32   /* master key and per-table data key length */
#define KS_NONCE_LEN    12   /* ChaCha20 nonce length */
#define KS_HMAC_LEN     32   /* HMAC-SHA256 output length */
#define KS_CT_OVERHEAD  (KS_NONCE_LEN + KS_HMAC_LEN)
#define KS_USER_ID_MAX  128
#define KS_DIR_MAX      512
#define KS_PATH_MAX     (KS_DIR_MAX + KS_USER_ID_MAX + 16)

/* ── secure zero ────────────────────────────────────────────────────────── */

#if defined(__STDC_LIB_EXT1__)
#define ks_secure_zero(p, n) memset_s((p), (n), 0, (n))
#elif defined(__GNUC__) || defined(__clang__)
static void ks_secure_zero(void *p, size_t n) {
    memset(p, 0, n);
    __asm__ __volatile__("" : : "r"(p) : "memory");
}
#else
static void ks_secure_zero(void *p, size_t n) {
    volatile unsigned char *vp = (volatile unsigned char *)p;
    while (n--)
        *vp++ = 0;
}
#endif

/* ── struct ─────────────────────────────────────────────────────────────── */

struct hu_keystore {
    hu_allocator_t *alloc;
    char            user_id[KS_USER_ID_MAX];
    uint8_t         master_key[KS_KEY_LEN];
    int             data_keys_count; /* number of distinct tables encrypted */
    bool            master_key_present;
};

/* ── directory helpers ──────────────────────────────────────────────────── */

/* Returns the directory used for tombstone files.
 * Priority: HU_KEYSTORE_DIR env var → /tmp (test) or ~/.human/keys (prod). */
static void keystore_dir(char *out, size_t cap) {
    const char *env = getenv("HU_KEYSTORE_DIR");
    if (env && *env) {
        snprintf(out, cap, "%s", env);
        return;
    }
#if defined(HU_IS_TEST) && HU_IS_TEST
    snprintf(out, cap, "/tmp");
#else
    const char *home = getenv("HOME");
    if (home && *home)
        snprintf(out, cap, "%s/.human/keys", home);
    else
        snprintf(out, cap, "/tmp");
#endif
}

static void tombstone_path(const char *user_id, char *out, size_t cap) {
    char dir[KS_DIR_MAX];
    keystore_dir(dir, sizeof(dir));
    snprintf(out, cap, "%s/%s.tomb", dir, user_id);
}

static bool tombstone_exists(const char *user_id) {
    char path[KS_PATH_MAX];
    tombstone_path(user_id, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

/* ── KDF ────────────────────────────────────────────────────────────────── */

/* TODO(W15-secure): replace with libsodium argon2id (OWASP: m=65536, t=3, p=1).
 * Current placeholder: SHA-256(passphrase || 0x01 || user_id).
 * Deterministic by design for the test suite; NOT safe against dictionary
 * attacks on a real passphrase. */
static void kdf_passphrase(const char *pp, size_t pp_len,
                            const char *user_id,
                            uint8_t out[KS_KEY_LEN]) {
    size_t uid_len = strlen(user_id);
    /* passphrase + domain separator byte + user_id */
    size_t total = pp_len + 1 + uid_len;
    uint8_t *buf = malloc(total);
    if (!buf) {
        memset(out, 0, KS_KEY_LEN);
        return;
    }
    memcpy(buf, pp, pp_len);
    buf[pp_len] = 0x01; /* domain separator */
    memcpy(buf + pp_len + 1, user_id, uid_len);
    hu_sha256(buf, total, out);
    ks_secure_zero(buf, total);
    free(buf);
}

/* Per-table data key: HMAC-SHA256(master_key, table_name). */
static void derive_data_key(const uint8_t master[KS_KEY_LEN],
                             const char *table_name,
                             uint8_t out[KS_KEY_LEN]) {
    hu_hmac_sha256(master, KS_KEY_LEN,
                   (const uint8_t *)table_name, strlen(table_name),
                   out);
}

/* ── lifecycle ──────────────────────────────────────────────────────────── */

hu_error_t hu_keystore_open(hu_allocator_t *alloc, const char *user_id,
                             hu_keystore_t **out) {
    if (!alloc || !user_id || !*user_id || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (strlen(user_id) >= KS_USER_ID_MAX)
        return HU_ERR_INVALID_ARGUMENT;

    hu_keystore_t *ks = alloc->alloc(alloc->ctx, sizeof(*ks));
    if (!ks)
        return HU_ERR_OUT_OF_MEMORY;

    memset(ks, 0, sizeof(*ks));
    ks->alloc = alloc;
    strncpy(ks->user_id, user_id, KS_USER_ID_MAX - 1);
    *out = ks;
    return HU_OK;
}

void hu_keystore_close(hu_keystore_t *ks, hu_allocator_t *alloc) {
    if (!ks)
        return;
    ks_secure_zero(ks->master_key, KS_KEY_LEN);
    alloc->free(alloc->ctx, ks, sizeof(*ks));
}

hu_error_t hu_keystore_unlock_with_passphrase(hu_keystore_t *ks,
                                               const char *pp, size_t pp_len) {
    if (!ks || !pp || pp_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    /* Check tombstone: key was destroyed, no re-derivation allowed. */
    if (tombstone_exists(ks->user_id))
        return HU_ERR_CRYPTO_DECRYPT;

    /* TODO(W15-secure): replace kdf_passphrase with argon2id. */
    kdf_passphrase(pp, pp_len, ks->user_id, ks->master_key);
    ks->master_key_present = true;
    return HU_OK;
}

hu_error_t hu_keystore_status(hu_keystore_t *ks, hu_keystore_status_t *out) {
    if (!ks || !out)
        return HU_ERR_INVALID_ARGUMENT;
    out->master_key_present = ks->master_key_present;
    out->keychain_available = false; /* OS-keychain not implemented yet */
    out->data_keys_count    = ks->data_keys_count;
    out->key_rotated_at     = 0;
    return HU_OK;
}

/* ── AEAD encrypt / decrypt ─────────────────────────────────────────────── */

/* TODO(W15-secure): replace this section with libsodium XChaCha20-Poly1305:
 *   crypto_aead_xchacha20poly1305_ietf_encrypt / _decrypt.
 * The current implementation uses ChaCha20 + HMAC-SHA256 with a fixed zero
 * nonce. It is deterministic (good for testing) but provides no nonce privacy
 * (unsafe for production where the same key+nonce pair would be reused). */

hu_error_t hu_keystore_encrypt(hu_keystore_t *ks, const char *table_name,
                                const void *plaintext, size_t pt_len,
                                void **out_ciphertext, size_t *out_len) {
    if (!ks || !table_name || (!plaintext && pt_len > 0) || !out_ciphertext || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!ks->master_key_present)
        return HU_ERR_CRYPTO_ENCRYPT;

    uint8_t dk[KS_KEY_LEN];
    derive_data_key(ks->master_key, table_name, dk);

    /* TODO(W15-secure): use a random nonce from libsodium randombytes_buf(). */
    uint8_t nonce[KS_NONCE_LEN] = {0}; /* fixed zero nonce — placeholder */

    size_t total = KS_NONCE_LEN + pt_len + KS_HMAC_LEN;
    uint8_t *ct = ks->alloc->alloc(ks->alloc->ctx, total);
    if (!ct) {
        ks_secure_zero(dk, KS_KEY_LEN);
        return HU_ERR_OUT_OF_MEMORY;
    }

    /* Layout: [nonce:12][ciphertext:pt_len][hmac:32] */
    memcpy(ct, nonce, KS_NONCE_LEN);
    if (pt_len > 0)
        hu_chacha20_encrypt(dk, nonce, 0, plaintext, ct + KS_NONCE_LEN, pt_len);

    /* Authentication tag over nonce || ciphertext. */
    hu_hmac_sha256(dk, KS_KEY_LEN, ct, KS_NONCE_LEN + pt_len,
                   ct + KS_NONCE_LEN + pt_len);

    ks_secure_zero(dk, KS_KEY_LEN);
    ks->data_keys_count++;
    *out_ciphertext = ct;
    *out_len = total;
    return HU_OK;
}

hu_error_t hu_keystore_decrypt(hu_keystore_t *ks, const char *table_name,
                                const void *ciphertext, size_t ct_len,
                                void **out_plaintext, size_t *out_len) {
    if (!ks || !table_name || !ciphertext || !out_plaintext || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!ks->master_key_present)
        return HU_ERR_CRYPTO_DECRYPT;
    if (ct_len < KS_CT_OVERHEAD)
        return HU_ERR_CRYPTO_DECRYPT;

    uint8_t dk[KS_KEY_LEN];
    derive_data_key(ks->master_key, table_name, dk);

    size_t pt_len = ct_len - KS_CT_OVERHEAD;
    const uint8_t *blob = (const uint8_t *)ciphertext;
    const uint8_t *nonce     = blob;
    const uint8_t *encrypted = blob + KS_NONCE_LEN;
    const uint8_t *stored_tag = blob + KS_NONCE_LEN + pt_len;

    /* Verify authentication tag. */
    uint8_t expected_tag[KS_HMAC_LEN];
    hu_hmac_sha256(dk, KS_KEY_LEN, blob, KS_NONCE_LEN + pt_len, expected_tag);

    /* Constant-time comparison. */
    uint8_t diff = 0;
    for (size_t i = 0; i < KS_HMAC_LEN; i++)
        diff |= expected_tag[i] ^ stored_tag[i];

    if (diff) {
        ks_secure_zero(dk, KS_KEY_LEN);
        ks_secure_zero(expected_tag, KS_HMAC_LEN);
        return HU_ERR_CRYPTO_DECRYPT;
    }

    uint8_t *pt = ks->alloc->alloc(ks->alloc->ctx, pt_len + 1);
    if (!pt) {
        ks_secure_zero(dk, KS_KEY_LEN);
        return HU_ERR_OUT_OF_MEMORY;
    }

    if (pt_len > 0)
        hu_chacha20_decrypt(dk, nonce, 0, encrypted, pt, pt_len);
    pt[pt_len] = '\0'; /* null-terminate for convenience */

    ks_secure_zero(dk, KS_KEY_LEN);
    *out_plaintext = pt;
    *out_len = pt_len;
    return HU_OK;
}

/* ── cryptographic forgetting ───────────────────────────────────────────── */

hu_error_t hu_keystore_destroy_master_key(const char *user_id) {
    if (!user_id || !*user_id)
        return HU_ERR_INVALID_ARGUMENT;
    if (strlen(user_id) >= KS_USER_ID_MAX)
        return HU_ERR_INVALID_ARGUMENT;

    char path[KS_PATH_MAX];
    tombstone_path(user_id, path, sizeof(path));

    /* Ensure the directory exists (best-effort; ignore errors). */
    char dir[KS_DIR_MAX];
    keystore_dir(dir, sizeof(dir));
#if !defined(_WIN32) && !defined(_WIN64)
    (void)mkdir(dir, 0700);
#endif

    FILE *f = fopen(path, "w");
    if (!f)
        return HU_ERR_IO;
    fclose(f);
    return HU_OK;
}
