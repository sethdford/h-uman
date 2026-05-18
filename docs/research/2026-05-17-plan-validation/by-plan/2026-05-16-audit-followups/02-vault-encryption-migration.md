---
plan: docs/plans/2026-05-16-audit-followups/02-vault-encryption-migration.md
auditor: group-11-audit-followups-adrs-renames-memory
audited_at: 2026-05-17
implemented: NONE
proven: NONE
wired: NONE
verdict: NOT_STARTED
confidence: HIGH
---

## Plan Summary
Migrate `src/security/vault.c` from XOR/base64 obfuscation to libsodium
`crypto_secretbox_xchacha20poly1305` AEAD with OS keychain master-key derivation,
versioned file format `HUVT_002`, and one-way migration of legacy v1 vaults.

## Key Claims (from the plan)
- Claim 1: libsodium dep added behind `HU_ENABLE_LIBSODIUM` (vendored amalgamation under `vendor/libsodium/`)
- Claim 2: File format magic `HUVT_002` with version byte, kdf_salt, nonces per entry
- Claim 3: Master key from OS keychain (macOS Keychain / Linux Secret Service / Windows DPAPI)
- Claim 4: Tampered ciphertext returns `HU_ERR_SECURITY_TAMPERED`
- Claim 5: Legacy v1 vaults auto-migrate on first open; `vault_xor_*` deleted afterwards
- Claim 6: `minimal` preset refuses to store rather than obfuscating

## Evidence

### Implemented? (code exists)
- `src/security/vault.c:1-4` — header still describes itself as
  "XOR/base64 obfuscation. Uses HUMAN_VAULT_KEY env var for XOR key; falls back to base64"
- `grep -rn "libsodium\|crypto_secretbox\|HUVT_002\|HUVT_001" src/security/ include/` — 0 hits
- No `vendor/libsodium/` directory
- No `HU_ENABLE_LIBSODIUM` macro in CMakePresets or CMakeLists

### Proven? (tests exist)
- `tests/test_vault.c` exists but tests the XOR/base64 implementation, not AEAD
- No `HU_ERR_SECURITY_TAMPERED` references anywhere

### Wired? (called in runtime path / dispatch)
- N/A — code not present to wire

## Gaps
- Entire migration not started
- Privacy thesis vulnerability (audit's "existential" framing) still present
- All 8 ACs unmet

## Notes
The audit's framing was "existential for 'your data never leaves the device' thesis."
The vault.c file still self-describes as obfuscation, contradicting the M2/M4 privacy
narrative in CLAUDE.md. Status in plan's frontmatter says "Spec — not yet implemented"
and remains accurate.
