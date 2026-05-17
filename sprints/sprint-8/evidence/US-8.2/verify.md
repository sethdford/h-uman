# Verifier Evidence — US-8.2 Persona Encryption-at-Rest

**Branch:** impl/US-8.2  
**Commit:** 6af7ad340622507580253088e72b2db2a10bb565  
**Verified:** 2026-05-17  
**Verifier:** claude-sonnet-4-6 (verifier agent)

---

## Summary

Verified 9/9 behaviors. 0 failed. 0 inconclusive.

---

## Contract (distilled from task spec)

1. Build is -Werror clean under `cmake --preset dev`
2. All 20 persona-encryption tests pass
3. Full suite = 10408/10409 (pre-existing flake only)
4. `check-test-references.sh tests/test_persona_encryption.c` exits 0
5. AC-8.2.2: wrong key → `HU_ERR_DECRYPT_FAILED` AND `*out` untouched (BLOCKED)
6. AC-8.2.4: legacy loader on encrypted file → `HU_ERR_LEGACY_REFUSED` (BLOCKED)
7. AC-8.2.5: atomic-save adversary test uses `mkdir(<path>.tmp)` blocker, asserts prior bytes survive and are still decryptable
8. `src/persona/persona_crypt.c` line 14: `#error` if `HU_HAS_LIBSODIUM` undefined
9. `hu_persona_classify_bytes` called in `load_encrypted` (line 574), `migrate_to_encrypted` (line 669), AND `load_legacy` (line 751) — same predicate, no drift

---

## Evidence Blocks

### BEHAVIOR 1: Build clean under -Werror
```
COMMAND: cmake --build --preset dev
EXIT: 0
EVIDENCE:
  [  0%] Checking 7-layer architectural topology (P2E)
  [  0%] Checking no hardcoded fillers regressed (PCTT Task 9)
  check-no-hardcoded-fillers: OK
  [  0%] Built target human_filler_guard
  [ 39%] Built target human_core_test
  [ 76%] Built target human_core
  [100%] Built target human_tests
  Layer topology OK: 0 cross-layer violations across 68 source files.
  [100%] Built target human_topology_check
  [100%] Built target human
RESULT: PASS
```

### BEHAVIOR 2: 20 persona-encryption tests pass
```
COMMAND: ./build/human_tests --suite=persona-encryption
EXIT: 0
EVIDENCE (trimmed to relevant section):
  PASS  test_classify_empty_buffer_is_unknown
  PASS  test_classify_plaintext_starting_with_brace_is_json
  PASS  test_classify_plaintext_with_leading_whitespace_is_json
  PASS  test_classify_HUP1_magic_is_encrypted_v1
  PASS  test_classify_HUP1_truncated_to_header_only_is_unknown
  PASS  test_classify_HUP1_wrong_version_byte_is_unknown
  PASS  test_classify_random_binary_is_unknown
  PASS  test_keystore_creates_keyfile_with_0600_perms
  PASS  test_keystore_returns_same_key_on_repeat_call
  PASS  test_keystore_rejects_world_readable_keyfile
  PASS  test_save_then_load_recovers_all_fields
  PASS  test_save_twice_yields_different_ciphertext_same_plaintext
  PASS  test_load_with_wrong_key_returns_decrypt_failed_and_out_untouched
  PASS  test_load_with_tampered_ciphertext_returns_decrypt_failed
  PASS  test_migrate_plaintext_yields_encrypted_round_trippable_file
  PASS  test_migrate_is_idempotent_on_already_encrypted_file
  PASS  test_load_legacy_refuses_encrypted_file_with_legacy_refused_error
  PASS  test_load_legacy_refuses_plaintext_when_no_sentinel
  PASS  test_load_legacy_loads_plaintext_only_when_sentinel_present
  PASS  test_save_encrypted_preserves_prior_state_when_tmp_blocked
  --- Results: 20/20 passed, 10374 skipped ---
RESULT: PASS
```

### BEHAVIOR 3: Full suite 10408/10409, pre-existing flake only
```
COMMAND: ./build/human_tests
EXIT: 1 (expected — pre-existing flake)
EVIDENCE:
  FAIL  (tests/test_model_router.c:338) assert failed: hu_route_log_count(log) > before
  --- Results: 10408/10409 passed, 1 FAILED ---
  (The failing test is test_model_router.c:338 route_populates_global_log,
   the same pre-existing order-pollution flake documented in the commit message)
RESULT: PASS (flake matches documented expectation; +20 over prior base of 10388)
```

### BEHAVIOR 4: check-test-references exits 0
```
COMMAND: scripts/check-test-references.sh tests/test_persona_encryption.c
EXIT: 0
EVIDENCE: (no output — clean pass)
RESULT: PASS
```

### BEHAVIOR 5: AC-8.2.2 wrong-key → HU_ERR_DECRYPT_FAILED, *out untouched
```
COMMAND: ./build/human_tests --suite=persona-encryption
EVIDENCE (test at tests/test_persona_encryption.c:336):
  test name: test_load_with_wrong_key_returns_decrypt_failed_and_out_untouched
  PASS
  Assertion: HU_ASSERT_EQ(hu_persona_load_encrypted(..., bad, &out), HU_ERR_DECRYPT_FAILED)
  Assertion: HU_ASSERT_EQ(memcmp(&out, &expected, sizeof(out)), 0)
  Production: src/persona/persona_crypt.c:574 calls hu_persona_classify_bytes;
              crypto_secretbox_open_easy failure returns HU_ERR_DECRYPT_FAILED
              without populating *out (local pt buffer, free on failure)
RESULT: PASS
```

### BEHAVIOR 6: AC-8.2.4 legacy on encrypted → HU_ERR_LEGACY_REFUSED
```
COMMAND: ./build/human_tests --suite=persona-encryption
EVIDENCE (test at tests/test_persona_encryption.c:475):
  test name: test_load_legacy_refuses_encrypted_file_with_legacy_refused_error
  PASS
  Assertion: HU_ASSERT_EQ(hu_persona_load_legacy(..., &out), HU_ERR_LEGACY_REFUSED)
  Assertion: HU_ASSERT_EQ(memcmp(&out, &expected, sizeof(out)), 0)
  Production: src/persona/persona_crypt.c:751-762 calls classify_bytes; encrypted
              → sodium_memzero + free + return HU_ERR_LEGACY_REFUSED before *out is touched
RESULT: PASS
```

### BEHAVIOR 7: Atomic-save adversary test uses mkdir blocker pattern
```
COMMAND: Read tests/test_persona_encryption.c:538-608
EVIDENCE:
  Line 580: HU_ASSERT_EQ(mkdir(tmp_path, 0755), 0);   ← mkdir blocker
  Line 587: HU_ASSERT_EQ(err, HU_ERR_IO_BUSY);        ← blocked returns IO_BUSY
  Lines 591-596: byte-for-byte fread + memcmp confirms prior file is intact
  Line 601: hu_persona_load_encrypted on prior file returns HU_OK ← still decryptable
RESULT: PASS
```

### BEHAVIOR 8: #error if HU_HAS_LIBSODIUM undefined
```
COMMAND: grep -n "HU_HAS_LIBSODIUM" src/persona/persona_crypt.c
EXIT: 0
EVIDENCE:
  14:#if !defined(HU_HAS_LIBSODIUM)
  15:#error "persona_crypt.c requires libsodium; build with -DHU_ENABLE_LIBSODIUM=ON"
RESULT: PASS
```

### BEHAVIOR 9: hu_persona_classify_bytes used by load_encrypted AND load_legacy
```
COMMAND: grep -n "hu_persona_classify_bytes" src/persona/persona_crypt.c
EXIT: 0
EVIDENCE:
  49:  definition (the predicate itself)
  574: hu_persona_load_encrypted → classify before decode
  669: hu_persona_migrate_to_encrypted → classify for idempotent check
  751: hu_persona_load_legacy → classify before legacy-refuse check
  (Declared in include/human/persona/crypto.h line 57)
RESULT: PASS
```

---

## AC Mapping Summary

| AC | Test | Result |
|----|------|--------|
| 8.2.1 migration round-trip | test_migrate_plaintext_yields_encrypted_round_trippable_file | PASS |
| 8.2.2 wrong key BLOCKED | test_load_with_wrong_key_returns_decrypt_failed_and_out_untouched | PASS |
| 8.2.3 fresh nonce per save | test_save_twice_yields_different_ciphertext_same_plaintext | PASS |
| 8.2.4 legacy-after-migration BLOCKED | test_load_legacy_refuses_encrypted_file_with_legacy_refused_error + test_load_legacy_refuses_plaintext_when_no_sentinel | PASS |
| 8.2.5 atomic-save adversary | test_save_encrypted_preserves_prior_state_when_tmp_blocked | PASS |
| 8.2.6 symbol references | all 5 production symbols called in test | PASS |

---

## RESULT_verifier=PASS story=US-8.2
