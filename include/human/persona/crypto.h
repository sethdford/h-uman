/* US-8.2 — Persona Encryption-at-Rest
 *
 * Public surface for the encrypted persona on-disk format and the
 * supporting key derivation / migration helpers.
 *
 * On-disk format (HUP1 v1):
 *
 *   offset  bytes  field
 *   0       4      magic "HUP1"   (rules out collision with any valid JSON)
 *   4       1      version (0x01)
 *   5       3      reserved (zero)
 *   8       24     XSalsa20 nonce (crypto_secretbox_NONCEBYTES)
 *   32      N      ciphertext + 16-byte Poly1305 MAC (crypto_secretbox_easy)
 *
 * The crypto primitive is libsodium's crypto_secretbox_easy
 * (XSalsa20-Poly1305).  The TU that implements these functions
 * (src/persona/persona_crypt.c) hard-fails at compile time if
 * HU_HAS_LIBSODIUM is not defined — there is no soft fallback to a
 * weaker primitive, which is exactly the silent-downgrade footgun
 * AC-8.2.4 exists to prevent.
 *
 * See `sprints/sprint-8/designs/US-8.2.md` for full rationale and
 * `.claude/rules/security-predicate-extraction.md` for why
 * `hu_persona_classify_bytes` is exposed as a pure predicate.
 */

#ifndef HU_PERSONA_CRYPTO_H
#define HU_PERSONA_CRYPTO_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona.h"

#include <stddef.h>
#include <stdint.h>

#define HU_PERSONA_CRYPT_KEY_BYTES    32u /* crypto_secretbox_KEYBYTES */
#define HU_PERSONA_CRYPT_NONCE_BYTES  24u /* crypto_secretbox_NONCEBYTES */
#define HU_PERSONA_CRYPT_MAC_BYTES    16u /* crypto_secretbox_MACBYTES */
#define HU_PERSONA_CRYPT_HEADER_BYTES 32u /* magic(4) + ver(1) + rsv(3) + nonce(24) */
#define HU_PERSONA_CRYPT_MAGIC        "HUP1"
#define HU_PERSONA_CRYPT_VERSION      0x01u

/* Pure classification predicate — testable without touching disk.
 *
 * Extracted into a header-callable function per
 * `.claude/rules/security-predicate-extraction.md` so that the
 * "is this file plaintext or encrypted?" decision is a single, pinned
 * truth-table that both load_encrypted and load_legacy consult.
 */
typedef enum hu_persona_format {
    HU_PERSONA_FORMAT_PLAINTEXT_JSON = 1,
    HU_PERSONA_FORMAT_ENCRYPTED_V1 = 2,
    HU_PERSONA_FORMAT_UNKNOWN = 3,
} hu_persona_format_t;

hu_persona_format_t hu_persona_classify_bytes(const uint8_t *buf, size_t len);

/* Derive (or fetch from keystore) a 32-byte persona key.
 *
 * Test path (HU_IS_TEST set): both linux and darwin honour the env var
 *   HU_PERSONA_KEYFILE_OVERRIDE -> absolute path
 * which selects a 0600 keyfile under the test's tmp directory.  This
 * keeps the test suite hermetic — no Keychain prompts, no per-developer
 * entitlement gymnastics.  See design §"Key derivation".
 *
 * Linux production path: reads (or creates with `getrandom` + 0600)
 *   ~/.human/keys/persona.key
 *
 * Darwin production path: SecItemCopyMatching / SecItemAdd under
 *   service "ai.human.persona", account <persona_name>.  Not exercised
 *   by the test suite; verified manually via /verify smoke.
 */
hu_error_t hu_persona_crypt_derive_key(hu_allocator_t *alloc, const char *persona_name,
                                       uint8_t key_out[HU_PERSONA_CRYPT_KEY_BYTES]);

/* Encrypted save — atomic via tmp+fsync+rename.
 *
 * The on-disk path follows the personal_model Phase-0 atomic pattern
 * (see CLAUDE.md M2 entry, tests/test_personal_model_atomic_save.c):
 * write to <path>.tmp, fsync the fd, fsync the parent dir, rename.
 *
 * The persona is serialised to a minimal JSON shape compatible with
 * `hu_persona_load_json`, then encrypted as XSalsa20-Poly1305.
 *
 * Returns HU_ERR_IO_BUSY if <path>.tmp already exists (concurrent
 * migration; design R5).
 */
hu_error_t hu_persona_save_encrypted(hu_allocator_t *alloc, const char *path,
                                     const hu_persona_t *persona,
                                     const uint8_t key[HU_PERSONA_CRYPT_KEY_BYTES]);

/* Encrypted load.
 *
 * Returns HU_ERR_DECRYPT_FAILED on MAC mismatch (wrong key or
 * tampered file).  Leaves *out entirely untouched on any error — the
 * caller must be able to rely on `out` being unchanged after a denial.
 * This is what AC-8.2.2 asserts.
 */
hu_error_t hu_persona_load_encrypted(hu_allocator_t *alloc, const char *path,
                                     const uint8_t key[HU_PERSONA_CRYPT_KEY_BYTES],
                                     hu_persona_t *out);

/* One-shot migration: detect plaintext, encrypt-in-place.
 *
 * Idempotent: returns HU_OK if the file is already HUP1.  On parse
 * failure, returns HU_ERR_INVALID_FORMAT and leaves plaintext intact
 * (we never delete data we cannot re-encrypt).
 *
 * Crash safety: at every point in time, at least one of
 *   <path>, <path>.legacy, <path>.tmp
 * contains a complete copy of the persona bytes (see design
 * "Migration order" steps).  AC-8.2.5 pins this via the deterministic
 * fix-shape probe in test_save_encrypted_preserves_prior_state_when_tmp_blocked.
 */
hu_error_t hu_persona_migrate_to_encrypted(hu_allocator_t *alloc, const char *path,
                                           const char *persona_name);

/* Refuse-path legacy loader.
 *
 * This is the load path that AC-8.2.4 (legacy refused after migration)
 * gates.  Behaviour:
 *
 *   HU_PERSONA_FORMAT_ENCRYPTED_V1 -> HU_ERR_LEGACY_REFUSED (the
 *     post-migration enforcement: callers that reach for the legacy
 *     path on an already-encrypted file get a hard denial, not a
 *     silent re-decode).  *out is left untouched.
 *
 *   HU_PERSONA_FORMAT_PLAINTEXT_JSON + <path>.migration-pending sentinel
 *     present -> HU_OK and the persona is loaded (this is the small,
 *     legitimate window during which an in-progress migration is being
 *     recovered).
 *
 *   HU_PERSONA_FORMAT_PLAINTEXT_JSON without sentinel -> HU_ERR_LEGACY_REFUSED.
 *     Plaintext personas only load via the dedicated legacy path,
 *     never as a silent fallback.
 *
 *   HU_PERSONA_FORMAT_UNKNOWN -> HU_ERR_INVALID_FORMAT.
 *
 * The classification consults `hu_persona_classify_bytes` — there is
 * no second copy of the "is this encrypted" decision that could drift.
 */
hu_error_t hu_persona_load_legacy(hu_allocator_t *alloc, const char *path, hu_persona_t *out);

#endif /* HU_PERSONA_CRYPTO_H */
