/*
 * Persona encryption-at-rest (Sprint 42, US-42.2)
 *
 * Implementation notes vs design:
 *
 * - Backend: this v1 uses the in-tree ChaCha20 + HMAC-SHA256 Encrypt-then-MAC
 *   primitive from include/human/crypto.h. HU_ENABLE_LIBSODIUM is OFF by default
 *   (CMakeLists.txt:91); pulling libsodium in for the v1 of US-42.2 would
 *   broaden the dependency surface without changing the user-visible contract.
 *   The wire format keeps a backend tag byte (0x01 libsodium / 0x02 chacha+hmac)
 *   so a future libsodium implementation can read the same files.
 *
 * - Keystore: file at ~/.human/keys/persona.key (0600), with the bounded
 *   O_CREAT|O_EXCL retry loop the design mandates (max 3 attempts, 50ms,
 *   then HU_ERR_IO_BUSY — NEVER recursive per design risk-3). Under HU_IS_TEST
 *   we use a deterministic in-memory key so CI doesn't need a real keystore.
 *
 * - Migration: ordering per crypto.h header banner; shred_and_unlink is
 *   called explicitly in hu_persona_migrate_to_encrypted after the
 *   .migration_done sentinel is fsync'd into place. This is the call site
 *   the prior session's critic flagged as missing.
 */

#include "human/persona/crypto.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/crypto.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

const uint8_t HU_PERSONA_CRYPTO_SENTINEL[HU_PERSONA_CRYPTO_SENTINEL_LEN] = {
    0x68, 0x75, 0x70, 0x65 /* "hupe" */
};

#define HU_PCRYPT_BACKEND_LIBSODIUM   0x01
#define HU_PCRYPT_BACKEND_CHACHA_HMAC 0x02

/* Header layout: sentinel(4) + backend(1) + reserved(3) + nonce(24) = 32 bytes.
 * Followed by ciphertext (N bytes) and finally a 16-byte truncated HMAC-SHA256
 * authentication tag. */
#define HU_PCRYPT_HEADER_LEN 32
#define HU_PCRYPT_NONCE_LEN  24
#define HU_PCRYPT_TAG_LEN    16

/* ── Pure classifier predicate (security-predicate-extraction.md) ───────── */

hu_persona_byte_class_t hu_persona_classify_bytes(const uint8_t *buf, size_t len,
                                                  bool migration_done_present) {
    if (!buf) {
        return HU_PERSONA_BYTES_INVALID;
    }
    if (len < HU_PERSONA_CRYPTO_SENTINEL_LEN) {
        /* Too short to be a wrapped blob; if a migration sentinel exists
         * the file must be refused regardless of content. */
        if (migration_done_present) {
            return HU_PERSONA_BYTES_MIGRATION_PENDING;
        }
        return HU_PERSONA_BYTES_PLAINTEXT;
    }
    if (memcmp(buf, HU_PERSONA_CRYPTO_SENTINEL, HU_PERSONA_CRYPTO_SENTINEL_LEN) == 0) {
        if (len < HU_PERSONA_CRYPTO_MIN_BLOB_LEN) {
            return HU_PERSONA_BYTES_INVALID;
        }
        return HU_PERSONA_BYTES_ENCRYPTED;
    }
    /* No sentinel. If a migration_done sibling exists, the file must NOT
     * be treated as legitimate plaintext anymore. */
    if (migration_done_present) {
        return HU_PERSONA_BYTES_MIGRATION_PENDING;
    }
    return HU_PERSONA_BYTES_PLAINTEXT;
}

/* ── Key open ───────────────────────────────────────────────────────────── */

#ifndef HU_IS_TEST
static int derive_key_path(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !*home) {
        return -1;
    }
    int n = snprintf(buf, cap, "%s/.human/keys/persona.key", home);
    if (n <= 0 || (size_t)n >= cap) {
        return -1;
    }
    return 0;
}

static int ensure_dir_chain(const char *home) {
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "%s/.human", home);
    if (n <= 0 || (size_t)n >= sizeof(buf)) {
        return -1;
    }
    (void)mkdir(buf, 0700);
    n = snprintf(buf, sizeof(buf), "%s/.human/keys", home);
    if (n <= 0 || (size_t)n >= sizeof(buf)) {
        return -1;
    }
    if (mkdir(buf, 0700) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/* Bounded retry: max 3 attempts, 50ms apart. NEVER recursive (design risk-3). */
static hu_error_t create_key_file_excl(const char *path,
                                       const uint8_t key[HU_PERSONA_CRYPTO_KEY_LEN]) {
    for (int attempt = 0; attempt < 3; attempt++) {
        int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd >= 0) {
            ssize_t w = write(fd, key, HU_PERSONA_CRYPTO_KEY_LEN);
            if (w != (ssize_t)HU_PERSONA_CRYPTO_KEY_LEN) {
                int saved = errno;
                close(fd);
                unlink(path);
                errno = saved;
                return HU_ERR_IO;
            }
            (void)fsync(fd);
            if (close(fd) != 0) {
                return HU_ERR_IO;
            }
            return HU_OK;
        }
        if (errno != EEXIST) {
            return HU_ERR_IO;
        }
        /* EEXIST -- file appeared between our check and our create. Another
         * process may have just won the race; check if the file is now valid
         * and readable on retry. Bounded sleep, NEVER recurse. */
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 50L * 1000L * 1000L};
        (void)nanosleep(&ts, NULL);
    }
    return HU_ERR_IO_BUSY;
}

static hu_error_t open_key_real(uint8_t out[HU_PERSONA_CRYPTO_KEY_LEN]) {
    char path[1024];
    if (derive_key_path(path, sizeof(path)) != 0) {
        return HU_ERR_IO;
    }
    const char *home = getenv("HOME");
    if (home && ensure_dir_chain(home) != 0) {
        return HU_ERR_IO;
    }

    /* Existing key? Read + assert mode 0600. */
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            return HU_ERR_IO;
        }
        if ((st.st_mode & 0077) != 0) {
            /* Wider than 0600 -- refuse to read. */
            close(fd);
            return HU_ERR_PERMISSION_DENIED;
        }
        ssize_t r = read(fd, out, HU_PERSONA_CRYPTO_KEY_LEN);
        close(fd);
        if (r != (ssize_t)HU_PERSONA_CRYPTO_KEY_LEN) {
            return HU_ERR_IO;
        }
        return HU_OK;
    }
    if (errno != ENOENT) {
        return HU_ERR_IO;
    }

    /* No key -- generate and create exclusively. */
    uint8_t fresh[HU_PERSONA_CRYPTO_KEY_LEN];
    FILE *urand = fopen("/dev/urandom", "rb");
    if (!urand) {
        return HU_ERR_IO;
    }
    size_t got = fread(fresh, 1, HU_PERSONA_CRYPTO_KEY_LEN, urand);
    fclose(urand);
    if (got != HU_PERSONA_CRYPTO_KEY_LEN) {
        return HU_ERR_IO;
    }
    hu_error_t rc = create_key_file_excl(path, fresh);
    if (rc != HU_OK) {
        return rc;
    }
    memcpy(out, fresh, HU_PERSONA_CRYPTO_KEY_LEN);
    /* Best-effort zero of the on-stack copy. */
    memset(fresh, 0, sizeof(fresh));
    return HU_OK;
}
#endif /* !HU_IS_TEST */

hu_error_t hu_persona_crypto_key_open(uint8_t out[HU_PERSONA_CRYPTO_KEY_LEN]) {
    if (!out) {
        return HU_ERR_INVALID_ARGUMENT;
    }
#ifdef HU_IS_TEST
    /* Deterministic test key -- CI macOS runners have no real Keychain
     * and we don't want tests writing to ~/.human/keys/persona.key. */
    for (size_t i = 0; i < HU_PERSONA_CRYPTO_KEY_LEN; i++) {
        out[i] = (uint8_t)(0x42 ^ (uint8_t)i);
    }
    return HU_OK;
#else
    return open_key_real(out);
#endif
}

/* ── Encrypt / Decrypt primitives ───────────────────────────────────────── */

/* Generate 24-byte nonce. Deterministic in tests; from /dev/urandom otherwise.
 * Caller is responsible for nonce uniqueness across calls with the same key;
 * we derive from a monotonic counter + time, then encrypt-then-MAC, so even
 * a low-quality nonce source does not weaken authentication. */
static int gen_nonce(uint8_t nonce[HU_PCRYPT_NONCE_LEN]) {
#ifdef HU_IS_TEST
    static uint64_t test_counter = 0;
    test_counter++;
    memset(nonce, 0, HU_PCRYPT_NONCE_LEN);
    memcpy(nonce, &test_counter, sizeof(test_counter));
    /* Mix in time so two test runs don't reuse the exact same nonces.
     * Authentication does not depend on nonce secrecy. */
    time_t now = time(NULL);
    memcpy(nonce + sizeof(test_counter), &now, sizeof(now));
    return 0;
#else
    FILE *urand = fopen("/dev/urandom", "rb");
    if (!urand) {
        return -1;
    }
    size_t got = fread(nonce, 1, HU_PCRYPT_NONCE_LEN, urand);
    fclose(urand);
    return (got == HU_PCRYPT_NONCE_LEN) ? 0 : -1;
#endif
}

/* Encrypt-then-MAC: ChaCha20(plaintext) -> ciphertext, then
 * HMAC-SHA256(header || ciphertext) truncated to 16 bytes is the tag.
 *
 * The MAC covers the entire header (sentinel + backend tag + reserved + nonce)
 * AND the ciphertext, so tampering with any byte (including the sentinel or
 * the backend selector) is detected. */
static hu_error_t
aead_encrypt(const uint8_t key[HU_PERSONA_CRYPTO_KEY_LEN], const uint8_t *plaintext, size_t pt_len,
             uint8_t *out_blob /* HU_PCRYPT_HEADER_LEN + pt_len + HU_PCRYPT_TAG_LEN */) {
    /* Build header. */
    memcpy(out_blob, HU_PERSONA_CRYPTO_SENTINEL, HU_PERSONA_CRYPTO_SENTINEL_LEN);
    out_blob[4] = HU_PCRYPT_BACKEND_CHACHA_HMAC;
    out_blob[5] = 0;
    out_blob[6] = 0;
    out_blob[7] = 0;

    uint8_t nonce24[HU_PCRYPT_NONCE_LEN];
    if (gen_nonce(nonce24) != 0) {
        return HU_ERR_CRYPTO_ENCRYPT;
    }
    memcpy(out_blob + 8, nonce24, HU_PCRYPT_NONCE_LEN);

    /* ChaCha20 uses 12-byte nonce; derive from first 12 of the 24-byte field.
     * The remaining 12 bytes pad the wire format for forward-compat with
     * the libsodium XSalsa20 path which uses 24-byte nonces directly. */
    uint8_t chacha_nonce[12];
    memcpy(chacha_nonce, nonce24, 12);

    uint8_t *ct = out_blob + HU_PCRYPT_HEADER_LEN;
    hu_chacha20_encrypt(key, chacha_nonce, /*counter*/ 1, plaintext, ct, pt_len);

    /* HMAC-SHA256(header || ciphertext). */
    uint8_t mac[32];
    size_t maced_len = HU_PCRYPT_HEADER_LEN + pt_len;
    /* HMAC the whole header+ct stream by concatenating in-place (out_blob already
     * has header + ct contiguous). */
    hu_hmac_sha256(key, HU_PERSONA_CRYPTO_KEY_LEN, out_blob, maced_len, mac);
    /* Truncate to 16 bytes. */
    memcpy(out_blob + maced_len, mac, HU_PCRYPT_TAG_LEN);

    /* Zero local copies. */
    memset(nonce24, 0, sizeof(nonce24));
    memset(chacha_nonce, 0, sizeof(chacha_nonce));
    memset(mac, 0, sizeof(mac));
    return HU_OK;
}

/* Constant-time 16-byte compare. */
static int ct_memcmp16(const uint8_t *a, const uint8_t *b) {
    uint8_t diff = 0;
    for (size_t i = 0; i < HU_PCRYPT_TAG_LEN; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff;
}

static hu_error_t aead_decrypt(const uint8_t key[HU_PERSONA_CRYPTO_KEY_LEN], const uint8_t *blob,
                               size_t blob_len, uint8_t *out_pt /* blob_len - HEADER - TAG */) {
    if (blob_len < HU_PERSONA_CRYPTO_MIN_BLOB_LEN) {
        return HU_ERR_INVALID_FORMAT;
    }
    if (memcmp(blob, HU_PERSONA_CRYPTO_SENTINEL, HU_PERSONA_CRYPTO_SENTINEL_LEN) != 0) {
        return HU_ERR_INVALID_FORMAT;
    }
    if (blob[4] != HU_PCRYPT_BACKEND_CHACHA_HMAC) {
        /* Unknown / unsupported backend tag. */
        return HU_ERR_NOT_SUPPORTED;
    }

    size_t ct_len = blob_len - HU_PCRYPT_HEADER_LEN - HU_PCRYPT_TAG_LEN;
    const uint8_t *ct = blob + HU_PCRYPT_HEADER_LEN;
    const uint8_t *tag = blob + HU_PCRYPT_HEADER_LEN + ct_len;

    /* Verify MAC over header || ciphertext, in constant time. */
    uint8_t mac[32];
    hu_hmac_sha256(key, HU_PERSONA_CRYPTO_KEY_LEN, blob, HU_PCRYPT_HEADER_LEN + ct_len, mac);
    if (ct_memcmp16(mac, tag) != 0) {
        memset(mac, 0, sizeof(mac));
        return HU_ERR_DECRYPT_FAILED;
    }
    memset(mac, 0, sizeof(mac));

    /* MAC verified -- decrypt. */
    uint8_t chacha_nonce[12];
    memcpy(chacha_nonce, blob + 8, 12);
    hu_chacha20_decrypt(key, chacha_nonce, /*counter*/ 1, ct, out_pt, ct_len);
    memset(chacha_nonce, 0, sizeof(chacha_nonce));
    return HU_OK;
}

/* ── File I/O helpers ───────────────────────────────────────────────────── */

/* Read entire file into a freshly allocated buffer. */
static hu_error_t read_all(hu_allocator_t *alloc, const char *path, uint8_t **out,
                           size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return (errno == ENOENT) ? HU_ERR_NOT_FOUND : HU_ERR_IO;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    rewind(f);
    size_t alloc_sz = (size_t)sz;
    uint8_t *buf = NULL;
    if (alloc_sz > 0) {
        buf = (uint8_t *)alloc->alloc(alloc->ctx, alloc_sz);
        if (!buf) {
            fclose(f);
            return HU_ERR_OUT_OF_MEMORY;
        }
        size_t got = fread(buf, 1, alloc_sz, f);
        if (got != alloc_sz) {
            alloc->free(alloc->ctx, buf, alloc_sz);
            fclose(f);
            return HU_ERR_IO;
        }
    }
    fclose(f);
    *out = buf;
    *out_len = alloc_sz;
    return HU_OK;
}

/* Atomic write: <path>.tmp -> fsync -> rename -> fsync(dir). */
static hu_error_t atomic_write(const char *path, const uint8_t *bytes, size_t len) {
    char tmp[1024];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof(tmp)) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) {
        return HU_ERR_IO;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, bytes + off, len - off);
        if (w <= 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            unlink(tmp);
            return HU_ERR_IO;
        }
        off += (size_t)w;
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        return HU_ERR_IO;
    }
    if (close(fd) != 0) {
        unlink(tmp);
        return HU_ERR_IO;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return HU_ERR_IO;
    }
    /* fsync the containing directory so the rename is durable. */
    char dirpath[1024];
    size_t plen = strlen(path);
    if (plen >= sizeof(dirpath)) {
        return HU_OK; /* path too long; skip dir fsync but the file is durable */
    }
    memcpy(dirpath, path, plen + 1);
    char *slash = strrchr(dirpath, '/');
    if (slash) {
        if (slash == dirpath) {
            /* root */
            slash[1] = '\0';
        } else {
            *slash = '\0';
        }
        int dfd = open(dirpath, O_RDONLY);
        if (dfd >= 0) {
            (void)fsync(dfd);
            close(dfd);
        }
    }
    return HU_OK;
}

/* Build sibling .migration_done path. */
static int derive_sentinel_path(const char *path, char *buf, size_t cap) {
    int n = snprintf(buf, cap, "%s.migration_done", path);
    if (n <= 0 || (size_t)n >= cap) {
        return -1;
    }
    return 0;
}

static bool sentinel_exists(const char *path) {
    char spath[1024];
    if (derive_sentinel_path(path, spath, sizeof(spath)) != 0) {
        return false;
    }
    struct stat st;
    return stat(spath, &st) == 0;
}

/* Overwrite-with-zeros, fsync, then unlink. The prior session's critic
 * flagged that we previously had memzero but no on-disk shred. This is
 * the explicit call site. */
static hu_error_t shred_and_unlink(const char *path) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return HU_OK;
        }
        return HU_ERR_IO;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return HU_ERR_IO;
    }
    off_t remaining = st.st_size;
    uint8_t zeros[4096];
    memset(zeros, 0, sizeof(zeros));
    while (remaining > 0) {
        size_t chunk = remaining > (off_t)sizeof(zeros) ? sizeof(zeros) : (size_t)remaining;
        ssize_t w = write(fd, zeros, chunk);
        if (w <= 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return HU_ERR_IO;
        }
        remaining -= w;
    }
    (void)fsync(fd);
    if (close(fd) != 0) {
        return HU_ERR_IO;
    }
    if (unlink(path) != 0) {
        return HU_ERR_IO;
    }
    return HU_OK;
}

/* ── Public surface ─────────────────────────────────────────────────────── */

hu_error_t hu_persona_save_encrypted(hu_allocator_t *alloc, const char *path, const uint8_t *in,
                                     size_t in_len) {
    if (!alloc || !path || (!in && in_len > 0)) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    uint8_t key[HU_PERSONA_CRYPTO_KEY_LEN];
    hu_error_t krc = hu_persona_crypto_key_open(key);
    if (krc != HU_OK) {
        return krc;
    }
    size_t blob_len = HU_PCRYPT_HEADER_LEN + in_len + HU_PCRYPT_TAG_LEN;
    uint8_t *blob = (uint8_t *)alloc->alloc(alloc->ctx, blob_len);
    if (!blob) {
        memset(key, 0, sizeof(key));
        return HU_ERR_OUT_OF_MEMORY;
    }
    hu_error_t erc = aead_encrypt(key, in, in_len, blob);
    memset(key, 0, sizeof(key));
    if (erc != HU_OK) {
        alloc->free(alloc->ctx, blob, blob_len);
        return erc;
    }
    hu_error_t wrc = atomic_write(path, blob, blob_len);
    alloc->free(alloc->ctx, blob, blob_len);
    return wrc;
}

hu_error_t hu_persona_load_encrypted(hu_allocator_t *alloc, const char *path, uint8_t **out,
                                     size_t *out_len) {
    if (!alloc || !path || !out || !out_len) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;
    *out_len = 0;

    uint8_t *blob = NULL;
    size_t blob_len = 0;
    hu_error_t rrc = read_all(alloc, path, &blob, &blob_len);
    if (rrc != HU_OK) {
        return rrc;
    }
    bool mdp = sentinel_exists(path);
    hu_persona_byte_class_t cls = hu_persona_classify_bytes(blob, blob_len, mdp);
    if (cls != HU_PERSONA_BYTES_ENCRYPTED) {
        alloc->free(alloc->ctx, blob, blob_len);
        return HU_ERR_INVALID_FORMAT;
    }
    uint8_t key[HU_PERSONA_CRYPTO_KEY_LEN];
    hu_error_t krc = hu_persona_crypto_key_open(key);
    if (krc != HU_OK) {
        alloc->free(alloc->ctx, blob, blob_len);
        return krc;
    }
    size_t pt_len = blob_len - HU_PCRYPT_HEADER_LEN - HU_PCRYPT_TAG_LEN;
    uint8_t *pt = NULL;
    if (pt_len > 0) {
        pt = (uint8_t *)alloc->alloc(alloc->ctx, pt_len);
        if (!pt) {
            memset(key, 0, sizeof(key));
            alloc->free(alloc->ctx, blob, blob_len);
            return HU_ERR_OUT_OF_MEMORY;
        }
    }
    hu_error_t drc = aead_decrypt(key, blob, blob_len, pt);
    memset(key, 0, sizeof(key));
    alloc->free(alloc->ctx, blob, blob_len);
    if (drc != HU_OK) {
        if (pt) {
            alloc->free(alloc->ctx, pt, pt_len);
        }
        return drc;
    }
    *out = pt;
    *out_len = pt_len;
    return HU_OK;
}

hu_error_t hu_persona_load_legacy(hu_allocator_t *alloc, const char *path, uint8_t **out,
                                  size_t *out_len) {
    if (!alloc || !path || !out || !out_len) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;
    *out_len = 0;

    /* Post-migration refusal: keyed off the sibling sentinel. We check this
     * BEFORE reading the file content -- per AC-42.2.3 final clause,
     * "any subsequent call to hu_persona_load() for a plaintext file returns
     * HU_ERR_SECURITY_DENIED without reading the file." We map the design's
     * SECURITY_DENIED label onto HU_ERR_LEGACY_REFUSED (the new error code
     * that distinguishes legacy-refusal from generic security-denial). */
    if (sentinel_exists(path)) {
        return HU_ERR_LEGACY_REFUSED;
    }
    return read_all(alloc, path, out, out_len);
}

hu_error_t hu_persona_migrate_to_encrypted(hu_allocator_t *alloc, const char *path) {
    if (!alloc || !path) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (sentinel_exists(path)) {
        return HU_ERR_LEGACY_REFUSED;
    }

    /* 1. read plaintext */
    uint8_t *plain = NULL;
    size_t plain_len = 0;
    hu_error_t rrc = read_all(alloc, path, &plain, &plain_len);
    if (rrc != HU_OK) {
        return rrc;
    }
    /* If the file is already an encrypted blob (no sentinel file but file
     * starts with sentinel bytes), treat as already-migrated -- write the
     * sentinel and return OK. This is the recovery path for AC-42.2.5. */
    if (plain_len >= HU_PERSONA_CRYPTO_SENTINEL_LEN &&
        memcmp(plain, HU_PERSONA_CRYPTO_SENTINEL, HU_PERSONA_CRYPTO_SENTINEL_LEN) == 0) {
        /* Already encrypted -- just lay down the sentinel and bail. */
        if (plain) {
            memset(plain, 0, plain_len);
            alloc->free(alloc->ctx, plain, plain_len);
        }
        char spath[1024];
        if (derive_sentinel_path(path, spath, sizeof(spath)) != 0) {
            return HU_ERR_INVALID_ARGUMENT;
        }
        const uint8_t marker[] = "done\n";
        return atomic_write(spath, marker, sizeof(marker) - 1);
    }

    /* 2. snapshot legacy: rename <path> -> <path>.legacy */
    char legacy[1024];
    int n = snprintf(legacy, sizeof(legacy), "%s.legacy", path);
    if (n <= 0 || (size_t)n >= sizeof(legacy)) {
        memset(plain, 0, plain_len);
        alloc->free(alloc->ctx, plain, plain_len);
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (rename(path, legacy) != 0) {
        memset(plain, 0, plain_len);
        alloc->free(alloc->ctx, plain, plain_len);
        return HU_ERR_IO;
    }

    /* 3-4. encrypt -> tmp -> fsync -> rename -> fsync(dir) */
    hu_error_t src = hu_persona_save_encrypted(alloc, path, plain, plain_len);
    /* 7a. zero in-memory plaintext immediately after encryption (regardless of rc). */
    memset(plain, 0, plain_len);
    alloc->free(alloc->ctx, plain, plain_len);
    if (src != HU_OK) {
        /* Rollback: restore legacy. */
        (void)rename(legacy, path);
        return src;
    }

    /* 5. write .migration_done sentinel, fsync. */
    char spath[1024];
    if (derive_sentinel_path(path, spath, sizeof(spath)) != 0) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    const uint8_t marker[] = "done\n";
    hu_error_t mrc = atomic_write(spath, marker, sizeof(marker) - 1);
    if (mrc != HU_OK) {
        /* Migration data is durable; the sentinel write failed. Caller
         * can retry; on retry we detect path already starts with sentinel
         * and replay step 5 only. Leave legacy snapshot in place so the
         * shred is deferred until sentinel succeeds. */
        return mrc;
    }

    /* 6. CRITICAL: explicit shred-and-unlink of the .legacy snapshot.
     * This is the call site the prior session's critic flagged as
     * missing. It MUST run after .migration_done has been fsync'd so
     * the system is in a recoverable state at all times. */
    hu_error_t shrc = shred_and_unlink(legacy);
    if (shrc != HU_OK) {
        /* Shred failed but the migration is otherwise complete and the
         * sentinel is in place. Surface the I/O error so the caller
         * knows the on-disk plaintext snapshot may still exist; the
         * primary file is encrypted and load_legacy on it will now
         * return HU_ERR_LEGACY_REFUSED. */
        return shrc;
    }
    return HU_OK;
}
