/*
 * W15 — Cryptographic keystore.
 *
 * Provides envelope encryption for the memory layer: a master key derived from
 * a user passphrase wraps per-table data keys. Cryptographic forgetting is
 * achieved by destroying the master key — even a backup restore cannot decrypt
 * data encrypted under a destroyed key.
 *
 * IMPLEMENTATION STATUS (first commit):
 *   - Deterministic placeholder AEAD: ChaCha20 + HMAC-SHA256 (no libsodium).
 *   - KDF: SHA-256(passphrase || user_id) — NOT secure; argon2id follow-up.
 *   - Nonce: fixed zeros (deterministic for testing). Real nonce in follow-up.
 *   See TODO(W15-secure) markers in src/security/keystore.c.
 *
 * NOT in first commit (follow-up PRs within W15):
 *   - hu_keystore_unlock_from_keychain (OS-keychain integration).
 *   - libsodium XChaCha20-Poly1305 AEAD.
 *   - argon2id passphrase derivation.
 *   - Encrypted memory decorator wrapping hu_memory_t.
 */
#ifndef HU_KEYSTORE_H
#define HU_KEYSTORE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_keystore_status {
    bool    master_key_present;
    bool    keychain_available; /* always false in first commit */
    int     data_keys_count;
    int64_t key_rotated_at;
} hu_keystore_status_t;

/* Opaque keystore handle. */
typedef struct hu_keystore hu_keystore_t;

/* Open a keystore for `user_id`. The keystore starts locked; call
 * hu_keystore_unlock_with_passphrase before encrypting or decrypting. */
hu_error_t hu_keystore_open(hu_allocator_t *alloc, const char *user_id,
                             hu_keystore_t **out);

/* Release resources. Zeroes master key in memory before freeing. */
void hu_keystore_close(hu_keystore_t *ks, hu_allocator_t *alloc);

/* Derive the master key from `pp` (passphrase). Fails with
 * HU_ERR_CRYPTO_DECRYPT if hu_keystore_destroy_master_key was previously
 * called for this user_id (tombstone is present on disk). */
hu_error_t hu_keystore_unlock_with_passphrase(hu_keystore_t *ks,
                                               const char *pp, size_t pp_len);

/* Fill `*out` with current status. */
hu_error_t hu_keystore_status(hu_keystore_t *ks, hu_keystore_status_t *out);

/* Encrypt `plaintext` (pt_len bytes) under the per-table data key for
 * `table_name`. Allocates *out_ciphertext via the allocator passed to open();
 * caller must free with alloc->free(). Returns HU_ERR_CRYPTO_ENCRYPT if the
 * keystore is locked. */
hu_error_t hu_keystore_encrypt(hu_keystore_t *ks, const char *table_name,
                                const void *plaintext, size_t pt_len,
                                void **out_ciphertext, size_t *out_len);

/* Decrypt `ciphertext` (ct_len bytes) under the per-table data key for
 * `table_name`. Allocates *out_plaintext via the allocator passed to open().
 * Returns HU_ERR_CRYPTO_DECRYPT on authentication failure or locked keystore. */
hu_error_t hu_keystore_decrypt(hu_keystore_t *ks, const char *table_name,
                                const void *ciphertext, size_t ct_len,
                                void **out_plaintext, size_t *out_len);

/* Cryptographic forgetting: permanently destroy the master key for `user_id`.
 * Writes a tombstone marker so future unlock attempts fail.
 * After this call, no data encrypted under this key can ever be decrypted,
 * even if the same passphrase is re-used. */
hu_error_t hu_keystore_destroy_master_key(const char *user_id);

#ifdef __cplusplus
}
#endif

#endif /* HU_KEYSTORE_H */
