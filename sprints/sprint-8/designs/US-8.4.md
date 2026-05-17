# Design for US-8.4: Signed Release Artifacts with Embedded Public Key

**Sprint:** 8
**Priority:** P1
**Risk tier:** HIGH (supply-chain; any default-allow path is a breach)
**Estimate:** M
**Dependencies:** US-8.3 (reproducible build) must merge first
**Story:** `sprints/sprint-8/stories.md` lines 155–192

---

## Approach

Add a small, audited translation unit `src/security/artifact_sign.c` that
exposes a single high-level entry point: `hu_artifact_sign_verify(alloc,
artifact_path, sig_path, err, err_len) -> hu_error_t`. The function reads
the artifact and detached signature into memory, calls libsodium's
`crypto_sign_verify_detached` against an embedded 32-byte ed25519 public
key (`hu_release_pubkey`), and returns one of exactly three outcomes:

| Outcome | Return code | Meaning |
|---|---|---|
| OK | `HU_OK` | Signature is valid for this artifact under the embedded key. |
| MISSING | `HU_ERR_NOT_FOUND` | No `.sig` file on disk (could be a dev build). |
| INVALID | `HU_ERR_SECURITY_DENIED` | `.sig` exists but does not verify (tampered or wrong key). |

These three outcomes map 1:1 to AC-8.4.1, AC-8.4.2, AC-8.4.3 and to the
`doctor --privacy` reporting in US-8.5 (`VALID | MISSING | INVALID`).

**Why libsodium, not OpenSSL.** The codebase already has a libsodium
integration path (`HU_ENABLE_LIBSODIUM`, CMakeLists.txt line 1734–1778)
with a graceful PBKDF2/ChaCha20 fallback. Reusing that toggle means no
new dependency surface. Releases are always built with
`HU_ENABLE_LIBSODIUM=ON` (this is asserted at build time via a
`#if !defined(HU_ENABLE_LIBSODIUM)` `#error` inside `artifact_sign.c` —
see "Build-time guard" below). Dev builds without libsodium compile out
the verify function entirely and `doctor --privacy` reports
`signature: NOT_COMPILED_IN`.

**Why the design factors a pure predicate.** Per
`.claude/rules/security-predicate-extraction.md`, the security decision
("must we reject this signature?") is extracted into a pure predicate
`hu_artifact_sign_must_reject(verify_status, sig_present, key_sane)` →
`hu_error_t`. The verify function does I/O and key crypto; the predicate
encodes the deny/allow truth table and is unit-tested exhaustively
without disk I/O.

**Why an embedded key, not a key file.** A signature check whose trust
anchor lives outside the binary is trivially bypassable (replace the key
file). The 32-byte ed25519 public key is a compile-time constant in
`include/human/signing/release_pubkey.h`. The private key never enters
the repo; it lives only in GitHub Actions secret
`RELEASE_SIGNING_KEY` (base64 ed25519 seed) and is loaded into a
tmpfs-backed file inside the release runner, used once, and dropped.

**Why a separate header for the public key.** Reviewers and downstream
consumers must be able to inspect the embedded trust anchor without
reading 500 lines of crypto code. `include/human/signing/release_pubkey.h`
is ~20 lines: a 32-byte array + a generation provenance comment + a
`HU_RELEASE_PUBKEY_FINGERPRINT` short SHA-256 prefix that the verify
function logs on failure (no secrets — public key only).

---

## Files to create / modify

| File | Change | Estimated LOC |
|---|---|---|
| `include/human/signing/release_pubkey.h` | NEW. `const uint8_t hu_release_pubkey[32]` + fingerprint constant + provenance comment. | +40 |
| `include/human/artifact_sign.h` | NEW. Public prototype + outcome enum + error-buffer contract. | +60 |
| `src/security/artifact_sign.c` | NEW. `hu_artifact_sign_verify` + pure predicate `hu_artifact_sign_must_reject` + path resolution helper for `/proc/self/exe` / `_NSGetExecutablePath`. | +220 |
| `src/security/CMakeLists.txt` *(or top-level CMakeLists.txt)* | Add `artifact_sign.c` to `human_core`; link against libsodium only when `HU_ENABLE_LIBSODIUM=ON`. | +6 |
| `src/doctor.c` | Add `doctor_check_release_signature` that calls `hu_artifact_sign_verify` on the resolved exe path and emits a `HU_DIAG_OK / WARN / ERR` line. Gated on `--privacy` (US-8.5 wires the flag). | +60 |
| `tests/test_artifact_sign.c` | NEW. Fixture-driven tests: known_good, tampered, missing, wrong_key, round-trip, key-sanity. | +280 |
| `tests/test_artifact_sign_predicate.c` | NEW. Truth-table tests for the pure predicate. | +90 |
| `tests/fixtures/signing/known_good.bin` | NEW. 256 bytes of deterministic content. | binary |
| `tests/fixtures/signing/known_good.bin.sig` | NEW. ed25519 detached sig signed by `tests/fixtures/signing/test_privkey.seed`. | binary |
| `tests/fixtures/signing/tampered.bin` | NEW. `known_good.bin` with byte at offset 13 XOR'd with 0x01. | binary |
| `tests/fixtures/signing/wrong_key.bin` + `.sig` | NEW. Signed by a different ed25519 keypair (also in fixtures). | binary |
| `tests/fixtures/signing/test_pubkey.h` | NEW. Test-only public key embedded for fixture tests. **Must not be confused with prod `hu_release_pubkey`.** | +20 |
| `tests/fixtures/signing/README.md` | NEW. How fixtures were generated; **explicitly states the test private key is intentionally checked in** and is NOT the production key. | +30 |
| `scripts/sign-artifacts.sh` | NEW. `sign-artifacts.sh <privkey_seed_file> <artifact_path>` → writes `<artifact_path>.sig`. Uses libsodium CLI if present, else `openssl pkeyutl -sign` with Ed25519. | +60 |
| `scripts/gen-release-keypair.sh` | NEW. One-shot keypair generator. Prints pubkey hex + base64 seed. Used once at sprint start to produce the embedded key; not invoked by CI. | +30 |
| `.github/workflows/release.yml` | Add `sign-artifacts` step after the build step, before `actions/upload-release-asset`. Uploads `<binary>.sig` as a release asset. Skips signing on dry-run. | +35 |
| `docs/security/signing-key-rotation.md` | NEW. Document (do not implement): how to rotate the embedded key, the compatibility window during transition, the procedure for revocation. | +120 |
| `.github/secret_scanning.yml` *(or pre-commit)* | NEW grep guard: `git grep -E 'BEGIN (PRIVATE|OPENSSH) KEY|RELEASE_SIGNING_KEY=[A-Za-z0-9+/]{20,}'` must return zero. | +10 |

Approximate total: ~960 LOC across ~16 new/modified files (excluding
fixtures).

---

## Public API contract (the implementer follows this exactly)

```c
/* include/human/artifact_sign.h */

#define HU_ARTIFACT_SIGN_ERR_LEN 256

/* Outcomes (return codes; the enum is documentation-only). */
typedef enum {
    HU_ARTIFACT_SIGN_OUTCOME_OK       = 0, /* HU_OK */
    HU_ARTIFACT_SIGN_OUTCOME_MISSING  = 1, /* HU_ERR_NOT_FOUND */
    HU_ARTIFACT_SIGN_OUTCOME_INVALID  = 2, /* HU_ERR_SECURITY_DENIED */
    HU_ARTIFACT_SIGN_OUTCOME_INTERNAL = 3, /* HU_ERR_INTERNAL — read failure, OOM, etc.
                                              treated as INVALID by callers per
                                              fail-closed contract */
} hu_artifact_sign_outcome_t;

hu_error_t hu_artifact_sign_verify(hu_allocator_t *alloc,
                                   const char    *artifact_path,
                                   const char    *sig_path,
                                   char          *err,
                                   size_t         err_len);

/* Pure predicate (extracted per security-predicate-extraction.md). */
hu_error_t hu_artifact_sign_must_reject(bool sig_file_present,
                                        bool sodium_verify_passed,
                                        bool pubkey_is_sane);

/* Resolves the running executable's path. Caller frees. */
hu_error_t hu_artifact_sign_resolve_self_path(hu_allocator_t *alloc, char **out_path);
```

**Fail-closed invariant.** Any branch inside `hu_artifact_sign_verify`
that cannot complete (read error, OOM, libsodium init failure, internal
state corruption) MUST return `HU_ERR_SECURITY_DENIED` (or
`HU_ERR_INTERNAL` which the caller treats as identical for trust
purposes). Never return `HU_OK` on an unhandled branch. The pure
predicate enforces this: `must_reject(false, false, false)` returns
`HU_ERR_SECURITY_DENIED`, not `HU_OK`. The only `HU_OK` path is
`(sig_present=true, verify_passed=true, key_sane=true)`.

---

## Implementation steps (for the implementer agent)

1. **Generate the production keypair, offline, once.** Run
   `scripts/gen-release-keypair.sh` on a clean machine. Commit only the
   public-key hex into `include/human/signing/release_pubkey.h`. Add
   the base64 seed to the repo's GitHub Actions secret
   `RELEASE_SIGNING_KEY`. **Never touch the seed again from the
   filesystem; immediately wipe.** Record the public-key fingerprint
   (first 16 hex of SHA-256) in `docs/security/signing-key-rotation.md`
   as the "current production key" entry.
2. **Create the public-key header.** Empty 32-byte stub with a `TODO`
   comment marking the unreplaced value, AND a build-time assertion
   that the header has been updated:
   `_Static_assert(sizeof(hu_release_pubkey) == 32, "...");`
   Step 1's keypair replaces the stub.
3. **Stub the public header and the C file** with prototypes and `return
   HU_ERR_NOT_IMPLEMENTED;` bodies. Verify the project builds.
4. **Write the predicate first.** Implement
   `hu_artifact_sign_must_reject` as a 12-line truth-table function.
   Write `tests/test_artifact_sign_predicate.c` with 8 test cases (2^3
   inputs) BEFORE filling in the verify function. All 8 pass.
5. **Generate test fixtures.** Use a separate test-only keypair (NOT
   the production key). Write `tests/fixtures/signing/README.md`
   explaining provenance and that the test private key is intentionally
   checked in. Add fixtures to the build (CMake `configure_file` or
   copy-to-build-dir step so the test can find them at runtime).
6. **Implement `hu_artifact_sign_resolve_self_path`** with `#ifdef
   __linux__` (readlink `/proc/self/exe`) and `#ifdef __APPLE__`
   (`_NSGetExecutablePath`). Test on both platforms in CI.
7. **Implement `hu_artifact_sign_verify`.** mmap or read the artifact
   into memory (cap at 1 GiB — any larger artifact is a corruption
   signal, return `HU_ERR_SECURITY_DENIED`). Read `.sig` (must be
   exactly 64 bytes). Call `crypto_sign_verify_detached`. Route the
   result through the predicate. Populate `err` with a fingerprint-only
   message ("signature did not verify under release key
   fingerprint=ABCDEF...") — never log the artifact path nor the
   signature bytes verbatim.
8. **Write the fixture-driven tests** (AC-8.4.1 through AC-8.4.5):
   - `test_artifact_sign_verifies_known_good_returns_ok` (AC-8.4.1)
   - `test_artifact_sign_rejects_tampered_returns_security_denied` (AC-8.4.2)
   - `test_artifact_sign_missing_sig_returns_not_found` (AC-8.4.3)
   - `test_artifact_sign_round_trip_via_sign_script` (AC-8.4.4 — invokes
     `scripts/sign-artifacts.sh` in a guarded test, skipped if
     `HU_IS_TEST` lacks shell access)
   - `test_artifact_sign_embedded_pubkey_is_sane` (AC-8.4.5)
   - `test_artifact_sign_wrong_key_returns_security_denied` (defense in
     depth, beyond AC)
   - `test_artifact_sign_oversized_artifact_returns_security_denied`
     (1 GiB cap)
   - `test_artifact_sign_sig_wrong_length_returns_security_denied`
     (anything ≠ 64 bytes)
9. **Run the full suite** (per `tests-that-pin-bugs.md`: targeted-green
   is insufficient on a security boundary). Confirm 10,000+ tests still
   pass with 0 ASan errors.
10. **Wire the release workflow.** Add `Sign release artifacts` step in
    `release.yml` after build, before upload. The step writes the seed
    to `${RUNNER_TEMP}/release.seed` (chmod 600, tmpfs), calls
    `scripts/sign-artifacts.sh`, immediately `shred -u`s the seed file,
    and uploads the resulting `.sig` as an additional release asset.
11. **Wire the doctor check** (gated for US-8.5 to flip on). Function
    exists; `--privacy` flag is added in US-8.5.
12. **Write `docs/security/signing-key-rotation.md`.** Documentation
    only; no code. Cover: current key fingerprint, rotation procedure
    (generate new key, dual-sign window of one release, then retire
    old), revocation path (emergency: ship a release that refuses to
    run if its own `.sig` matches a known-bad fingerprint).
13. **Add the secret-scanning guard.** `git grep` for private-key
    patterns; CI fails if a match is found. Test by attempting to
    commit a deliberate fake key and confirming the guard fires.
14. **Run `/verify` and confirm `RESULT_verifier=PASS`** before
    marking the task complete.

---

## Risks

### R1. Default-allow on any error path (HIGH / LARGE) — supply-chain breach
**What could go wrong:** an unhandled libsodium init failure, an
allocation failure mid-read, or a path-resolution error returns `HU_OK`
because the implementer wrote `if (err) return err;` and forgot the
default case. A tampered binary is now reported `signature: VALID`.

**Mitigation:**
- Pure predicate forces every (sig_present, verify_passed, key_sane)
  combo to be explicitly handled. The truth table is locked in 8
  unit tests.
- Top of `hu_artifact_sign_verify`: initialize a local
  `hu_error_t rc = HU_ERR_SECURITY_DENIED;` and only assign `HU_OK`
  in the single happy-path branch after the predicate confirms.
- Code review checklist item (mandatory): "Does any branch in
  artifact_sign.c return HU_OK without going through
  hu_artifact_sign_must_reject?"
- The adversarial tests assert the dangerous case is BLOCKED
  (per `.claude/rules/tests-that-pin-bugs.md`):
  `HU_ASSERT_EQ(rc, HU_ERR_SECURITY_DENIED)` — NOT
  `HU_ASSERT_TRUE(rc != HU_OK)` (which would pass on `HU_ERR_OOM` too,
  hiding a routing bug).

### R2. Private key leakage into the repo (HIGH / LARGE)
**What could go wrong:** the seed file from
`scripts/gen-release-keypair.sh` gets committed, or the GitHub
Actions step echoes the seed into a log line. Once leaked, every
"signed" release is forgeable; rotation requires a full key-rollover
ceremony.

**Mitigation:**
- `gen-release-keypair.sh` writes the seed only to stdout with an
  explicit "DO NOT REDIRECT TO A FILE IN THE REPO" warning header
  AND prepends `.gitignore` entries for `*.seed` and `release.key`.
- `release.yml` uses `::add-mask::` on the seed before any other step
  can run; the `shred -u` happens in a `finally`-style trap so a
  failed sign step still wipes the seed.
- Pre-commit hook: `git diff --cached | grep -E 'BEGIN (PRIVATE|
  OPENSSH) KEY'` blocks the commit.
- CI guard from step 13.

### R3. Key rotation not planned for, key compromise blocks all releases (MEDIUM / LARGE)
**What could go wrong:** the embedded `hu_release_pubkey` is compromised
(insider threat, leaked secret, lost hardware). With no rotation plan,
every existing binary becomes untrustworthy and there is no in-band
recovery — users must download a "new" binary they cannot verify.

**Mitigation:**
- `docs/security/signing-key-rotation.md` (step 12) documents the
  procedure even though rotation is not implemented in this story.
- The header `include/human/signing/release_pubkey.h` is structured to
  accept future expansion to an array of keys:
  ```c
  /* Future: extend to const uint8_t hu_release_pubkeys[N][32]; */
  ```
  The current implementation hard-codes N=1; the verify function
  accepts a single key. Adding N>1 is a future story, not this one.
- Out-of-band trust anchor: the public key fingerprint is published in
  `docs/security/signing-key-rotation.md` AND will be cross-posted to
  the website (US-8.5 follow-on). A user with a tampered binary AND a
  tampered website still has a third trust anchor: the git commit SHA
  of the header.

### R4. `/proc/self/exe` / `_NSGetExecutablePath` returns a symlink target attacker controls (MEDIUM / MEDIUM)
**What could go wrong:** on Linux, an attacker who can write to the
process's working directory or `/proc` namespace could redirect the
self-path resolution to a benign binary while the running binary is
tampered. The verify check passes against the benign decoy.

**Mitigation:**
- `readlink /proc/self/exe` returns the kernel-resolved path of the
  actual executing binary, not a userspace-controlled symlink. We use
  the file descriptor from `/proc/self/exe` directly (open + fstat +
  read), not the resolved path string. The path string is only used
  to locate `<path>.sig`.
- Document this in a header comment on
  `hu_artifact_sign_resolve_self_path`.
- This is residual risk we accept: an attacker with arbitrary file
  write in `/proc` already owns the system; signature verification is
  not the right defense at that layer.

### R5. Libsodium not linked at release time (LOW / LARGE)
**What could go wrong:** a release is built without
`HU_ENABLE_LIBSODIUM=ON`. Verify is compiled out. `doctor --privacy`
silently reports `signature: NOT_COMPILED_IN` and users have no
mechanism to detect tampering.

**Mitigation:**
- `release.yml` explicitly sets `-DHU_ENABLE_LIBSODIUM=ON` in the
  CMake configure step.
- A CI check after build: `nm build/human | grep -q
  hu_artifact_sign_verify || exit 1`. If the symbol is absent, the
  release is rejected.
- Build-time `#if !defined(HU_ENABLE_LIBSODIUM)` inside
  `artifact_sign.c` emits a `#warning` (not `#error`, because dev
  builds should still compile). The doctor's `NOT_COMPILED_IN` line
  is `HU_DIAG_WARN`, not `HU_DIAG_OK`.

### R6. Signature file format ambiguity (LOW / MEDIUM)
**What could go wrong:** the `.sig` file could be raw 64 bytes, hex,
base64, PEM-wrapped, or PGP-style ASCII-armored depending on which
signer is used. Implementer A writes raw; implementer B's sign script
writes hex; verify fails on real releases.

**Mitigation:**
- Specification: `.sig` is **exactly 64 raw bytes** (the ed25519
  signature). No header, no armor, no newline. Documented in
  `include/human/artifact_sign.h`.
- `scripts/sign-artifacts.sh` writes exactly 64 bytes and the
  round-trip test (AC-8.4.4) confirms.
- Test #8 (sig wrong length) explicitly rejects 65-byte (newline-
  appended) and 128-byte (hex-encoded) inputs.

### R7. Backward compatibility for unsigned dev builds (LOW / SMALL)
**What could go wrong:** developers running locally-built binaries get
`signature: INVALID` warnings on every `doctor` run, leading them to
ignore real warnings later.

**Mitigation:**
- The OK/MISSING/INVALID distinction matters: dev builds have no `.sig`
  → `MISSING` (a `WARN`, not an `ERR`). Only INVALID (sig exists and
  fails) is an `ERR`.
- AC-8.4.3 pins this: missing returns `HU_ERR_NOT_FOUND`, not
  `HU_ERR_SECURITY_DENIED`.

### R8. Observability gap on production failures (LOW / MEDIUM)
**What could go wrong:** a real tamper event occurs in the wild and we
have no telemetry: the user sees `signature: INVALID` and contacts
support, but we cannot correlate fingerprints or counts.

**Mitigation:**
- The `err` buffer (AC-8.4.2) includes the public-key fingerprint
  (first 16 hex of SHA-256) of the embedded key. If the user reports
  a different fingerprint than expected, that is itself diagnostic
  signal (key swap vs. data tamper).
- Future story (not this one): aggregate anonymous tamper-detection
  counts via opt-in telemetry. Out of scope for US-8.4.

---

## Test strategy

### Predicate tests (`tests/test_artifact_sign_predicate.c`)
Exhaustive truth table over 3 booleans (sig_present, verify_passed,
key_sane) = 8 cases. Only `(true, true, true)` returns `HU_OK`. Of the
other 7, the one `(false, *, *)` returns `HU_ERR_NOT_FOUND`; the rest
return `HU_ERR_SECURITY_DENIED`.

### Verify tests (`tests/test_artifact_sign.c`)
Fixture-driven, 8 cases:

| Test | Setup | Expected |
|---|---|---|
| AC-8.4.1 happy path | `known_good.bin` + `known_good.bin.sig` (test key) | `HU_OK` |
| AC-8.4.2 tampered | `tampered.bin` + `known_good.bin.sig` | `HU_ERR_SECURITY_DENIED`; `err` contains "did not verify" |
| AC-8.4.3 missing | `known_good.bin`, no `.sig` | `HU_ERR_NOT_FOUND` |
| AC-8.4.4 round trip | invoke `sign-artifacts.sh` then verify | `HU_OK` |
| AC-8.4.5 key sanity | read `hu_release_pubkey` compile-time | not all 0x00, not all 0xFF |
| wrong key | `wrong_key.bin` + `wrong_key.bin.sig` (verified with prod key) | `HU_ERR_SECURITY_DENIED` |
| oversized | 2 GiB sparse file | `HU_ERR_SECURITY_DENIED` |
| bad sig length | 32-byte and 128-byte `.sig` | `HU_ERR_SECURITY_DENIED` |

All adversarial tests assert the dangerous case is **BLOCKED**:
`HU_ASSERT_EQ(rc, HU_ERR_SECURITY_DENIED)` — never
`HU_ASSERT_TRUE(rc != HU_OK)`.

### Build / link tests
- `tests/test_artifact_sign_link.c` references `hu_artifact_sign_verify`
  to satisfy `.claude/rules/test-references-production-symbol.md`.
- CI step: `nm build/human | grep hu_artifact_sign_verify`.

### Integration test
- `tests/test_doctor_release_signature.c`: invokes `hu_doctor` against a
  test binary fixture in a tmpdir; expects three test runs producing
  OK / MISSING / INVALID lines.

### Not in scope for tests (deferred to US-8.5)
- The `human doctor --privacy` CLI flag wiring is in US-8.5.

---

## Acceptance criteria mapping

| AC | Where covered | Notes |
|---|---|---|
| AC-8.4.1 (known good → `HU_OK`) | `test_artifact_sign_verifies_known_good_returns_ok` | Uses test fixture + test pubkey, not prod key, via a `#ifdef HU_TEST_OVERRIDE_RELEASE_PUBKEY` injection point. The override is `HU_IS_TEST`-gated and absent in release builds. |
| AC-8.4.2 (flipped sig byte → `HU_ERR_SECURITY_DENIED` + err msg) | `test_artifact_sign_rejects_tampered_returns_security_denied` | Asserts BLOCKED, not accepted. |
| AC-8.4.3 (missing `.sig` → `HU_ERR_NOT_FOUND`) | `test_artifact_sign_missing_sig_returns_not_found` | Distinct from SECURITY_DENIED; doctor renders as WARN not ERR. |
| AC-8.4.4 (round trip sign + verify) | `test_artifact_sign_round_trip_via_sign_script` | Invokes `scripts/sign-artifacts.sh`; skipped if `HU_IS_TEST` lacks shell. |
| AC-8.4.5 (pubkey is not 0x00…/0xFF…) | `test_artifact_sign_embedded_pubkey_is_sane` | Compile-time sanity check on `hu_release_pubkey`. |

---

## Naming note for the parent

The brief uses `src/security/signature.c` / `hu_signature_verify(...,
const uint8_t pubkey[32])`. The story ACs (canonical) use
`src/security/artifact_sign.c` / `hu_artifact_sign_verify(alloc, path,
sig_path, err, err_len)` with the public key **embedded** (not passed
as parameter). This design follows the ACs (the contract). The verify
function signature differs from the brief on three points:

1. Name: `hu_artifact_sign_verify` (per AC-8.4.1).
2. Key: embedded `hu_release_pubkey`, not a parameter (per "embedded
   public key" in the story title and AC-8.4.5).
3. Error reporting: out-buffer `err, err_len` (per AC-8.4.2 "the err
   buffer contains a human-readable message"), not just a return code.

If the parent intended the brief's signature literally, bounce back.
Otherwise the AC is the contract and we proceed.

---

## Out of scope (explicitly)

- CI auto-signing of every PR build (story says release only).
- macOS `codesign` notarization (different trust chain).
- SBOM signing (US-8.5).
- Key rotation implementation (documented only).
- Multi-key support (header structured to allow it; implementation is
  N=1).
- Telemetry on tamper events.
