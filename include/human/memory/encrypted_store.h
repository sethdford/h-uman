/*
 * Encrypted memory envelope (W15 keystore wrapper).
 *
 * Thin adapter that wraps a single memory row's plaintext under the
 * per-user `hu_keystore_t` so the on-disk byte stream cannot be read
 * without the master key. This is a structural primitive — the
 * SQLite engine (and any other backend) is expected to call wrap()
 * before INSERT and unwrap() after SELECT when the user has opted
 * in via `memory.encrypt_at_rest`.
 *
 * Envelope (versioned, forward-compatible):
 *
 *     ┌──────────────┬────────────────────────────────┐
 *     │ "HUE1"  (4B) │ payload from hu_keystore_encrypt│
 *     └──────────────┴────────────────────────────────┘
 *
 * The 4-byte magic ("Human-Uman Envelope, version 1") is the cheap,
 * authoritative sniff that lets a reader distinguish a wrapped row
 * from a legacy plaintext row without paying for a decrypt attempt.
 * The payload is whatever `hu_keystore_encrypt` produced — currently
 * either v0 (PBKDF2 + ChaCha20+HMAC) or v1 (XChaCha20-Poly1305 with
 * libsodium); both are decryptable by `hu_keystore_decrypt` given the
 * same master key.
 *
 * Bumping the magic to "HUE2" is reserved for envelope-level format
 * changes (e.g. attaching a key-id header for rotation). The keystore
 * AEAD versioning is independent and lives inside the payload.
 *
 * Backward compat: a legacy (pre-encryption) row that was never
 * wrapped will not start with the magic bytes, so
 * `hu_encrypted_store_is_encrypted` returns false and the caller can
 * pass the raw bytes through unchanged. This is essential for the
 * opt-in rollout — flipping `encrypt_at_rest` ON must not orphan any
 * already-stored data.
 *
 * What this module does NOT do:
 *   - It does not own the keystore. Callers pass `hu_keystore_t *`
 *     and remain responsible for unlocking/closing it.
 *   - It does not key per-table or per-row. A single namespace string
 *     is used so any wrapped blob can be unwrapped by any caller with
 *     the same keystore.
 *   - It does not log plaintext, ciphertext, or any keystore metadata.
 */
#ifndef HU_MEMORY_ENCRYPTED_STORE_H
#define HU_MEMORY_ENCRYPTED_STORE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/security/keystore.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 4-byte envelope magic. ASCII so a hex dump is self-describing. */
#define HU_ENCRYPTED_STORE_MAGIC      "HUE1"
#define HU_ENCRYPTED_STORE_MAGIC_LEN  4

/* Wrap `plaintext` (pt_len bytes) into a versioned envelope and write
 * the result to `*ct_out`. The wrapper buffer is allocated via `alloc`;
 * the caller MUST free with the same allocator. On success,
 * `*ct_len_out` holds the total envelope length
 * (HU_ENCRYPTED_STORE_MAGIC_LEN + keystore payload length).
 *
 * `alloc` must be the same allocator that was passed to
 * `hu_keystore_open` — the wrapper uses it both for its own buffer
 * and to free the intermediate ciphertext returned by the keystore,
 * which avoids the keystore needing to expose its internal allocator.
 *
 * Empty plaintext (pt_len == 0) is allowed and produces a valid
 * envelope whose payload authenticates the empty string.
 *
 * Returns:
 *   HU_OK on success
 *   HU_ERR_INVALID_ARGUMENT if any required pointer is NULL or the
 *     keystore is missing
 *   HU_ERR_CRYPTO_ENCRYPT if the keystore is locked or the AEAD fails
 *   HU_ERR_OUT_OF_MEMORY on allocation failure
 */
hu_error_t hu_encrypted_store_wrap(hu_keystore_t *ks, hu_allocator_t *alloc,
                                   const void *plaintext, size_t pt_len,
                                   void **ct_out, size_t *ct_len_out);

/* Unwrap a versioned envelope previously produced by
 * `hu_encrypted_store_wrap`. Strips the magic bytes, then delegates
 * to `hu_keystore_decrypt`. Authentication failure (wrong key,
 * tampered payload) yields HU_ERR_CRYPTO_DECRYPT — never silent
 * partial output.
 *
 * On success, `*pt_out` holds the plaintext and `*pt_len_out` its
 * length. The plaintext buffer is allocated by the keystore using its
 * own internal allocator (the same one the caller passed to
 * `hu_keystore_open`), so callers must free with that allocator
 * — typically the same `alloc` they handed to `wrap`.
 *
 * Returns:
 *   HU_OK on success
 *   HU_ERR_INVALID_ARGUMENT if the envelope is shorter than the magic
 *     header or the magic doesn't match
 *   HU_ERR_CRYPTO_DECRYPT on AEAD authentication failure
 *   HU_ERR_OUT_OF_MEMORY on allocation failure
 */
hu_error_t hu_encrypted_store_unwrap(hu_keystore_t *ks,
                                     const void *ciphertext, size_t ct_len,
                                     void **pt_out, size_t *pt_len_out);

/* Cheap O(1) sniff: does `blob` start with the envelope magic?
 * Safe to call with NULL or short buffers (returns false). Use this
 * to decide between unwrap() and pass-through for a legacy plaintext
 * row.
 *
 * Note: a true return is not proof of decryptability — only that the
 * blob *claims* to be a wrapped envelope. The keystore's AEAD is the
 * only authoritative verifier. */
bool hu_encrypted_store_is_encrypted(const void *blob, size_t blob_len);

#ifdef __cplusplus
}
#endif

#endif /* HU_MEMORY_ENCRYPTED_STORE_H */
