# Critic findings — US-8.2 Persona Encryption-at-Rest

## HIGH (2)

- `src/persona/persona_crypt.c:658-735` — **Migration does not rename-to-.legacy, does not create the `.migration-pending` sentinel, and does not shred on-disk plaintext.** The design spec (steps 6–11 of "Migration order") requires: `rename(<path>, <path>.legacy)` before writing the tmp file, writing `<path>.migration-pending` as a crash marker, and calling `shred_and_unlink` on the `.legacy` file after the rename succeeds. The actual code reads plaintext into memory, encrypts it, then calls `atomic_write(path, ct, ct_len)` directly — replacing the plaintext via rename of the tmp file. The in-memory plaintext buffer is zeroed, but the original plaintext bytes remain on-disk until the OS decides to reuse those blocks. A process killed between the `rename` (line 478) completing and the filesystem overwriting the inode leaves the plaintext bytes recoverable with a forensic tool. The `shred_and_unlink` helper at line 625 was written but is never called in the migration path (only silenced via a `__attribute__((unused))` wrapper at line 804). Fix: implement steps 6–11 from the design spec — `rename` original to `.legacy`, write sentinel, overwrite `.legacy` with zeros, `unlink` it, then remove sentinel.

- `src/persona/persona_crypt.c:182` — **Unbounded recursive retry in keystore on EEXIST race.** When two processes race to create the keyfile, the loser branch at line 180 retries by calling `keystore_linux_load_or_create` recursively without a depth limit. If a third actor (e.g. a test harness or a fast-respawning daemon) removes the file between the retry's `stat` call and its `open(O_RDONLY)`, the recursion continues indefinitely, overflowing the stack. Fix: convert the EEXIST branch to a loop with a bounded retry count (e.g. 3), returning `HU_ERR_IO` after exhaustion.

## MED (3)

- `tests/test_persona_encryption.c` (absent) — **No test for truncated ciphertext (file = header + fewer than MAC_BYTES of body).** A file exactly 33–47 bytes long passes `hu_persona_classify_bytes` (len >= HEADER_BYTES=32, magic correct) but fails the second gate at line 579 (len < HEADER_BYTES + MAC_BYTES). That gate is correct, but the path is not exercised by any test. An attacker who truncates a ciphertext file after migration would trigger this path; without a test, a future refactor could silently remove the gate. Fix: add `test_load_encrypted_returns_invalid_format_on_truncated_ciphertext` with a file of exactly HEADER_BYTES+1 bytes.

- `tests/test_persona_encryption.c` (absent) — **Design specified `test_migrate_does_not_delete_plaintext_when_parse_fails` and `test_migrate_recovers_from_pending_sentinel`; neither was written.** The recovery path (process killed with `.migration-pending` sentinel present and `.legacy` still on disk) is a documented crash-window scenario (design Risk R2) with no test coverage. Because the implementation also skips sentinel creation (HIGH finding above), the recovery path is both absent and untested. Fix: once the migration sentinel logic is implemented, add both tests.

- `src/persona/persona_crypt.c:137` — **`keystore_linux_load_or_create` does not validate that the parent directory itself was created with 0700 mode when it already existed.** `ensure_parent_dir_0700` only sets 0700 on directories it creates; if `~/.human/keys/` already exists with mode 0755 (common on newly-provisioned Linux boxes), `ensure_parent_dir_0700` returns `true` without correcting the mode, and the key is read from a directory world-listable by any process on the machine. The design (R4 mitigation) says parent dir should be 0700, but the code only enforces this at creation time. Fix: after `ensure_parent_dir_0700` returns, `stat` the parent and reject if `(st.st_mode & 0077) != 0`, mirroring the keyfile perm check.

## LOW (1)

- `src/persona/persona_crypt.c:804-806` — **`persona_crypt_keep_shred_symbol` is a dead-code suppressor for `shred_and_unlink` that signals the migration path is incomplete.** The comment says "referenced by the recovery path that lives outside this story's scope (sprint-9 wiring)". Given that `shred_and_unlink` should already be called in this story's `migrate_to_encrypted` (per the design), this suppressor exists to hide the real omission. Once the HIGH finding above is fixed and `shred_and_unlink` is called from `migrate_to_encrypted`, this wrapper must be removed. Leaving it in place after the fix makes it appear intentional.

## Cross-agent regression risk

- None identified. `src/persona/persona.c` was correctly left unmodified per the design's "Out of scope" declaration. The new public symbols (`hu_persona_save_encrypted`, `hu_persona_load_encrypted`, `hu_persona_migrate_to_encrypted`, `hu_persona_load_legacy`, `hu_persona_classify_bytes`) are additive and not yet wired into any call site, so no existing caller is broken.

RESULT_critic=HAS_FINDINGS_0_2 story=US-8.2 severity=HIGH
