# Spec: Vault Encryption Migration (XOR → libsodium secretbox)

**Status:** Spec — not yet implemented
**Author:** 2026-05-16 audit follow-up
**Owner:** TBD
**Risk:** High — touches credential storage; data migration required
**Effort:** 1 week (impl + migration + tests) + 1 week burn-in

## Problem statement

`src/security/vault.c:4` describes itself as **"XOR/base64 obfuscation"** —
not encryption. Credentials stored via the vault to `~/.human/` are
recoverable by any local attacker. This directly contradicts the privacy
thesis ("data never leaves the device" implies "data is safe on the device").

The audit framing was clear:

> Privacy thesis depends on this. Recoverable by any local attacker.
> Existential for "your data never leaves the device" thesis.

The keystore (`src/security/keystore.c`) does AEAD properly for master-key
derivation — that's a good template. The vault must reach the same bar.

## Acceptance criteria

| AC | Description | Verification |
|---|---|---|
| AC-1 | New vault uses libsodium `crypto_secretbox_xchacha20poly1305` AEAD. | Code review + test that ciphertext is indistinguishable from random. |
| AC-2 | Master key derived from OS keychain (macOS Keychain / Linux Secret Service / Windows DPAPI). Falls back to passphrase prompt if keychain unavailable. | Manual test per platform; CI test for fallback path. |
| AC-3 | Each stored secret has a unique 24-byte nonce. | Test: store same plaintext twice, assert different ciphertexts. |
| AC-4 | Tampered ciphertext is rejected with `HU_ERR_SECURITY_TAMPERED`. | Test: flip a bit; assert open fails. |
| AC-5 | Migration: existing XOR-obfuscated vaults are read once, decrypted with the legacy code, then re-encrypted on first save. | Migration test using a captured legacy vault fixture. |
| AC-6 | After migration, the legacy XOR code path is removed (no dual paths). | Grep — no `vault_xor_*` references remain. |
| AC-7 | Stored secret keys (not values) remain queryable without decrypting all values (e.g., list secret names). | Test: list 100 secrets; only requested ones decrypt. |
| AC-8 | All tests must pass with ASan; no leaks. | Existing CI gate. |

## Design

### Dependency

Add libsodium as a build dependency, gated by `HU_ENABLE_LIBSODIUM`
(default ON for `dev` and `release` presets; OFF for `minimal`).

Vendor-in the amalgamation under `vendor/libsodium/` to preserve the
"zero deps beyond libc" claim for `minimal` builds. `minimal` builds
get a vault that **refuses to store** rather than obfuscates — explicit
failure, not silent weakness.

### File format

```
[8 bytes]  magic "HUVT_002"
[1 byte]   format_version = 2
[16 bytes] kdf_salt
[1 byte]   reserved
[N entries]:
  [u32 le] key_len
  [N bytes] key (cleartext, allows listing without decryption)
  [u32 le] ct_len
  [24 bytes] nonce
  [N bytes] ciphertext (AEAD-sealed value || mac)
```

Version 1 = legacy XOR. Migration triggered when version byte == 1.

### Key derivation

```
master_key = HKDF-SHA256(
    secret = keychain_get("human-vault") OR passphrase_kdf(prompt),
    salt = file.kdf_salt,
    info = "h-uman vault v2"
)
```

Master key cached in-memory for session lifetime in a `mlock`-pinned
allocation. Wiped on shutdown.

### API (header unchanged)

`hu_vault_get`, `hu_vault_set`, `hu_vault_delete`, `hu_vault_list` keep their
existing signatures. Internal callers see no change.

## Migration

```c
hu_error_t hu_vault_open(const char *path, hu_vault_t **out) {
    // Read header, dispatch on version byte.
    if (version == 1) {
        // Read all v1 entries into memory.
        // Generate new salt, derive new master key.
        // Atomic-rewrite as v2.
        // Continue as v2.
    }
    // ... v2 open path
}
```

Atomic rewrite: tmp file + fsync + rename, same pattern as
`hu_personal_model_save` (already shipped, see CLAUDE.md M2 row).

## Out of scope

- Hardware-backed keys (Secure Enclave, YubiKey). Future work.
- Per-secret access policies (e.g., this secret only accessible after biometric).

## Audit evidence

- `src/security/vault.c:4` — header comment: "XOR/base64 obfuscation."
- `src/security/keystore.c:136-150` — AEAD precedent within the codebase.
- Threat model: any process running as the user can read `~/.human/` files.

## Risks

- **Lost master key = lost vault.** *Mitigation:* on first init, prompt for a
  recovery passphrase that derives a parallel decryption path. Document
  recovery flow in `docs/standards/security/`.
- **Keychain not available in CI.** *Mitigation:* CI uses passphrase fallback
  via env var `HU_VAULT_PASSPHRASE` (test-only).
- **Migration runs on every open if v1 detected.** *Mitigation:* migration
  rewrites the file as v2; subsequent opens skip the migration branch.
- **libsodium build issues on uncommon platforms.** *Mitigation:* vendor the
  amalgamation; `minimal` preset disables vault entirely rather than degrading.
