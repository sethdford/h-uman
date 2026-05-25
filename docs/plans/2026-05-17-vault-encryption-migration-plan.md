---
title: Vault Encryption Migration Plan
date: 2026-05-17
status: closed
owner: security
related:
  - docs/research/2026-05-17-plan-validation/by-plan/2026-05-16-audit-followups/02-vault-encryption-migration.md
  - src/security/vault.c
  - src/security/vault_aead.c
  - include/human/security/vault_aead.h
  - tests/test_vault_aead.c
last_audit: 2026-05-25
---

# Vault Encryption Migration Plan

## Context

`src/security/vault.c` self-describes (line 1) as "XOR/base64 obfuscation".
That is exactly what it is. The current vault offers **no confidentiality
against an attacker who learns `HUMAN_VAULT_KEY`** (XOR is reversible by
inspection) and **no integrity at all** (no MAC, no AEAD — a single byte
flip in the on-disk JSON silently changes the decrypted secret).

This is a credibility risk against the product thesis of "the assistant
that's actually yours", because a vault that promises to protect API
keys and OAuth tokens must actually do so. The audit pinned this in
`docs/research/2026-05-17-plan-validation/by-plan/2026-05-16-audit-followups/02-vault-encryption-migration.md`.

The migration is **phased** because the change touches:

1. Cryptographic primitives (Phase 1, this plan's first slice).
2. On-disk format (Phase 2).
3. Key management — derivation, storage, rotation (Phase 3).
4. Migration of existing user vaults (Phase 4).
5. Removal of the legacy code path (Phase 5).

Each phase has a clean rollback. Each phase ships its own tests. No
phase requires holding two formats hot at the same time longer than
the migration window of Phase 4.

## Threat model

What the new vault protects against:

| Threat | Protected? | Notes |
|---|---|---|
| Offline disk read by another local user | ✓ | AEAD with key derived from passphrase; ciphertext is opaque without key. |
| Offline disk read by an attacker who copies `~/.human/` to a remote machine | ✓ | Key is not on disk (Phase 3 — OS keychain or passphrase). |
| Tamper of a single secret value | ✓ | Authentication tag is computed over the full envelope. |
| Tamper of the entire vault file structure | ✓ | Each value's AAD binds it to its slot key, so substituting a value across slots fails auth. |
| Replay of a snapshot of the whole vault file | ✗ | Out of scope for Phase 1 — a backup-restore replay is indistinguishable from "user restored from backup". A versioned rollback log is a Phase 6 follow-up. |
| Live memory dump while the vault is open | ✗ | Plaintext exists in process memory after `hu_vault_get`. Out of scope. Mitigated by short lifetimes + `secure_zero` on free. |
| Side-channel timing attacks against the host process | Partial | EtM tag check is constant-time. libsodium/OpenSSL primitives are constant-time by construction. |
| Compromise of the underlying frontier model that processes vault contents | ✗ | Vault hands plaintext to providers by design. Out of scope. |

## Phase 1: AEAD primitive (this slice — shipped)

### Scope
- Implement `hu_vault_aead_encrypt` / `hu_vault_aead_decrypt` in
  `src/security/vault_aead.c`.
- Stateless. Takes a 32-byte master key (caller-derived) plus AAD plus
  plaintext; returns a magic-byte-tagged envelope.
- Compile-time backend selection in this priority order:
  1. libsodium XChaCha20-Poly1305-IETF (`HU_HAS_LIBSODIUM`)
  2. OpenSSL AES-256-GCM (`HU_ENABLE_FIPS_CRYPTO`)
  3. ChaCha20 + HMAC-SHA256 Encrypt-then-MAC (always available)
- All backends share a single envelope-with-magic-byte layout (see the
  header), so any compiled-in backend can decrypt ciphertext written by
  any other.

### Risks
- Wrong AAD passed by a caller would silently produce ciphertext that
  cannot be decrypted later. Mitigation: Phase 2 chooses a single,
  derived AAD ("vault:" + path + ":" + key) inside the vault module
  itself; callers don't pick AADs.
- Backend mismatch across machines (e.g. ciphertext written with
  `HU_HAS_LIBSODIUM` on one machine but decrypted on a binary built
  without libsodium). Mitigation: the magic byte makes this a
  detectable error rather than silent corruption; the migration phase
  will require the read-only path for any backend that produced
  ciphertext in use.

### Test contract (proved by `tests/test_vault_aead.c`)
- `test_vault_aead_roundtrip` — encrypt then decrypt recovers plaintext.
- `test_vault_aead_wrong_key_fails_auth` — flipped key returns
  `HU_ERR_CRYPTO_DECRYPT`.
- `test_vault_aead_tampered_ciphertext_fails_auth` — single-bit
  flip in the middle of the envelope returns `HU_ERR_CRYPTO_DECRYPT`.
- `test_vault_aead_tampered_tag_fails_auth` — flip in the final byte
  (the tag).
- `test_vault_aead_aad_binding_fails_with_different_aad` — same key &
  ciphertext, different AAD fails auth (so cross-slot copy fails).
- `test_vault_aead_nonce_is_unique_across_calls` — two encrypts with
  identical inputs produce different envelopes (catches a deterministic-
  nonce regression).
- `test_vault_aead_envelope_magic_matches_active_backend` — the first
  byte of every new envelope is the magic for the active backend,
  pinning the contract that lets future builds decrypt today's output.
- `test_vault_aead_short_ciphertext_fails` and
  `test_vault_aead_unknown_magic_byte_fails` — invalid inputs do not
  buffer-overrun.
- `test_vault_aead_null_args_return_invalid` — NULL inputs return
  `HU_ERR_INVALID_ARGUMENT`, not a crash.

### Rollback
Phase 1 ships ONLY a new module. The legacy `vault.c` is untouched
except for a deprecation comment. Rollback = revert the additions.

## Phase 2: on-disk format

### Scope
- Introduce `hu_vault_v2_*` API in `src/security/vault_v2.c` that wraps
  the Phase 1 primitive.
- On-disk layout: JSON object with `{ "version": 2, "secrets":
  { <slot>: <hex-encoded-envelope> } }` (vs the current
  `{ <slot>: <base64-of-xor> }`).
- AAD is constructed inside `vault_v2.c` as `"vault:" + vault_path +
  ":" + slot_key` so the caller can never get the AAD wrong.
- File mode 0600 (already enforced by `hu_io_secure_open`).

### Risks
- A user may have a partially-written file (crash during save).
  Mitigation: write to `<path>.tmp` + `fsync` + `rename`, the same
  atomic-save pattern used for `personal_model` (validated by
  `tests/test_personal_model_atomic_save.c`).
- An attacker who tampers with the JSON wrapper (e.g. drops a slot,
  reorders pairs) wouldn't be caught by per-value AEAD alone.
  Mitigation: Phase 2.1 adds a per-file MAC over the canonicalized
  JSON, keyed by the same master key under a distinct HKDF label.

### Test contract
- v2 vault round-trips multiple slots.
- Reading a vault file with `"version": 1` falls back to the legacy
  reader (Phase 4 owns the actual data migration).
- Per-value tamper fails. Per-file MAC tamper fails. Slot deletion
  fails the per-file MAC.
- Atomic save: a directory-blocked `.tmp` slot does NOT clobber the
  previous file (same adversary test as personal-model).

## Phase 3: key management

### Scope
- `hu_vault_key_derive_from_passphrase` — uses the existing
  `pbkdf2_hmac_sha256` (or libsodium's Argon2id when linked) with the
  per-vault salt persisted at `<vault_path>.salt`. Reuses the proven
  keystore.c machinery instead of reimplementing.
- `hu_vault_key_load_from_keychain` (macOS Keychain via
  `SecKeychainFindGenericPassword`; Linux libsecret via D-Bus;
  Windows Credential Manager via `CredReadW`). Returns
  `HU_ERR_NOT_SUPPORTED` on platforms without an OS keychain.
- The vault constructor picks: keychain → passphrase env (current
  `HUMAN_VAULT_KEY` semantics, but derived to 256 bits) → prompt
  (CLI only). No silent fallback to base64.

### Risks
- Keychain integration is the single most platform-fragile area in
  this whole migration. Each platform's API has subtle UI flows (Touch
  ID prompts, libsecret schema collisions). Mitigation: ship the
  passphrase path first; gate keychain on a `HU_VAULT_KEYCHAIN`
  CMake option that defaults OFF.
- Lost passphrase = lost vault. The "cryptographic forgetting" property
  of `hu_keystore_destroy_master_key` is a feature here, not a bug, but
  users must understand it. Mitigation: doc, CLI prompt asks the user
  to confirm.

### Test contract
- PBKDF2/Argon2id KDF is reproducible across processes (salt is
  persisted).
- Wrong passphrase yields a key whose `hu_vault_aead_decrypt` fails on
  every slot — proves the auth tag is the actual gate, not the KDF.
- Keychain mock returns a fixed key; vault opens with that key.
- Missing keychain entry falls back to passphrase (with a unit test
  that pre-empties the keychain).

## Phase 4: migration of existing vaults

### Scope
- On first open of a vault that detects `"version": 1` (or any non-v2
  layout, including the bare key-value form of the legacy file), the
  vault reads every slot via the legacy path, re-encrypts each value
  with `hu_vault_aead_encrypt` + the migrated key, and writes the v2
  layout atomically. The legacy file is renamed to
  `<vault_path>.v1-backup-<timestamp>` rather than deleted, so the
  user can recover if the migration goes wrong.
- The migrated key is whatever the v1 vault used (`HUMAN_VAULT_KEY`
  env var if present, else "no key"). If the v1 vault was unkeyed
  (base64-only), Phase 4 refuses to migrate without explicit consent
  (`--migrate-unkeyed` CLI flag) because writing AEAD ciphertext under
  a key the user never set is worse than the status quo.
- An advisory log line is emitted on every migration (no secret data,
  just `[vault] migrated N slots from v1 to v2`).

### Risks
- Migration races with a concurrent vault writer. Mitigation: take an
  advisory `flock` on `<vault_path>.lock` before reading. Out of scope
  for Phase 4 if `flock` is unavailable; document the assumption.
- A partial migration leaves the user with v1 backup + partial v2 file.
  Mitigation: atomic-save ensures the v2 file is fully written or not
  written; the v1 backup is only renamed AFTER the v2 file is on disk.

### Test contract
- Fixture: a v1 vault with 5 slots, `HUMAN_VAULT_KEY="testkey"`.
  Migration produces a v2 vault with the same 5 slots, all decryptable
  with the new path. The v1 backup file exists and matches the
  original byte-for-byte.
- Fixture: an unkeyed v1 vault. Migration without
  `HU_VAULT_MIGRATE_UNKEYED=1` exits non-zero with a clear message and
  does not modify the v1 file.
- Fixture: a v1 vault that's already been migrated (v2 layout). The
  migration is a no-op.

## Phase 5: deprecation of the XOR backend

### Scope
- Once telemetry (or, more realistically, a release-cycle deadline)
  confirms the v1 path is unreferenced, delete `src/security/vault.c`
  in its entirety. The v1-backup files that users still have on disk
  remain readable via a separate `human vault import-v1` CLI command
  that's documented as "for emergency recovery only".

### Risks
- Users who skipped a release may still have v1 vaults. Mitigation:
  keep the import-v1 CLI for at least two more major versions.

### Test contract
- After Phase 5 lands, `git grep -r "XOR" src/security/` returns
  nothing.
- The import-v1 CLI round-trips a fixture v1 vault and emits a v2
  vault, then exits.

## Backwards-compatibility plan

| Caller of `hu_vault_*` (legacy API) | Action |
|---|---|
| Configured `vault_path` exists and is v1 | Phase 4 migrates on first open; subsequent opens see v2. |
| Configured `vault_path` doesn't exist | First write creates a v2 vault. (Phase 2 makes this true.) |
| `HUMAN_VAULT_KEY` env var present | Continues to be the source key in Phase 1 and Phase 2; in Phase 3 it's run through PBKDF2/Argon2id rather than used raw. The visible env var name does not change. |
| Tests using `HU_IS_TEST` in-memory vault | Continue to work — the in-memory branch in `vault.c` is independent of the on-disk format. The Phase 4 migration step is a no-op when the vault is in-memory. |

The public header `include/human/security/vault.h` is **not changed**
in Phase 1. The new symbols are in a new header
`include/human/security/vault_aead.h`. The legacy
`hu_vault_create/set/get/delete/list_keys/destroy/get_api_key`
signatures remain stable through Phase 4 to give callers the maximum
window to migrate.

## Cryptographic design rationale

### Cipher choice
- **libsodium XChaCha20-Poly1305-IETF** is the gold-standard AEAD for
  applications that need a 192-bit random nonce. The 24-byte nonce is
  large enough that uniform-random nonce generation is collision-safe
  for the operational lifetime of any conceivable vault.
- **AES-256-GCM** is the FIPS-compliant choice. The 12-byte nonce is
  fine when the nonce is uniformly random over a single key's lifetime
  (probability of collision after 2^32 messages ≈ 2^-32, acceptable
  for a vault that holds tens of secrets).
- **ChaCha20 + HMAC-SHA256 Encrypt-then-MAC** is the always-available
  fallback. EtM is the construction that survived the longest in the
  cryptographic literature (TLS 1.2 chose it; TLS 1.3 inherited it
  through ChaCha20-Poly1305 / AES-GCM). It is NOT roll-our-own — the
  primitives come from `src/crypto/dispatch.c` and `human/crypto.h`,
  which are the same primitives already used by
  `src/security/keystore.c` v0 ciphertext.

We deliberately do NOT support a custom-built construction (e.g.
streaming XOR + HMAC over the plaintext): the failure modes of
hand-rolled AEAD are well-documented, and we have no reason to take
that risk.

### Nonce strategy
Every encrypt call generates a fresh random nonce from the OS RNG
(`arc4random_buf` / `getrandom` / `/dev/urandom` /
`randombytes_buf`). The failure mode for nonce reuse under any of the
three backends is catastrophic key recovery, so a randomized nonce
returned by a single OS-RNG call beats any deterministic scheme.

The nonce-uniqueness invariant is pinned by the test
`test_vault_aead_nonce_is_unique_across_calls`. If the RNG were ever
stubbed to a constant for testing, that test would catch it.

### KDF parameters (Phase 3)
- PBKDF2-HMAC-SHA256, 600,000 iterations (OWASP 2023 minimum for SHA-256
  PBKDF2). Inherited from `src/security/keystore.c::KS_PBKDF2_ITERS`.
- Argon2id (when libsodium linked): `crypto_pwhash_OPSLIMIT_MODERATE`
  + `crypto_pwhash_MEMLIMIT_MODERATE` (libsodium defaults — currently
  3 iterations and 256 MiB). Same defaults as keystore.c.

### MAC key derivation (v3 backend)
In the EtM v3 envelope, the same 32-byte master key is fed through
`HMAC-SHA256(key, "vault-aead-v3-mac")` to produce a distinct MAC
subkey. This is the RFC 5869 motivation — never use the same byte
sequence for both stream-cipher key and MAC key, even when the
primitives are independent. The label is bound into the derivation
so a future v4 envelope can use a different label and trivially
prove its keys cannot be confused with v3 keys.

### Constant-time properties
- The libsodium and OpenSSL backends inherit the constant-time
  guarantees of their underlying primitives.
- The v3 backend's tag check uses `va_ct_memcmp` (constant-time XOR
  diff accumulator), the same construction used in
  `src/security/secrets.c`.

### What we do NOT do
- We do not derive keys from passwords inside the AEAD primitive. KDF
  is a separate concern (Phase 3) so the primitive can be reused by
  callers that already have a key (e.g. a keychain-backed vault).
- We do not implement a "key wrap" envelope. Per-secret keys are
  derived from the master key + context (in Phase 2's higher layer)
  rather than wrapped, because key-wrap APIs add complexity without
  improving the threat model for a single-master-key vault.
- We do not implement post-quantum hybrid AEAD. Out of scope; the
  threat model is offline-disk-read, not store-now-decrypt-later for
  arbitrary attacker-chosen ciphertext.

## Out of scope (filed as future work)

- HSM / TPM-backed master key (would replace Phase 3's keychain path).
- Vault-wide replay protection (Phase 6).
- Per-secret rotation (overwriting a slot rotates the per-slot AAD;
  global key rotation is straightforward but not yet wired).
- Cloud sync of an encrypted vault (this whole plan presumes local-only,
  which matches the product thesis).
