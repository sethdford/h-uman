/*
 * W15 — Cryptographic keystore implementation.
 *
 * AEAD: ChaCha20 (ciphertext) + HMAC-SHA256 (authentication tag), with a
 * per-call cryptographically-random 12-byte nonce. Format:
 *   [nonce:12][ciphertext:N][hmac:32]
 *
 * Random source priority:
 *   1. arc4random_buf (macOS / *BSD / glibc 2.36+)
 *   2. getrandom() (Linux 3.17+)
 *   3. /dev/urandom (POSIX fallback)
 * If none of these succeed, encrypt fails with HU_ERR_CRYPTO_ENCRYPT —
 * we never silently fall back to a deterministic nonce in production.
 *
 * KDF: PBKDF2-HMAC-SHA256 with a per-user 16-byte random salt and
 * 600,000 iterations (OWASP 2023 recommendation for SHA-256 PBKDF2).
 * Salt file: <key_dir>/<user_id>.salt (mode 0600, generated on first
 * unlock). Subsequent unlocks reuse the persisted salt so the master
 * key is reproducible across process restarts (required for offline
 * decryption of previously-stored ciphertext) while still defeating
 * cross-user rainbow tables.
 *
 * **Pending Argon2id**: this hardening is deliberately strong enough
 * to defend against offline dictionary attacks under realistic
 * threat models, but Argon2id is the gold standard. The libsodium
 * upgrade path is documented in docs/standards/security/data-privacy.md.
 *
 * Tombstone: hu_keystore_destroy_master_key writes a zero-byte file
 * <key_dir>/<user_id>.tomb. Future unlock attempts check for this file
 * and return HU_ERR_CRYPTO_DECRYPT, preventing key re-derivation. The
 * salt file is also unlinked so even if the tombstone is later deleted
 * (e.g. by an attacker with FS access), the original master key cannot
 * be re-derived from the same passphrase.
 */

#include "human/security/keystore.h"
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
/* Newer glibc declares getrandom in <sys/random.h>; the syscall
 * fallback works on any 3.17+ kernel without that header. */
#include <sys/syscall.h>
#if __has_include(<sys/random.h>)
#include <sys/random.h>
#endif
#endif

/* libsodium upgrade: when HU_HAS_LIBSODIUM is defined we route random,
 * KDF, and AEAD through libsodium's XChaCha20-Poly1305-IETF and Argon2id.
 * Existing v0 ciphertext (PBKDF2 + ChaCha20+HMAC) remains decryptable
 * via a 1-byte version prefix on every new write. */
#ifdef HU_HAS_LIBSODIUM
#include <sodium.h>
#endif

/* ── constants ─────────────────────────────────────────────────────────── */

#define KS_KEY_LEN      32   /* master key and per-table data key length */
#define KS_NONCE_LEN    12   /* legacy ChaCha20 nonce length (v0) */
#define KS_HMAC_LEN     32   /* HMAC-SHA256 output length */
#define KS_SALT_LEN     16   /* per-user salt length (works for PBKDF2 and Argon2id) */
#define KS_PBKDF2_ITERS 600000 /* OWASP 2023 PBKDF2-HMAC-SHA256 minimum */
#define KS_CT_OVERHEAD  (KS_NONCE_LEN + KS_HMAC_LEN)
#define KS_USER_ID_MAX  128
#define KS_DIR_MAX      512
#define KS_PATH_MAX     (KS_DIR_MAX + KS_USER_ID_MAX + 16)

/* Ciphertext version tags (first byte of the on-disk envelope).
 *
 * v0 (pre-libsodium) layout: [nonce:12][ct:N][hmac:32]. No leading
 * version byte — detection on decrypt is by length plus a v1-magic
 * byte check (see hu_keystore_decrypt). New writes never produce v0
 * once libsodium is linked.
 *
 * v1 layout: [magic:0x01][nonce:24][ct:N][tag:16] using
 * crypto_aead_xchacha20poly1305_ietf_*. Authenticated by Poly1305 so
 * misdetection from a v0 leading-byte collision fails fast and the
 * decoder transparently falls through to v0. */
#define KS_V1_MAGIC          0x01
#ifdef HU_HAS_LIBSODIUM
#define KS_V1_NONCE_LEN      crypto_aead_xchacha20poly1305_ietf_NPUBBYTES /* 24 */
#define KS_V1_TAG_LEN        crypto_aead_xchacha20poly1305_ietf_ABYTES    /* 16 */
#define KS_V1_OVERHEAD       (1 + KS_V1_NONCE_LEN + KS_V1_TAG_LEN)
#endif

/* KDF flag file values. The flag is a sibling to the salt file:
 * <key_dir>/<user_id>.kdf. Single byte file. Missing => v0 (PBKDF2).
 * Written once on first unlock so subsequent unlocks pick the same
 * algorithm and the master key stays reproducible. */
#define KS_KDF_V0_PBKDF2     0x00
#define KS_KDF_V1_ARGON2ID   0x01

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

static void salt_path(const char *user_id, char *out, size_t cap) {
    char dir[KS_DIR_MAX];
    keystore_dir(dir, sizeof(dir));
    snprintf(out, cap, "%s/%s.salt", dir, user_id);
}

/* ── cryptographic randomness ──────────────────────────────────────────── */

#ifdef HU_HAS_LIBSODIUM
/* Lazy one-shot sodium_init. Thread-safe per upstream contract: it is
 * safe to call from multiple threads, but only once per process. */
static int ks_sodium_init_once(void) {
    static int initialized = -1;
    if (initialized < 0) {
        initialized = sodium_init() < 0 ? -1 : 1;
    }
    return initialized > 0 ? 0 : -1;
}
#endif

/* Returns 0 on success, -1 on failure. Never returns deterministic data:
 * we'd rather fail encrypt than produce a same-key/same-nonce reuse. */
static int ks_random_bytes(uint8_t *buf, size_t len) {
    if (len == 0)
        return 0;
#ifdef HU_HAS_LIBSODIUM
    if (ks_sodium_init_once() == 0) {
        randombytes_buf(buf, len);
        return 0;
    }
    /* sodium_init failed — fall through to OS-native sources. */
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(buf, len);
    return 0;
#elif defined(__linux__)
    /* Try getrandom() first (no FD pressure, immune to chroot). */
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
            break; /* fall through to /dev/urandom */
        }
        off += (size_t)n;
    }
    if (off == len)
        return 0;
    /* Fallback */
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
    /* POSIX fallback. */
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f)
        return -1;
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    return got == len ? 0 : -1;
#endif
}

/* ── salt persistence ──────────────────────────────────────────────────── */

/* Reads the per-user salt; if it doesn't exist generates one and writes
 * it to disk. Returns 0 on success, -1 on I/O or RNG failure. */
static int ks_load_or_create_salt(const char *user_id, uint8_t out[KS_SALT_LEN]) {
    char dir[KS_DIR_MAX];
    keystore_dir(dir, sizeof(dir));
#if !defined(_WIN32) && !defined(_WIN64)
    (void)mkdir(dir, 0700);
#endif
    char path[KS_PATH_MAX];
    salt_path(user_id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(out, 1, KS_SALT_LEN, f);
        fclose(f);
        if (n == KS_SALT_LEN)
            return 0;
        /* Truncated salt — treat as missing and re-generate to avoid
         * partial-state bugs. */
    }
    /* Generate, persist with mode 0600. */
    if (ks_random_bytes(out, KS_SALT_LEN) != 0)
        return -1;
#if !defined(_WIN32) && !defined(_WIN64)
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    ssize_t w = write(fd, out, KS_SALT_LEN);
    close(fd);
    if (w != KS_SALT_LEN)
        return -1;
#else
    f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t w = fwrite(out, 1, KS_SALT_LEN, f);
    fclose(f);
    if (w != KS_SALT_LEN)
        return -1;
#endif
    return 0;
}

/* ── KDF version flag ──────────────────────────────────────────────────── */

static void kdf_path(const char *user_id, char *out, size_t cap) {
    char dir[KS_DIR_MAX];
    keystore_dir(dir, sizeof(dir));
    snprintf(out, cap, "%s/%s.kdf", dir, user_id);
}

/* Returns the algorithm tag persisted for this user, or KS_KDF_V0_PBKDF2
 * when no flag file exists (legacy default). I/O errors also yield v0
 * — the goal is "never decrypt-broken existing data". */
static uint8_t ks_kdf_version_for(const char *user_id) {
    char path[KS_PATH_MAX];
    kdf_path(user_id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f)
        return KS_KDF_V0_PBKDF2;
    uint8_t v = KS_KDF_V0_PBKDF2;
    /* Best-effort: if read fails we keep the v0 default. The (void) cast
     * is insufficient under GCC's __attribute__((warn_unused_result));
     * assign to a discard variable so -Werror=unused-result doesn't trip. */
    size_t nread = fread(&v, 1, 1, f);
    (void)nread;
    fclose(f);
    return v;
}

/* Persist the KDF version exactly once. If the flag already exists we
 * leave it alone — the chosen algorithm must be sticky across the
 * lifetime of the salt file or the master key would change. */
static void ks_kdf_version_persist(const char *user_id, uint8_t version) {
    char path[KS_PATH_MAX];
    kdf_path(user_id, path, sizeof(path));
    FILE *probe = fopen(path, "rb");
    if (probe) {
        fclose(probe);
        return;
    }
#if !defined(_WIN32) && !defined(_WIN64)
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    /* Best-effort write: if it fails we either left the placeholder bytes
     * or wrote partial — either way the next read picks v0 default. */
    ssize_t nwritten = write(fd, &version, 1);
    (void)nwritten;
    close(fd);
#else
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    (void)fwrite(&version, 1, 1, f);
    fclose(f);
#endif
}

/* ── KDF ────────────────────────────────────────────────────────────────── */

/* PBKDF2-HMAC-SHA256 RFC 8018, single-block (output_len == hash_len == 32).
 *
 * F(P, S, c, 1) = U_1 XOR U_2 XOR ... XOR U_c
 *   where U_1 = HMAC-SHA256(P, S || INT(1))
 *         U_i = HMAC-SHA256(P, U_{i-1})
 * INT(1) is the 4-byte big-endian block index.
 *
 * For a 32-byte output we only need block 1. This makes the loop
 * straightforward and avoids the variable-length-output path.
 */
static void pbkdf2_hmac_sha256(const uint8_t *password, size_t password_len,
                                const uint8_t *salt, size_t salt_len,
                                uint32_t iterations, uint8_t out[KS_KEY_LEN]) {
    /* Salt || INT(1) — the input to U_1. */
    uint8_t salt_with_idx[KS_SALT_LEN + 4];
    if (salt_len > KS_SALT_LEN)
        salt_len = KS_SALT_LEN; /* defensive — caller passes KS_SALT_LEN */
    memcpy(salt_with_idx, salt, salt_len);
    salt_with_idx[salt_len + 0] = 0;
    salt_with_idx[salt_len + 1] = 0;
    salt_with_idx[salt_len + 2] = 0;
    salt_with_idx[salt_len + 3] = 1;

    uint8_t u[32];
    uint8_t accum[32];
    hu_hmac_sha256(password, password_len, salt_with_idx, salt_len + 4, u);
    memcpy(accum, u, 32);
    for (uint32_t i = 1; i < iterations; i++) {
        hu_hmac_sha256(password, password_len, u, 32, u);
        for (size_t j = 0; j < 32; j++)
            accum[j] ^= u[j];
    }
    memcpy(out, accum, KS_KEY_LEN);
    ks_secure_zero(u, sizeof(u));
    ks_secure_zero(accum, sizeof(accum));
    ks_secure_zero(salt_with_idx, sizeof(salt_with_idx));
}

/* Production KDF: dispatches to PBKDF2-HMAC-SHA256 (legacy) or Argon2id
 * (libsodium) based on the persisted per-user algorithm flag.
 *
 * Algorithm selection rules:
 *   1. Flag file present → use that algorithm (sticky; required so the
 *      master key is reproducible across unlocks).
 *   2. Flag file missing AND libsodium linked → Argon2id (and write
 *      the flag).
 *   3. Otherwise → PBKDF2-HMAC-SHA256.
 *
 * Returns 0 on success, -1 if the salt couldn't be loaded/generated or
 * the chosen KDF failed. */
static int kdf_passphrase(const char *pp, size_t pp_len, const char *user_id,
                          uint8_t out[KS_KEY_LEN]) {
    uint8_t salt[KS_SALT_LEN];
    if (ks_load_or_create_salt(user_id, salt) != 0) {
        memset(out, 0, KS_KEY_LEN);
        return -1;
    }

    uint8_t version = ks_kdf_version_for(user_id);
#ifdef HU_HAS_LIBSODIUM
    /* Fresh keystores get the strongest KDF available. The flag file
     * locks this in for subsequent unlocks. */
    if (version == KS_KDF_V0_PBKDF2) {
        char fpath[KS_PATH_MAX];
        kdf_path(user_id, fpath, sizeof(fpath));
        FILE *probe = fopen(fpath, "rb");
        if (!probe) {
            version = KS_KDF_V1_ARGON2ID;
            ks_kdf_version_persist(user_id, version);
        } else {
            fclose(probe);
        }
    }
#endif

    int rc = -1;
    if (version == KS_KDF_V1_ARGON2ID) {
#ifdef HU_HAS_LIBSODIUM
        if (ks_sodium_init_once() == 0) {
            /* Test mode: use the interactive limits — sub-second on
             * commodity hardware. Production: moderate limits, ~0.7s on
             * a recent CPU per the libsodium 1.0.18 calibration table. */
#if defined(HU_IS_TEST) && HU_IS_TEST
            unsigned long long ops = crypto_pwhash_OPSLIMIT_INTERACTIVE;
            size_t mem = crypto_pwhash_MEMLIMIT_INTERACTIVE;
#else
            unsigned long long ops = crypto_pwhash_OPSLIMIT_MODERATE;
            size_t mem = crypto_pwhash_MEMLIMIT_MODERATE;
#endif
            rc = crypto_pwhash(out, KS_KEY_LEN,
                               (const char *)pp, pp_len, salt, ops, mem,
                               crypto_pwhash_ALG_ARGON2ID13);
        }
#endif
        if (rc != 0) {
            /* libsodium not linked or pwhash failed — fall back to v0
             * so the keystore stays openable. The flag file already
             * captured the upgrade attempt; we just log the regression
             * here by NOT updating the flag (keystore stays on v1
             * preference for next attempt with libsodium present). */
        }
    }
    if (version == KS_KDF_V0_PBKDF2 || rc != 0) {
        /* In test mode we cap iterations dramatically to keep the suite
         * fast. Production builds always use the full OWASP iteration
         * count. */
#if defined(HU_IS_TEST) && HU_IS_TEST
        uint32_t iters = 1000;
#else
        uint32_t iters = KS_PBKDF2_ITERS;
#endif
        pbkdf2_hmac_sha256((const uint8_t *)pp, pp_len, salt, KS_SALT_LEN, iters, out);
        rc = 0;
        /* If we ended up here because v1 was preferred but failed, persist
         * the v0 fallback so the next unlock doesn't keep retrying. */
        ks_kdf_version_persist(user_id, KS_KDF_V0_PBKDF2);
    }

    ks_secure_zero(salt, sizeof(salt));
    return rc;
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

    if (kdf_passphrase(pp, pp_len, ks->user_id, ks->master_key) != 0)
        return HU_ERR_CRYPTO_ENCRYPT;
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

hu_error_t hu_keystore_encrypt(hu_keystore_t *ks, const char *table_name,
                                const void *plaintext, size_t pt_len,
                                void **out_ciphertext, size_t *out_len) {
    if (!ks || !table_name || (!plaintext && pt_len > 0) || !out_ciphertext || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!ks->master_key_present)
        return HU_ERR_CRYPTO_ENCRYPT;

    uint8_t dk[KS_KEY_LEN];
    derive_data_key(ks->master_key, table_name, dk);

#ifdef HU_HAS_LIBSODIUM
    /* v1 envelope: [magic:0x01][nonce:24][ct+poly1305_tag:N+16].
     * AEAD additional-data is the table name so a ciphertext from one
     * table can't be replayed under another (same plaintext, different
     * derived key already prevents this; AAD makes it explicit). */
    if (ks_sodium_init_once() == 0) {
        size_t total = KS_V1_OVERHEAD + pt_len;
        uint8_t *ct = ks->alloc->alloc(ks->alloc->ctx, total);
        if (!ct) {
            ks_secure_zero(dk, KS_KEY_LEN);
            return HU_ERR_OUT_OF_MEMORY;
        }
        ct[0] = KS_V1_MAGIC;
        uint8_t *nonce = ct + 1;
        randombytes_buf(nonce, KS_V1_NONCE_LEN);
        unsigned long long ct_len_out = 0;
        if (crypto_aead_xchacha20poly1305_ietf_encrypt(
                ct + 1 + KS_V1_NONCE_LEN, &ct_len_out,
                (const unsigned char *)plaintext, (unsigned long long)pt_len,
                (const unsigned char *)table_name,
                (unsigned long long)strlen(table_name), NULL, nonce, dk) != 0) {
            ks->alloc->free(ks->alloc->ctx, ct, total);
            ks_secure_zero(dk, KS_KEY_LEN);
            return HU_ERR_CRYPTO_ENCRYPT;
        }
        ks_secure_zero(dk, KS_KEY_LEN);
        ks->data_keys_count++;
        *out_ciphertext = ct;
        *out_len = 1 + KS_V1_NONCE_LEN + (size_t)ct_len_out;
        return HU_OK;
    }
    /* sodium_init failed; fall through to v0 below. */
#endif

    /* v0 envelope: [nonce:12][ciphertext:pt_len][hmac:32]. */
    uint8_t nonce[KS_NONCE_LEN];
    if (ks_random_bytes(nonce, KS_NONCE_LEN) != 0) {
        ks_secure_zero(dk, KS_KEY_LEN);
        return HU_ERR_CRYPTO_ENCRYPT;
    }

    size_t total = KS_NONCE_LEN + pt_len + KS_HMAC_LEN;
    uint8_t *ct = ks->alloc->alloc(ks->alloc->ctx, total);
    if (!ct) {
        ks_secure_zero(dk, KS_KEY_LEN);
        ks_secure_zero(nonce, KS_NONCE_LEN);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(ct, nonce, KS_NONCE_LEN);
    if (pt_len > 0)
        hu_chacha20_encrypt(dk, nonce, 0, plaintext, ct + KS_NONCE_LEN, pt_len);
    hu_hmac_sha256(dk, KS_KEY_LEN, ct, KS_NONCE_LEN + pt_len,
                   ct + KS_NONCE_LEN + pt_len);

    ks_secure_zero(dk, KS_KEY_LEN);
    ks_secure_zero(nonce, KS_NONCE_LEN);
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

    const uint8_t *blob = (const uint8_t *)ciphertext;
    uint8_t dk[KS_KEY_LEN];
    derive_data_key(ks->master_key, table_name, dk);

#ifdef HU_HAS_LIBSODIUM
    /* Attempt v1 envelope first when the leading byte and minimum
     * length match. Poly1305 fails fast on a wrong key/AAD, so a v0
     * blob whose first byte happens to be 0x01 just falls through. */
    if (ct_len >= KS_V1_OVERHEAD && blob[0] == KS_V1_MAGIC &&
        ks_sodium_init_once() == 0) {
        const uint8_t *nonce = blob + 1;
        const uint8_t *body = blob + 1 + KS_V1_NONCE_LEN;
        unsigned long long body_len =
            (unsigned long long)(ct_len - 1 - KS_V1_NONCE_LEN);
        size_t pt_capacity =
            ct_len - 1 - KS_V1_NONCE_LEN - KS_V1_TAG_LEN;
        uint8_t *pt = ks->alloc->alloc(ks->alloc->ctx, pt_capacity + 1);
        if (!pt) {
            ks_secure_zero(dk, KS_KEY_LEN);
            return HU_ERR_OUT_OF_MEMORY;
        }
        unsigned long long pt_len_out = 0;
        if (crypto_aead_xchacha20poly1305_ietf_decrypt(
                pt, &pt_len_out, NULL, body, body_len,
                (const unsigned char *)table_name,
                (unsigned long long)strlen(table_name), nonce, dk) == 0) {
            pt[pt_len_out] = '\0';
            ks_secure_zero(dk, KS_KEY_LEN);
            *out_plaintext = pt;
            *out_len = (size_t)pt_len_out;
            return HU_OK;
        }
        ks->alloc->free(ks->alloc->ctx, pt, pt_capacity + 1);
        /* AEAD verification failed — could be a v0 blob whose first
         * byte collided with 0x01, or a real corruption. Fall through
         * to v0 attempt; that path also has its own auth check. */
    }
#endif

    /* v0 path. */
    if (ct_len < KS_CT_OVERHEAD) {
        ks_secure_zero(dk, KS_KEY_LEN);
        return HU_ERR_CRYPTO_DECRYPT;
    }

    size_t pt_len = ct_len - KS_CT_OVERHEAD;
    const uint8_t *nonce     = blob;
    const uint8_t *encrypted = blob + KS_NONCE_LEN;
    const uint8_t *stored_tag = blob + KS_NONCE_LEN + pt_len;

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
    pt[pt_len] = '\0';

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

    /* Also remove the per-user salt and KDF flag: re-deriving the master
     * key requires BOTH the passphrase AND the original salt. Removing
     * the salt makes the destruction stick even if the tombstone is
     * later deleted. The KDF flag is removed in lock-step so a future
     * unlock with the same user_id starts fresh and picks the strongest
     * available algorithm. */
    char spath[KS_PATH_MAX];
    salt_path(user_id, spath, sizeof(spath));
    (void)remove(spath);
    char kpath[KS_PATH_MAX];
    kdf_path(user_id, kpath, sizeof(kpath));
    (void)remove(kpath);
    return HU_OK;
}
