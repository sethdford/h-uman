/*
 * Vault AEAD — authenticated encryption primitive for the secrets vault.
 *
 * Phase 1 of the vault encryption migration plan
 * (docs/plans/2026-05-17-vault-encryption-migration-plan.md).
 *
 * This module replaces the XOR/base64 obfuscation in src/security/vault.c
 * with a real authenticated encryption construction. It is stateless and
 * takes a pre-derived 32-byte symmetric key — key management (derivation
 * from a passphrase, OS keychain integration, master-key rotation) is
 * Phase 3 of the migration plan and lives in a separate module.
 *
 * Backend selection (compile-time, in order of preference):
 *   1. libsodium XChaCha20-Poly1305-IETF    (when HU_HAS_LIBSODIUM)
 *   2. OpenSSL AES-256-GCM                  (when HU_ENABLE_FIPS_CRYPTO)
 *   3. ChaCha20 + HMAC-SHA256 (Encrypt-then-MAC, always available)
 *
 * All three are vetted AEAD constructions. The fallback uses the same
 * primitives that src/security/keystore.c v0 ciphertext uses, with
 * implementations that come from src/crypto/dispatch.c (chacha20 +
 * sha256 reference and asm).
 *
 * On-disk envelope format (version-tagged so future backend swaps can
 * decrypt old ciphertext):
 *
 *   v1 (libsodium XChaCha20-Poly1305):
 *     [magic:0x01][nonce:24][ct:N][tag:16]
 *
 *   v2 (OpenSSL AES-256-GCM):
 *     [magic:0x02][nonce:12][ct:N][tag:16]
 *
 *   v3 (ChaCha20 + HMAC-SHA256 EtM):
 *     [magic:0x03][nonce:12][ct:N][mac:32]
 *
 * The magic byte identifies the construction at decrypt time. Decrypt
 * always reads the magic, dispatches to the right backend, and fails
 * with HU_ERR_CRYPTO_DECRYPT on any byte mismatch (wrong key, tampered
 * ciphertext, tampered nonce, tampered AAD).
 *
 * AAD (additional authenticated data) is bound into the tag without
 * being encrypted. The vault uses AAD to bind a ciphertext to the
 * (vault_path, secret_key) pair so a ciphertext copied from one secret
 * slot to another fails authentication.
 */
#ifndef HU_SECURITY_VAULT_AEAD_H
#define HU_SECURITY_VAULT_AEAD_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Master key length: 32 bytes (256 bits) for all supported backends. */
#define HU_VAULT_AEAD_KEY_LEN 32

/* Maximum on-disk envelope overhead across all backends (worst case is
 * v1: 1 magic + 24 nonce + 16 tag = 41 bytes). Callers can size their
 * ciphertext buffer as plaintext_len + HU_VAULT_AEAD_MAX_OVERHEAD. */
#define HU_VAULT_AEAD_MAX_OVERHEAD 64

/* Backend identifier — returned by hu_vault_aead_active_backend(). */
typedef enum hu_vault_aead_backend {
    HU_VAULT_AEAD_BACKEND_LIBSODIUM = 1, /* XChaCha20-Poly1305-IETF */
    HU_VAULT_AEAD_BACKEND_OPENSSL_AES_GCM = 2,
    HU_VAULT_AEAD_BACKEND_CHACHA20_HMAC = 3 /* Encrypt-then-MAC fallback */
} hu_vault_aead_backend_t;

/* Returns the backend that NEW ciphertext will be written with on this
 * build. Decrypt accepts ciphertext from any compiled-in backend
 * (identified by the magic byte), but a fresh encrypt always uses the
 * highest-priority backend that was linked. */
hu_vault_aead_backend_t hu_vault_aead_active_backend(void);

/* Encrypt `plaintext` (pt_len bytes) under `key` (must be
 * HU_VAULT_AEAD_KEY_LEN bytes). The `aad` (aad_len bytes) is bound into
 * the authentication tag but not encrypted — typically a context label
 * like "vault:<path>:<secret-key>" that prevents ciphertext copy-paste
 * across slots.
 *
 * Allocates `*out_ciphertext` of size `*out_len` via `alloc`; caller
 * must free with `alloc->free(alloc->ctx, *out_ciphertext, *out_len)`.
 *
 * Returns:
 *   HU_OK on success.
 *   HU_ERR_INVALID_ARGUMENT for NULL key / out / alloc.
 *   HU_ERR_CRYPTO_ENCRYPT  if the OS RNG fails (we never silently
 *                          fall back to deterministic nonces).
 *   HU_ERR_OUT_OF_MEMORY   if allocation fails. */
hu_error_t hu_vault_aead_encrypt(hu_allocator_t *alloc, const uint8_t key[HU_VAULT_AEAD_KEY_LEN],
                                 const uint8_t *aad, size_t aad_len, const uint8_t *plaintext,
                                 size_t pt_len, uint8_t **out_ciphertext, size_t *out_len);

/* Decrypt `ciphertext` (ct_len bytes) under `key`. The supplied `aad`
 * (aad_len bytes) MUST match the value passed at encrypt time, byte for
 * byte, or authentication fails.
 *
 * Allocates `*out_plaintext` of size `*out_len` via `alloc`; caller
 * must free with `alloc->free(alloc->ctx, *out_plaintext, *out_len)`.
 *
 * Returns:
 *   HU_OK on success.
 *   HU_ERR_INVALID_ARGUMENT for NULL key / out / alloc, or ct_len too
 *                          small to even contain the envelope header.
 *   HU_ERR_CRYPTO_DECRYPT  on authentication failure (wrong key,
 *                          tampered ciphertext, tampered nonce,
 *                          tampered AAD, unknown magic byte, or
 *                          a ciphertext for a backend not compiled in).
 *   HU_ERR_OUT_OF_MEMORY   if allocation fails.
 *
 * The implementation is constant-time across success and authentication
 * failure to the extent the underlying primitive is. */
hu_error_t hu_vault_aead_decrypt(hu_allocator_t *alloc, const uint8_t key[HU_VAULT_AEAD_KEY_LEN],
                                 const uint8_t *aad, size_t aad_len, const uint8_t *ciphertext,
                                 size_t ct_len, uint8_t **out_plaintext, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* HU_SECURITY_VAULT_AEAD_H */
