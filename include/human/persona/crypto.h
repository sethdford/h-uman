#ifndef HU_PERSONA_CRYPTO_H
#define HU_PERSONA_CRYPTO_H

/*
 * Persona encryption-at-rest (Sprint 42, US-42.2)
 *
 * Operates on raw JSON-byte blobs. The persona core's serialization format
 * is unchanged — these helpers add a sentinel-prefixed AEAD wrapper around
 * whatever the persona writer produces, and reverse it on load.
 *
 * Backend:
 *   - libsodium crypto_secretbox_xsalsa20poly1305 when HU_ENABLE_LIBSODIUM=1
 *   - existing ChaCha20 + HMAC-SHA256 EtM (include/human/crypto.h) otherwise
 *   The selection is compile-time. Both wire formats begin with the same
 *   4-byte sentinel ("hupe") so the classifier predicate is backend-agnostic.
 *
 * Wire format (v1):
 *   [4 byte sentinel "hupe"]
 *   [1 byte backend tag: 0x01 = libsodium, 0x02 = chacha20+hmac]
 *   [3 bytes reserved/zero]
 *   [24 byte nonce / IV]
 *   [N bytes ciphertext]
 *   [16 bytes auth tag]  (poly1305 or HMAC-SHA256-truncated)
 *
 * Key material:
 *   - darwin: macOS Keychain (kSecClassGenericPassword, service "hu.persona.key",
 *             account "persona-secretbox"); kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly.
 *   - linux : 0600 keyfile at ~/.human/keys/persona.key
 *             Mode is fstat-asserted on open; widened mode -> HU_ERR_PERMISSION_DENIED.
 *   - test  : when HU_IS_TEST is defined, an in-memory shim returns a deterministic
 *             32-byte key. CI has no real keychain on macOS runners; the shim path
 *             exercises the full encrypt/decrypt logic without OS keychain.
 *
 * Migration discipline (CRITICAL — caught by the prior session's critic):
 *   1. read plaintext
 *   2. rename <persona>.json -> <persona>.json.legacy  (snapshot)
 *   3. write <persona>.json.tmp with sentinel + nonce + ct + tag
 *   4. fsync(tmp); rename(tmp, <persona>.json); fsync(dir)
 *   5. write+fsync <persona>.json.migration_done sentinel file
 *   6. shred_and_unlink(<persona>.json.legacy)  <-- explicit call site, never skip
 *   7. memzero plaintext buffer
 *
 *  After step 5 succeeds, hu_persona_load_legacy() on the same path returns
 *  HU_ERR_LEGACY_REFUSED — refusal predicate keys off the .migration_done sentinel.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sentinel magic prefixed to every encrypted persona file: "hupe". */
#define HU_PERSONA_CRYPTO_SENTINEL_LEN 4
extern const uint8_t HU_PERSONA_CRYPTO_SENTINEL[HU_PERSONA_CRYPTO_SENTINEL_LEN];

/* Minimum encrypted blob: sentinel(4) + backend(1) + reserved(3) +
 * nonce(24) + tag(16). Empty plaintext is permitted (length 48). */
#define HU_PERSONA_CRYPTO_MIN_BLOB_LEN 48

/* Persona AEAD key length. 32 bytes for both backends. */
#define HU_PERSONA_CRYPTO_KEY_LEN 32

/* Result of inspecting the first few bytes of a file off disk. Pure
 * predicate — no I/O, no allocation. Single source of truth used by both
 * load_encrypted and load_legacy per security-predicate-extraction.md. */
typedef enum {
    HU_PERSONA_BYTES_PLAINTEXT,         /* no sentinel + no migration_done sentinel file */
    HU_PERSONA_BYTES_ENCRYPTED,         /* sentinel present, length >= MIN_BLOB_LEN */
    HU_PERSONA_BYTES_MIGRATION_PENDING, /* no sentinel BUT .migration_done sentinel exists ->
                                           refused */
    HU_PERSONA_BYTES_INVALID            /* sentinel present but truncated, or NULL buf */
} hu_persona_byte_class_t;

/* Pure classifier — inspects the first HU_PERSONA_CRYPTO_SENTINEL_LEN bytes
 * for the sentinel. The migration_done flag lets the caller signal whether
 * a sibling ".migration_done" sentinel file is on disk; this distinguishes
 * "legitimately plaintext (pre-migration)" from "plaintext that must be
 * refused (post-migration)". */
hu_persona_byte_class_t hu_persona_classify_bytes(const uint8_t *buf, size_t len,
                                                  bool migration_done_present);

/* Encrypt a JSON blob (in) and write it atomically to `path`.
 *   - in/in_len: the persona JSON serialization (caller-owned).
 *   - path: target file path (e.g. ~/.human/personas/<name>.json).
 *   - alloc: used for the ciphertext buffer; freed before return.
 * Atomic: writes `<path>.tmp`, fsyncs, renames, fsyncs the directory.
 * Returns HU_OK | HU_ERR_IO | HU_ERR_CRYPTO_ENCRYPT | HU_ERR_OUT_OF_MEMORY |
 *         HU_ERR_INVALID_ARGUMENT.
 */
hu_error_t hu_persona_save_encrypted(hu_allocator_t *alloc, const char *path, const uint8_t *in,
                                     size_t in_len);

/* Read+decrypt `path`. On success, *out is a newly-allocated buffer of
 * *out_len bytes containing the decrypted plaintext (JSON). The buffer is
 * allocated via `alloc` and must be freed by the caller via
 * alloc->free(ctx, *out, *out_len).
 *
 * Returns:
 *   HU_OK                 -- successfully decrypted
 *   HU_ERR_NOT_FOUND      -- file does not exist
 *   HU_ERR_INVALID_FORMAT -- first 4 bytes do not match sentinel
 *   HU_ERR_DECRYPT_FAILED -- AEAD tag mismatch / wrong key / tampered ct
 *   HU_ERR_IO             -- read failure
 *   HU_ERR_OUT_OF_MEMORY  -- allocation failure
 */
hu_error_t hu_persona_load_encrypted(hu_allocator_t *alloc, const char *path, uint8_t **out,
                                     size_t *out_len);

/* Read a plaintext (pre-migration) persona file. If a `<path>.migration_done`
 * sentinel exists in the same directory, returns HU_ERR_LEGACY_REFUSED
 * WITHOUT reading the file contents — this is the post-migration safety
 * gate per AC-42.2.3 final clause.
 *
 * On success, *out is a newly-allocated buffer of *out_len bytes containing
 * the file contents verbatim (caller must free).
 */
hu_error_t hu_persona_load_legacy(hu_allocator_t *alloc, const char *path, uint8_t **out,
                                  size_t *out_len);

/* Migrate `path` from plaintext to encrypted. Sequence is fully ordered;
 * see header banner. shred_and_unlink is called explicitly inside this
 * function after the .migration_done sentinel has been fsync'd.
 *
 * Returns:
 *   HU_OK                  -- migration complete; legacy bytes shredded
 *   HU_ERR_NOT_FOUND       -- path does not exist
 *   HU_ERR_LEGACY_REFUSED  -- already migrated (.migration_done present)
 *   HU_ERR_IO              -- file-system failure mid-migration
 *   HU_ERR_CRYPTO_ENCRYPT  -- AEAD encryption failed
 *   HU_ERR_OUT_OF_MEMORY   -- allocation failure
 *
 * If this function returns anything other than HU_OK, the original file
 * is preserved (rename-snapshot-first), and the caller can safely retry.
 */
hu_error_t hu_persona_migrate_to_encrypted(hu_allocator_t *alloc, const char *path);

/* Open / obtain the persona encryption key. Returns 32 bytes into out[].
 * On darwin queries Keychain; on linux reads ~/.human/keys/persona.key
 * (mode-asserted 0600). Under HU_IS_TEST returns a deterministic test key
 * so CI doesn't require a real keychain.
 *
 * On first use the key is created (Keychain SecItemAdd / linux O_CREAT|O_EXCL
 * with bounded retry, max 3 attempts at 50ms, returns HU_ERR_IO_BUSY on
 * exhaustion). NEVER recursive. */
hu_error_t hu_persona_crypto_key_open(uint8_t out[HU_PERSONA_CRYPTO_KEY_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* HU_PERSONA_CRYPTO_H */
