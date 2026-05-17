# Design for US-8.2: Encryption-at-Rest for Persona Files

**Story:** US-8.2 (P0, HIGH risk) — Sprint 8 "Verifiable Privacy"
**Author:** tech-lead
**Date:** 2026-05-17
**Status:** DESIGN_READY — implementer can execute

---

## Approach

Add a single new translation unit `src/persona/persona_crypt.c` (and matching
header `include/human/persona_crypt.h`) that owns every byte of the encrypted
persona on-disk format. The existing `hu_persona_load` in `src/persona/persona.c`
is **not** changed yet; instead, the gated entry points
(`hu_persona_load_encrypted`, `hu_persona_save_encrypted`,
`hu_persona_migrate_to_encrypted`, `hu_persona_load_legacy`) are new public
functions that the caller (and a later wiring story) picks up. This keeps the
risk surface small and reversible: if anything in Sprint 8 goes wrong, no
existing code path is altered.

The crypto primitive is **libsodium `crypto_secretbox_easy`** (XSalsa20-Poly1305)
matching the AC text. libsodium is **already wired into the build** via
`HU_ENABLE_LIBSODIUM` in `CMakeLists.txt:61` and `CMakeLists.txt:1726-1772`
(pkg-config first, manual `find_path`/`find_library` fallback, defines
`HU_HAS_LIBSODIUM=1`). The story-claimed "vendor/libsodium" is **not** actually
vendored; the build relies on a system/Homebrew install. **No new external
dependency is needed**, but the preset selection matters: implementer must
build with `-DHU_ENABLE_LIBSODIUM=ON` and the new TU must hard-fail at compile
time with `#error` if `HU_HAS_LIBSODIUM` is undefined — this is a security
TU and a soft fallback to "no crypto" is exactly the silent-downgrade footgun
the story warns about.

Key derivation has two backends behind one façade:

- **Linux:** read 32 random bytes from `~/.human/keys/persona.key` (mode 0600,
  created on first use via `getrandom(2)` + `O_CREAT|O_EXCL`, parent dir
  `~/.human/keys/` is `chmod 0700`).
- **Darwin:** `SecItemCopyMatching` (service `ai.human.persona`, account
  `<persona_name>`), generating via `SecRandomCopyBytes` + `SecItemAdd` on
  first call.
- **Test (`HU_IS_TEST` set):** both platforms route through the **Linux
  keyfile backend pointed at a tmp dir** via env var
  `HU_PERSONA_KEYFILE_OVERRIDE`. This keeps the test suite hermetic (no
  Keychain prompts, no per-developer entitlement gymnastics) and is exactly
  the seam called out in the task brief.

The "is this file already encrypted, did migration complete" decision is
extracted as a **pure predicate** per `.claude/rules/security-predicate-extraction.md`:

```c
/* in include/human/persona_crypt.h, callable from tests without touching disk */
typedef enum {
    HU_PERSONA_FORMAT_PLAINTEXT_JSON,    /* first byte '{', valid UTF-8 prefix */
    HU_PERSONA_FORMAT_ENCRYPTED_V1,      /* magic "HUP1" + 24B nonce + ct+mac */
    HU_PERSONA_FORMAT_UNKNOWN             /* corrupt / truncated */
} hu_persona_format_t;

hu_persona_format_t hu_persona_classify_bytes(const uint8_t *buf, size_t len);
```

On-disk format for `HU_PERSONA_FORMAT_ENCRYPTED_V1`:

```
offset  bytes  field
0       4      magic "HUP1"        (so classify_bytes never confuses ct with JSON)
4       1      version = 0x01
5       3      reserved zero
8       24     XSalsa20 nonce      (fresh per-write, crypto_secretbox_NONCEBYTES)
32      N      ciphertext+MAC      (crypto_secretbox_easy output, MAC trailing)
```

The 4-byte magic is load-bearing: it makes
`hu_persona_classify_bytes` a O(1) check that cannot collide with any valid
JSON document (which must begin with `{` or whitespace per RFC 8259). It also
makes `HU_ERR_LEGACY_REFUSED` from `hu_persona_load_legacy` deterministic — we
refuse based on magic, not based on a heuristic.

---

## Files to modify

| File | Change | Est LOC |
|---|---|---|
| `include/human/persona_crypt.h` | NEW. Public signatures, `hu_persona_format_t`, error code references. | +90 |
| `src/persona/persona_crypt.c` | NEW. All crypto, atomic write, classify predicate. | +420 |
| `src/persona/persona_crypt_keystore_linux.c` | NEW. Linux keyfile backend (0600). | +110 |
| `src/persona/persona_crypt_keystore_darwin.c` | NEW. Keychain via `SecItemCopyMatching`. | +130 |
| `include/human/error.h` | Add `HU_ERR_LEGACY_REFUSED` and `HU_ERR_CRYPTO_*` if missing. | +6 |
| `tests/test_persona_encryption.c` | NEW. ACs 8.2.1–8.2.6 + classify_bytes truth table + wrong-key blocked + atomic-crash test. | +480 |
| `tests/fixtures/persona_plaintext_sample.json` | NEW. Minimal valid persona JSON with non-empty traits/values/example_banks. | +60 |
| `CMakeLists.txt` | Add new sources to `human_core`; gate `persona_crypt_keystore_darwin.c` on `APPLE`, linux variant on `UNIX AND NOT APPLE`; ensure `-DHU_PERSONA_ENCRYPTED` available; add `tests/test_persona_encryption.c` to test target. | +25 |
| `CMakePresets.json` | Confirm `dev` and `test` presets set `HU_ENABLE_LIBSODIUM=ON` (audit only; usually already on). | 0 or +2 |

**Out of scope for this story (explicit non-changes):**

- `src/persona/persona.c` — `hu_persona_load`/`hu_persona_save_json` are
  **not modified**. Wiring is a follow-on story (deliberate; AC-8.2.4 mentions
  a `HU_PERSONA_ENCRYPTED` compile-time gate but the wiring belongs in the
  story that ties this into the daemon's persona load path).
- Auto-migration on daemon startup — explicit "Out of scope" in the story.
- Multi-device sync, Secure Enclave — explicit "Out of scope".

---

## Public API (verbatim signatures the implementer will pin)

```c
/* include/human/persona_crypt.h */

#include "human/error.h"
#include "human/allocator.h"
#include "human/persona.h"
#include <stddef.h>
#include <stdint.h>

#define HU_PERSONA_CRYPT_KEY_BYTES       32u   /* crypto_secretbox_KEYBYTES */
#define HU_PERSONA_CRYPT_NONCE_BYTES     24u   /* crypto_secretbox_NONCEBYTES */
#define HU_PERSONA_CRYPT_MAC_BYTES       16u   /* crypto_secretbox_MACBYTES */
#define HU_PERSONA_CRYPT_HEADER_BYTES    32u   /* magic(4)+ver(1)+rsv(3)+nonce(24) */

typedef enum {
    HU_PERSONA_FORMAT_PLAINTEXT_JSON = 1,
    HU_PERSONA_FORMAT_ENCRYPTED_V1   = 2,
    HU_PERSONA_FORMAT_UNKNOWN        = 3
} hu_persona_format_t;

/* Pure classification predicate — testable without touching disk. */
hu_persona_format_t hu_persona_classify_bytes(const uint8_t *buf, size_t len);

/* Key derivation. On test builds (HU_IS_TEST), both backends route through
 * a keyfile at $HU_PERSONA_KEYFILE_OVERRIDE if set; otherwise the platform
 * default path is used. */
hu_error_t hu_persona_crypt_derive_key(
    hu_allocator_t *alloc,
    const char     *persona_name,   /* nul-terminated; bounded ≤ 64 */
    uint8_t         key_out[HU_PERSONA_CRYPT_KEY_BYTES]);

/* Encrypted save. Atomic via tmp+fsync+rename, mirrors personal_model
 * Phase-0 pattern (see CLAUDE.md M2). */
hu_error_t hu_persona_save_encrypted(
    hu_allocator_t       *alloc,
    const char           *path,
    const hu_persona_t   *persona,
    const uint8_t         key[HU_PERSONA_CRYPT_KEY_BYTES]);

/* Encrypted load. Returns HU_ERR_SECURITY_DENIED on MAC mismatch (wrong key
 * or tampered). Leaves *out untouched on any error. */
hu_error_t hu_persona_load_encrypted(
    hu_allocator_t       *alloc,
    const char           *path,
    const uint8_t         key[HU_PERSONA_CRYPT_KEY_BYTES],
    hu_persona_t         *out);

/* One-shot migration: detect plaintext, encrypt-in-place, shred original.
 * Idempotent: returns HU_OK if the file is already HU_PERSONA_FORMAT_ENCRYPTED_V1. */
hu_error_t hu_persona_migrate_to_encrypted(
    hu_allocator_t *alloc,
    const char     *path,
    const char     *persona_name);

/* Refuse-path loader. Returns HU_ERR_LEGACY_REFUSED if the file at <path>
 * is HU_PERSONA_FORMAT_ENCRYPTED_V1 (post-migration). Returns HU_OK and
 * loads only if the file is unambiguously plaintext JSON AND a sentinel
 * file <path>.migration-pending exists. This is the *only* code path that
 * touches plaintext after migration is the default. */
hu_error_t hu_persona_load_legacy(
    hu_allocator_t *alloc,
    const char     *path,
    hu_persona_t   *out);
```

---

## Migration semantics (load-bearing — read carefully)

**Atomic write pattern** (mirrors `hu_personal_model_save` Phase 0 — see
`CLAUDE.md` M2 entry and `tests/test_personal_model_atomic_save.c`):

1. Build full ciphertext in memory: `magic(4) | ver(1) | rsv(3) | nonce(24) | ct+mac(N)`.
2. Open `<path>.tmp` via `open(O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC, 0600)`. If
   `EEXIST`, fail with `HU_ERR_IO_BUSY` — do NOT overwrite a partial migration
   from a concurrent process.
3. `write` full buffer, check short writes, retry on `EINTR`.
4. `fsync(fd)` — required for crash-safety AC-8.2.5.
5. `close(fd)`.
6. Open parent directory, `fsync(dirfd)`, close — ensures the rename is
   durable.
7. `rename(<path>.tmp, <path>)` — atomic POSIX rename across the same
   filesystem.

**Migration order** (`hu_persona_migrate_to_encrypted`):

1. Open `<path>` read-only, read full contents into a heap buffer.
2. `hu_persona_classify_bytes` on the buffer:
   - `HU_PERSONA_FORMAT_ENCRYPTED_V1` → return `HU_OK` (idempotent).
   - `HU_PERSONA_FORMAT_UNKNOWN` → return `HU_ERR_INVALID_FORMAT`, do nothing.
   - `HU_PERSONA_FORMAT_PLAINTEXT_JSON` → proceed.
3. `hu_persona_load_json` to parse plaintext into a transient `hu_persona_t`.
   On parse failure: free, return `HU_ERR_INVALID_FORMAT`, **leave plaintext
   untouched** (do not delete data we cannot replace).
4. `hu_persona_crypt_derive_key` for this persona name. On failure: free,
   return; leave plaintext untouched.
5. Write `<path>.tmp` with the atomic sequence above.
6. Write a sentinel `<path>.migration-pending` containing the bytes
   `"plaintext-shredded"` via the same atomic write (this is the
   crash-window marker; see Risks).
7. `rename(<path>.tmp, <path>)`.
8. **Shred plaintext-in-buffer**: `sodium_memzero` the heap copy of the
   plaintext JSON before free.
9. **Shred plaintext-on-disk**: open the now-replaced file? No — at step 7
   the plaintext bytes are already gone from `<path>`. The `.bak` of the
   original is what we need to shred. **Refinement: at step 0, before step 2,
   the implementer must `rename(<path>, <path>.legacy)` so we still have the
   original to read while writing the new one.** Then the shred is:
   - `open(<path>.legacy, O_WRONLY)` → `write` one pass of zeros sized to
     the original length → `fsync` → `close` → `unlink(<path>.legacy)`.
10. Remove the sentinel: `unlink(<path>.migration-pending)`.
11. `hu_persona_deinit` the transient persona.

The renamed `.legacy` step is important: if the process dies between step 1
and step 7, `<path>` still has plaintext (no harm; we just restart). If it
dies between step 7 and step 10, `<path>` is encrypted, `<path>.legacy` is
plaintext, and the sentinel tells the recovery path to finish the shred.
Recovery: on next migrate call, if `<path>.migration-pending` exists, run
steps 9–10 and return `HU_OK`.

**Shred discipline:** one-pass zero overwrite then `unlink`. SSDs make
multi-pass overwrite theater (the FTL may not even write to the same cells),
so the threat model here is "casual disk read after deletion", not "forensic
recovery of a discarded SSD" — which is out of scope and would require
filesystem-level secure-delete or full-disk encryption (the user's job).

---

## Implementation steps (for the implementer agent)

Sequenced smallest-reversible-change first. Each step ends with a green
test run.

1. **Header + error code.** Add `include/human/persona_crypt.h` (full
   signatures above). Add `HU_ERR_LEGACY_REFUSED` to `include/human/error.h`
   if it does not exist (grep first; the codebase has a lot of error codes —
   reuse before adding). Add fixture
   `tests/fixtures/persona_plaintext_sample.json`. **Build must compile**;
   no `.c` yet → expect link errors only in test target.

2. **Classify predicate + unit tests (failing).** Add
   `hu_persona_classify_bytes` skeleton returning `HU_PERSONA_FORMAT_UNKNOWN`
   for everything. Write the truth-table tests in
   `tests/test_persona_encryption.c`:
   - `test_classify_empty_buffer_is_unknown`
   - `test_classify_plaintext_starting_with_brace_is_json`
   - `test_classify_plaintext_with_leading_whitespace_is_json`
   - `test_classify_HUP1_magic_is_encrypted_v1`
   - `test_classify_HUP1_magic_truncated_to_header_only_is_unknown`
   - `test_classify_HUP1_magic_with_wrong_version_byte_is_unknown`
   - `test_classify_random_binary_is_unknown`
   - All FAIL (skeleton returns UNKNOWN for everything). Then implement
     `hu_persona_classify_bytes`. All PASS.

3. **Linux keystore backend.** Implement `persona_crypt_keystore_linux.c`:
   read or create `~/.human/keys/persona.key` (mode 0600, parent 0700),
   honor `HU_PERSONA_KEYFILE_OVERRIDE` under `HU_IS_TEST`. Tests:
   - `test_keystore_linux_creates_keyfile_with_0600_perms`
   - `test_keystore_linux_creates_parent_dir_with_0700_perms`
   - `test_keystore_linux_rejects_world_readable_existing_keyfile`
     (asserts `HU_ERR_SECURITY_DENIED` if perms widened — defense in depth)
   - `test_keystore_linux_returns_same_key_on_repeat_call_same_name`
   - `test_keystore_linux_returns_different_keys_for_different_persona_names`
     (HKDF-style derivation from a single master + persona_name; OR keep
     it dead-simple and use one key for all personas — implementer's call,
     but document and test the chosen behavior)

4. **Darwin keystore backend (stub-first).** Implement
   `persona_crypt_keystore_darwin.c` to delegate to the Linux keyfile path
   when `HU_IS_TEST` is defined (no Keychain calls in tests). Outside
   `HU_IS_TEST`, call `SecItemCopyMatching`/`SecItemAdd`/`SecRandomCopyBytes`.
   No new test cases here — the test path is exercised by step 3's tests
   running on macOS via the override. Production darwin path is verified
   manually in `/verify` smoke.

5. **Save + load round-trip.** Implement `hu_persona_save_encrypted` (with
   the full atomic write sequence) and `hu_persona_load_encrypted`. Tests:
   - `test_save_then_load_recovers_all_fields` (AC-8.2.1 happy path)
   - `test_save_produces_HUP1_magic_at_offset_0`
   - `test_save_twice_yields_different_ciphertext_same_plaintext` (AC-8.2.3)
   - `test_load_with_wrong_key_returns_security_denied_and_out_untouched`
     (AC-8.2.2 — adversarial, must assert BLOCKED per `tests-that-pin-bugs.md`)
   - `test_load_with_tampered_ciphertext_returns_security_denied`
   - `test_load_with_truncated_file_returns_invalid_format`
   - `test_save_to_unwritable_dir_returns_io_error_and_no_partial_file`

6. **Migration.** Implement `hu_persona_migrate_to_encrypted` with the
   rename-to-`.legacy` + shred sequence above. Tests:
   - `test_migrate_plaintext_yields_encrypted_round_trippable_file`
     (AC-8.2.1 end-to-end)
   - `test_migrate_is_idempotent_on_already_encrypted_file`
   - `test_migrate_does_not_delete_plaintext_when_parse_fails`
   - `test_migrate_recovers_from_pending_sentinel`
     (set up state mid-migration manually, call migrate, assert it cleans
     up)
   - `test_migrate_crash_after_tmp_before_rename_leaves_original_intact`
     (AC-8.2.5 — simulate by injecting a failure point via an
     `HU_IS_TEST`-only hook that returns from inside the write between
     fsync and rename; assert `<path>` still has the original plaintext
     bytes and is parseable)

7. **Refuse-path loader.** Implement `hu_persona_load_legacy`. Tests
   (these are the AC-8.2.4-flavored tests — every one asserts the
   dangerous case is BLOCKED, per the rule):
   - `test_load_legacy_refuses_encrypted_file_with_legacy_refused_error`
     (the critical anti-regression: this is the test whose name promises
     "refused" and whose assertion is
     `HU_ASSERT_EQ(err, HU_ERR_LEGACY_REFUSED)` AND `out` is untouched)
   - `test_load_legacy_loads_plaintext_only_when_sentinel_present`
   - `test_load_legacy_refuses_plaintext_when_no_sentinel`
   - `test_load_legacy_does_not_populate_out_on_refusal`

8. **Atomic-save adversarial test (mirroring personal_model pattern).**
   Add `test_save_encrypted_preserves_prior_state_when_tmp_blocked` — block
   the `<path>.tmp` slot with a directory, attempt save, assert the prior
   `<path>` (if any) is byte-identical to before.

9. **Run full suite.** `./build/human_tests` — must be 0 failures, 0 ASan
   errors. Then `/verify`.

---

## Acceptance criteria mapping

| AC | Test name(s) |
|---|---|
| AC-8.2.1 (migrate yields encrypted; round-trip) | `test_migrate_plaintext_yields_encrypted_round_trippable_file` + `test_save_then_load_recovers_all_fields` |
| AC-8.2.2 (wrong key → `HU_ERR_SECURITY_DENIED`, out untouched) | `test_load_with_wrong_key_returns_security_denied_and_out_untouched` (adversarial-BLOCKED phrasing) |
| AC-8.2.3 (nonce fresh per save) | `test_save_twice_yields_different_ciphertext_same_plaintext` |
| AC-8.2.4 (gated load refuses plaintext) | `test_load_legacy_refuses_encrypted_file_with_legacy_refused_error` + `test_load_legacy_refuses_plaintext_when_no_sentinel` |
| AC-8.2.5 (crash mid-write leaves original intact) | `test_migrate_crash_after_tmp_before_rename_leaves_original_intact` + `test_save_encrypted_preserves_prior_state_when_tmp_blocked` |
| AC-8.2.6 (DoD references the three public symbols) | All three symbols are called in the test file → `scripts/check-test-references.sh` passes automatically |

---

## Risks (HIGH overall — story is P0/HIGH)

### R1 — Silent downgrade to plaintext (HIGH probability, LARGE impact)

**Failure mode:** `hu_persona_load_legacy` falls through to plaintext load
even after migration, because (a) the sentinel check is wrong, (b) the
classify predicate gets confused, or (c) someone "fixes" a test by relaxing
`HU_ERR_LEGACY_REFUSED` to `HU_OK`.

**Mitigation:**
- The refuse-path test asserts `HU_ASSERT_EQ(err, HU_ERR_LEGACY_REFUSED)`
  and `HU_ASSERT_FALSE(out_was_populated)` — per
  `.claude/rules/tests-that-pin-bugs.md`, the test name and assertion both
  describe the BLOCKED case.
- The classify predicate is the single source of truth (per
  `security-predicate-extraction.md`); both `load_encrypted` and
  `load_legacy` call it, so no second copy of "is this encrypted" exists
  to drift.
- Code review checklist item: any future PR that touches
  `hu_persona_load_legacy` must include a passing `LEGACY_REFUSED` test.

### R2 — Migration crashes mid-way, user loses persona (MEDIUM, LARGE)

**Failure mode:** Process killed between "delete plaintext" and "write
encrypted"; user's persona is gone.

**Mitigation:** The rename-to-`.legacy` + sentinel + recovery sequence
above. At every point in time, at least one of (`<path>`, `<path>.legacy`,
`<path>.tmp`) contains a complete copy of the persona. The recovery path
in `migrate` is itself tested (`test_migrate_recovers_from_pending_sentinel`).

### R3 — libsodium not linked, crypto silently disabled (LOW, LARGE)

**Failure mode:** Someone builds with `HU_ENABLE_LIBSODIUM=OFF` and the new
TU compiles a no-op stub that returns plaintext.

**Mitigation:** `src/persona/persona_crypt.c` begins with:
```c
#if !defined(HU_HAS_LIBSODIUM)
#error "persona_crypt.c requires libsodium; build with -DHU_ENABLE_LIBSODIUM=ON"
#endif
```
There is no fallback. If libsodium is unavailable, the persona system
must not pretend to encrypt.

### R4 — Keyfile world-readable / Keychain unavailable (MEDIUM, MEDIUM)

**Failure mode:** Linux user has `umask 022` so the keyfile lands 0644,
or macOS keychain access denied (locked keychain, sandboxed binary).

**Mitigation:**
- Keystore creates the file with explicit `fchmod(fd, 0600)` after open,
  and verifies on subsequent reads (`stat` + `S_IMODE`), refusing to load
  if perms widened — surfaced as `HU_ERR_SECURITY_DENIED` not silent
  acceptance. Test:
  `test_keystore_linux_rejects_world_readable_existing_keyfile`.
- Darwin: surface keychain errors as `HU_ERR_SECURITY_DENIED` with a
  human-readable `err` buffer; do NOT auto-fall-back to a keyfile.

### R5 — Concurrent migration from two processes (LOW, MEDIUM)

**Failure mode:** Daemon and CLI both try to migrate the same persona file.

**Mitigation:** `open(O_CREAT|O_EXCL)` on `<path>.tmp` and on
`<path>.migration-pending` — the second process gets `EEXIST` and bails
with `HU_ERR_IO_BUSY`. No corruption; just a benign retry.

### R6 — Observability of bad inputs (LOW, SMALL)

**Mitigation:** On `HU_ERR_SECURITY_DENIED` from `load_encrypted`, log
(under `HU_IS_TEST` gated logger to avoid prod noise) `"persona: MAC check
failed at <path>"`. Production observability is a Sprint 9 concern.

### R7 — Performance: per-load Argon2id would be costly (N/A here)

The key is read once per save/load from the keystore (32 bytes of file
I/O or a Keychain call). The crypto is XSalsa20-Poly1305 which is
sub-millisecond for a 10KB persona. No N+1 risk. No latency budget issue.

---

## Test strategy

- **Unit tests in `tests/test_persona_encryption.c`** (24+ cases by my
  count above). All run under ASan. No real Keychain calls (override
  keyfile path under `HU_IS_TEST`).
- **Fixture** at `tests/fixtures/persona_plaintext_sample.json` — a
  realistic but minimal plaintext persona with at least one entry in
  each of `traits`, `values`, `communication_rules`, `example_banks`.
- **No integration test against the daemon** in this story — the daemon
  wiring is a follow-on story and any integration test would couple this
  PR to that work. Manual `/verify` runs a save+load+migrate flow with
  `human persona --encrypt-now <name>` CLI shim (also a follow-on; not in
  this PR).
- **Adversarial discipline:** AC-8.2.2 and AC-8.2.4 are the BLOCKED-case
  tests per `tests-that-pin-bugs.md`. Reviewer must verify that test
  *names* and test *assertions* both describe the blocked path.

---

## Anti-pattern checklist (for code review)

- [ ] No `hu_persona_classify_bytes` open-coded anywhere outside its
      definition. (Grep before merge.)
- [ ] `hu_persona_load_legacy` adversarial test asserts
      `HU_ERR_LEGACY_REFUSED` AND that `out` is untouched.
- [ ] No `SQLITE_TRANSIENT` (no SQLite involved here, but mention by way
      of the project's persistent rule list).
- [ ] All `sodium_*` errors checked; no `(void)` discards on crypto returns.
- [ ] `sodium_memzero` on every transient key buffer before free.
- [ ] No `printf`/`fprintf(stderr)` in production code (use the project's
      logger; per `src/persona/CLAUDE.md` conventions if present).
- [ ] `HU_IS_TEST` guards on keychain access and on the migration-crash
      injection hook.
- [ ] `tests/test_persona_encryption.c` references at least one production
      symbol per `.claude/rules/test-references-production-symbol.md`
      (it references all four public functions — passes by construction).
- [ ] Full suite `./build/human_tests` green after every step, not just at
      the end (per `tests-that-pin-bugs.md`: targeted-green is a lie).

---

## Library availability check (resolved)

| Dependency | Status |
|---|---|
| libsodium | **Already wired** via `HU_ENABLE_LIBSODIUM` in `CMakeLists.txt:61, 1726-1772`. Pkg-config first, manual fallback. Defines `HU_HAS_LIBSODIUM=1`. Resolution at link via `human_core PUBLIC ${HU_LIBSODIUM_LIBRARIES}`. **No new dependency.** |
| Security.framework (darwin Keychain) | Existing macOS link. `find_library(SECURITY_FRAMEWORK Security)` may need to be added to `CMakeLists.txt` if not already linked transitively — implementer to verify before writing the darwin backend, but this is a 2-line CMake addition, not a story-level blocker. |
| Argon2id | Not required for this story. Persona key is random bytes from `/dev/urandom` (or `getrandom(2)`), not derived from a passphrase. The story explicitly chooses XSalsa20-Poly1305, not a passphrase-based scheme. |

No external dependency additions required. Story is ready to implement
under the existing toolchain.

---

## Done when

- All 24+ unit tests green under ASan.
- `./build/human_tests` full suite: 10,000+ tests, 0 failures, 0 ASan
  errors (per project DoD).
- `scripts/check-test-references.sh` passes for `test_persona_encryption.c`.
- `/verify` returns `RESULT_verifier=PASS`.
- Code review confirms the BLOCKED-case test names and assertions are
  consistent.
- `src/persona/persona.c` is **not modified** (no scope creep into the
  wiring story).
