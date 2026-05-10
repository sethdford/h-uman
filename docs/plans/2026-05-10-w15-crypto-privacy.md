---
title: "W15 — Cryptographic Privacy: envelope encryption, key-deletion = forgetting, DP-SGD, audit + export"
created: 2026-05-10
status: proposed
parent: 2026-05-10-memory-v2-roadmap-overview.md
risk: high
scope: include/human/security/, src/security/, src/memory/, src/main.c (CLI subcommands)
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

Backed by libsodium (already a dependency option). XChaCha20-Poly1305 for AEAD. Argon2id for passphrase derivation.

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
