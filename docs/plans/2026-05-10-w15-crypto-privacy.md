---
title: "W15 — Cryptographic Privacy: envelope encryption, key-deletion = forgetting, DP-SGD, audit + export"
created: 2026-05-10
status: closed
parent: 2026-05-10-memory-v2-roadmap-overview.md
risk: high
scope: include/human/security/, src/security/, src/memory/, src/main.c (CLI subcommands)
last_audit: 2026-05-25
---

# W15 — Cryptographic Privacy

## Goal

Make privacy a **structural** property, not a setting. Per-user envelope encryption: a master key derived from passphrase + OS keychain wraps per-table data keys. Erasure becomes key destruction (cryptographic forgetting; survives backup restore). Adds DP-SGD support to W13 training. Adds user-readable audit log and GDPR Article 20 export.

## Motivation

v1's erasure (W4) is structural — it deletes rows. But:
- Backups still contain the deleted rows (no cryptographic guarantee).
- W13 training adapters can leak conversation content into model weights (no DP).
- Users can't see what data we have or how it was used (no audit log).
- Users can't take their data with them (no export). GDPR Article 20 violation when EU AI Act enforcement begins Aug 2026.

## Prior art

- Apple Intelligence — rumored envelope encryption + key-destruction forgetting.
- Signal protocol — per-conversation keys.
- DP-SGD (Abadi et al. 2016) — gradient-clipping + Gaussian noise.
- Tink / NaCl — encryption library precedent.

## Design

### Keystore

```c
/* include/human/security/keystore.h */

typedef struct hu_keystore_status {
    bool master_key_present;
    bool keychain_available;
    int data_keys_count;
    int64_t key_rotated_at;
} hu_keystore_status_t;

typedef struct hu_keystore hu_keystore_t;

hu_error_t hu_keystore_open(hu_allocator_t *alloc, const char *user_id, hu_keystore_t **out);
hu_error_t hu_keystore_unlock_with_passphrase(hu_keystore_t *ks, const char *pp, size_t pp_len);
hu_error_t hu_keystore_unlock_from_keychain(hu_keystore_t *ks);  /* macOS Keychain / Linux secret-service */
hu_error_t hu_keystore_status(hu_keystore_t *ks, hu_keystore_status_t *out);

/* Encrypt/decrypt a row blob with a per-table data key. */
hu_error_t hu_keystore_encrypt(hu_keystore_t *ks, const char *table_name,
                                const void *plaintext, size_t pt_len,
                                void **out_ciphertext, size_t *out_len);
hu_error_t hu_keystore_decrypt(hu_keystore_t *ks, const char *table_name,
                                const void *ciphertext, size_t ct_len,
                                void **out_plaintext, size_t *out_len);

/* Cryptographic forgetting: destroy the master key for `user_id`. After this,
 * no data encrypted under that key can ever be decrypted, even from backups. */
hu_error_t hu_keystore_destroy_master_key(const char *user_id);
```

### Crypto status (2026-05-10, FIX 21)

Pre-libsodium hardening landed. The keystore now uses:

- **AEAD**: ChaCha20 (in-tree) + HMAC-SHA256 with a **per-call cryptographically-random 12-byte nonce** sourced from `arc4random_buf` / `getrandom()` / `/dev/urandom` in that order. If the OS RNG is unavailable, encrypt fails with `HU_ERR_CRYPTO_ENCRYPT` rather than fall back to a deterministic value (no same-key/same-nonce reuse, ever).
- **KDF**: PBKDF2-HMAC-SHA256 with a **per-user 16-byte random salt** persisted at `<key_dir>/<user_id>.salt` (mode 0600) and **600,000 iterations** (OWASP 2023 recommendation for PBKDF2-HMAC-SHA256). In test builds the iteration count is dropped to 1,000 to keep the suite fast.
- **Cryptographic forgetting**: `hu_keystore_destroy_master_key` writes the tombstone *and* unlinks the salt. Even if the tombstone is later deleted, the original master key cannot be re-derived because the salt is gone.

Adversarial coverage:

- `test_w15_random_nonce_makes_ciphertext_unique`: encrypting the same plaintext twice produces different ciphertexts including different nonces.
- `test_w15_per_user_salt_separates_keys`: two users with the same passphrase derive different master keys; user-A ciphertext does not decrypt under user-B's keystore.
- `test_w15_destruction_removes_salt_makes_recovery_impossible`: even with the tombstone deleted by an attacker, the same passphrase produces a fresh master key (ciphertext under the original key is unrecoverable).

### Crypto status (2026-05-10, FIX 22 — libsodium upgrade) — DONE

The libsodium upgrade is now in tree, gated by the new CMake option `HU_ENABLE_LIBSODIUM` (off by default; CI flips it on after `brew install libsodium` / `apt install libsodium-dev`). When enabled, every keystore operation routes through libsodium primitives with a versioned envelope so existing v0 deployments keep working byte-for-byte.

What changed in `src/security/keystore.c`:

- **AEAD (v1)**: `crypto_aead_xchacha20poly1305_ietf_encrypt/decrypt` with a 24-byte random nonce and a 16-byte Poly1305 tag. The ciphertext envelope is `[magic:0x01][nonce:24][ct:N][tag:16]` and `table_name` is bound as AEAD additional data so a blob from one table cannot be replayed under another. The encryption path *always* prefers v1 when libsodium is linked and `sodium_init` succeeds; if init fails it falls through to the existing v0 ChaCha20 + HMAC-SHA256 path so we never silently ship an unauthenticated build.
- **KDF (v1)**: `crypto_pwhash` with `crypto_pwhash_ALG_ARGON2ID13` and `OPSLIMIT_MODERATE` / `MEMLIMIT_MODERATE` in production (test builds use `OPSLIMIT_MIN` so 9,439 tests still complete in ~34s). The KDF version is persisted per-user at `<key_dir>/<user_id>.kdf` (mode 0600) and is **sticky for the lifetime of the salt**, so an existing PBKDF2 user keeps deriving their PBKDF2 master key even on a libsodium-enabled binary. New users on a libsodium build get Argon2id; new users on a non-libsodium build get PBKDF2 — recorded explicitly in the flag file so a future binary that links libsodium does not silently rewrite their master key.
- **Random**: `randombytes_buf` when libsodium is available; `arc4random_buf` / `getrandom()` / `/dev/urandom` otherwise. The fallback chain is unchanged.
- **Cryptographic forgetting**: `hu_keystore_destroy_master_key` now removes the tombstone, the salt, **and** the KDF flag. A future unlock for the same user_id starts fresh and picks the strongest algorithm available on the host.
- **Decryption (read path)**: Tries v1 first when the leading byte is `0x01` and the buffer is at least `KS_V1_OVERHEAD` bytes; falls through to the v0 path on a Poly1305 auth failure (which catches the rare case of a v0 blob whose nonce happens to start with `0x01`).

CMake plumbing:

- `HU_ENABLE_LIBSODIUM=ON` discovers libsodium via `pkg-config` first, then a manual probe of `/opt/homebrew/{include,lib}`, `/usr/local/{include,lib}`, `/usr/{include,lib}`. On success it defines `HU_HAS_LIBSODIUM=1` on `human_core` (and on `human_core_test` after that target is added later in the file). The link directories propagate to `human_tests` via `PUBLIC` link directives so tests don't have to repeat the discovery dance.
- When `HU_ENABLE_LIBSODIUM=ON` but libsodium can't be located, configuration emits a `WARNING` and continues building the v0-only path so a missing dev-package never breaks a developer's build.

Adversarial coverage added in `tests/test_w15_keystore.c`:

- `test_w15_v1_ciphertext_has_magic_byte_when_libsodium_enabled`: encrypting under a fresh keystore on a libsodium build produces a ciphertext whose first byte is `0x01` and whose total length matches the v1 envelope. The test compiles to a no-op when `HU_HAS_LIBSODIUM` is undefined.
- `test_w15_v0_kdf_remains_decryptable_under_libsodium`: forces the per-user KDF flag to `0x00` (PBKDF2) before the first unlock, encrypts, closes, reopens, and confirms the same passphrase reproduces the same master key and decrypts the blob under a libsodium-enabled binary. Demonstrates the sticky-KDF backward-compat contract.
- `test_w15_decrypt_handles_short_buffer_with_v1_magic`: a three-byte buffer starting with the v1 magic byte must reject cleanly with `HU_ERR_CRYPTO_DECRYPT` rather than overrun on the v1 length math.
- `test_w15_destroy_removes_kdf_flag`: cryptographic forgetting also unlinks the KDF flag so future unlocks pick the strongest available algorithm.

End-to-end validation: `9439/9439` tests pass with `HU_ENABLE_LIBSODIUM=ON` (libsodium 1.0.22 from Homebrew) and `9439/9439` pass with `HU_ENABLE_LIBSODIUM=OFF` (PBKDF2 + ChaCha20+HMAC fallback). Both runs are AddressSanitizer-clean.

### Memory facade decorator

```c
/* src/security/memory_encrypted.c */
hu_error_t hu_memory_open_encrypted(hu_allocator_t *alloc, hu_graph_t *graph,
                                     hu_keystore_t *ks, hu_memory_t **out);
```

Wraps any `hu_memory_t`. On every read, decrypts the payload after fetching ciphertext from SQLite. On every write, encrypts before persisting. Schema is unchanged (blobs are opaque to SQLite).

### DP-SGD on W13

Extends `hu_learner_config_t`:
```c
bool dp_enabled;          /* default false */
float dp_epsilon;         /* privacy budget; default 8.0 */
float dp_delta;           /* default 1e-5 */
float clip_norm;          /* gradient clipping norm; default 1.0 */
```

Implementation: per-step, clip gradients to `clip_norm`, add Gaussian noise with `sigma = clip_norm * sqrt(2*ln(1.25/delta)) / epsilon`. Track cumulative epsilon; refuse training when budget exhausted.

### Audit log

```sql
CREATE TABLE IF NOT EXISTS audit_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contact_id TEXT NOT NULL DEFAULT '',
    operation TEXT NOT NULL,           /* "read", "write", "erase", "export" */
    kind INTEGER NOT NULL,             /* hu_memory_kind_t */
    target_id INTEGER,
    actor TEXT NOT NULL,               /* "user", "agent", "scheduler", channel name */
    occurred_at INTEGER NOT NULL,
    summary TEXT
);
```

Every `hu_memory_*` call appends a row. Audit log itself is encrypted (so the log can't be read without unlocking the keystore).

### CLI

```
human memory audit [--since DATE] [--actor NAME] [--kind ENTITY|RELATION|...]
human memory export [--format json|csv] [--out PATH]
human memory forget --user-id ID --i-am-sure
```

`forget` calls `hu_keystore_destroy_master_key` then optionally drops all rows for tidiness. The cryptographic guarantee is the key destruction.

## Phases

1. Keystore module + libsodium integration.
2. Encrypted memory decorator.
3. Audit log + every facade call appends.
4. DP-SGD in W13 trainer.
5. CLI subcommands `audit`, `export`, `forget`.
6. Adversarial tests (key destruction, restore-after-forget, DP epsilon enforcement).

## Test plan

- `test_w15_keystore_open_and_lock`.
- `test_w15_passphrase_derivation_deterministic`.
- `test_w15_encrypt_decrypt_round_trip`.
- `test_w15_cryptographic_forgetting_unrecoverable`: destroy key, recover from backup, decryption fails.
- `test_w15_dp_epsilon_budget_blocks_excess_training`.
- `test_w15_audit_log_records_every_call`.
- `test_w15_export_round_trip_matches_internal_data`.
- `test_w15_adversarial_brute_force_passphrase_argon2_resistance`: timing-bounded.
- `test_w15_adversarial_audit_log_tamper_detected`: HMAC over each row prevents silent edits.

## Success metric

- Cryptographic forgetting verified: data unrecoverable post-key-destruction (formal test).
- Audit log records 100% of memory operations (verified by instrumented run).
- DP-SGD adapter trained at ε=8.0 still passes A/B preference test (W13 metric).
- Export round-trips bit-for-bit (no data loss).
- Binary size delta ≤ +90 KB.

## Risks

| Risk | Mitigation |
|------|------------|
| User loses passphrase → all data unrecoverable | OS-keychain backup + encrypted recovery code on first setup |
| libsodium not available on minimal builds | Fall back to deterministic encrypted decorator with warning; tests cover both paths |
| Audit log itself becomes a privacy leak | Audit log encrypted under same keystore |
| DP noise destroys adapter quality | Tunable `epsilon`; A/B benchmark per training run |

## Out of scope

- Hardware secure enclave integration. (Future workstream.)
- Multi-device key sync.
- Zero-knowledge proofs over memory.

## Binary size budget: +90 KB.
