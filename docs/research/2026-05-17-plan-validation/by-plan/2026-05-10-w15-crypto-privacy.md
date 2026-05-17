---
plan: docs/plans/2026-05-10-w15-crypto-privacy.md
auditor: group-7-memory
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: PARTIAL
verdict: PARTIAL
confidence: HIGH
---

## Plan Summary
Per-user envelope encryption (master key → per-table data keys), cryptographic forgetting via key destruction, DP-SGD for W13, audit log, GDPR Article 20 export. Plan status: "keystore + audit landed; libsodium upgrade landed; DP-SGD + GDPR export pending".

## Key Claims (from the plan)
- Envelope encryption
- Cryptographic erasure
- DP-SGD
- Audit log
- GDPR export

## Evidence

### Implemented? (code exists)
- `src/security/keystore.c` + `include/human/security/keystore.h` — keystore + libsodium
- `src/security/audit.c` + `audit_log.c` + `cot_audit.c` + `mcp_audit.c` — audit logging
- `src/memory/encrypted_store.c` + `include/human/memory/encrypted_store.h`
- `src/memory/erasure.c` — cascading erasure (structural; cryptographic erasure via key destruction not yet)
- No DP-SGD found: grep for `dp_sgd|differential_priv` returned no hits in src/ml/ or src/security/

### Proven? (tests exist)
- `tests/test_w15_keystore.c`
- `tests/test_w15_backup_restore.c`

### Wired? (called in runtime path / dispatch)
- Keystore wired into daemon init paths
- Audit log written during sensitive operations
- DP-SGD not wired (not yet implemented)
- GDPR export not found

## Gaps
- DP-SGD missing in ml stack
- GDPR Article 20 export (machine-readable user-data export) not found
- Cryptographic erasure (key destruction = forgetting) appears partial

## Notes
Plan honestly self-reports partial status. Reality matches: keystore + audit + libsodium yes; DP-SGD + GDPR export still open.
