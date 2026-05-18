# Sprint 42 Backlog — "Verifiable Privacy"

## Goal
Replace h-uman's theatrical privacy claims with structurally verifiable guarantees:
real DP-SGD accounting, persona encryption at rest, reproducible builds, signed
artifacts, and a `doctor --privacy` posture command that fails loud when any claim
is false.

## User Stories (in priority order)

---

### US-42.1 (P0): Real DP-SGD with per-sample gradient clipping and RDP accountant

**As a** privacy-conscious user who trains a personal model on-device,
**I want** the DP-SGD implementation to use true per-sample gradient clipping and
a Renyi-DP accountant that converts to (epsilon, delta) via Canonne-Kamath-Steinke
2020,
**so that** the advertised privacy budget is mathematically defensible and not
post-hoc theater.

**Context:** `include/human/ml/learner.h:81-95` describes intent correctly.
`src/ml/learner.c:54-75` shows the existing accountant uses naive linear
accumulation — no RDP moments. `src/ml/learner_cpu.c:321-343` clips per-sample
for the CPU backend but `src/ml/learner_ggml.c:312` warns it is approximate;
`src/ml/learner_mlx.c:307` adds noise post-hoc without confirmed per-sample
clipping. A new `src/ml/dp_sgd.c` provides the canonical implementation all three
backends delegate to.

**Acceptance criteria:**

- **AC-42.1.1** GIVEN `hu_dp_sgd_step()` in `src/ml/dp_sgd.c` (NEW) called with
  a synthetic batch where at least one gradient's L2 norm exceeds `clip_norm = C`,
  WHEN the step executes,
  THEN every output gradient's L2 norm is <= C (deterministic fixture, no PRNG
  needed for the clipping assertion).

- **AC-42.1.2** GIVEN `hu_dp_rdp_to_eps_delta()` implementing Mironov-Talwar-Zhang
  2019 closed-form RDP with Canonne-Kamath-Steinke 2020 conversion,
  WHEN called with `noise_multiplier=1.1`, `sampling_rate=0.01`, `steps=1000`,
  `delta=1e-5` (Opacus oracle fixture),
  THEN the returned epsilon is within +/- 0.05 of the Opacus reference value
  pinned in `tests/test_dp_sgd.c::dp_rdp_opacus_oracle_fixture`.

- **AC-42.1.3** GIVEN `dp_enabled=true` and the RDP accountant tracking cumulative
  spend across multiple `hu_learner_train()` calls,
  WHEN `hu_dp_accountant_total_epsilon()` would exceed the configured `dp_epsilon`
  on the next step,
  THEN training returns `HU_ERR_PRIVACY_BUDGET_EXHAUSTED` before that step executes
  and no weight update is applied.

- **AC-42.1.4** GIVEN the ggml and MLX backends with `dp_enabled=true`,
  WHEN `hu_learner_train()` is called,
  THEN both backends call `hu_dp_sgd_step()` from `src/ml/dp_sgd.c` rather than
  applying ad-hoc noise — confirmed by a `HU_IS_TEST` call counter asserting
  count > 0 after training.

- **AC-42.1.5** GIVEN `dp_enabled=false`,
  WHEN `hu_learner_train()` runs,
  THEN `hu_dp_sgd_step()` is never called (call counter remains 0).

**Estimate:** L
**Risk:** HIGH — incorrect RDP math ships as a false privacy guarantee; every
AC requires a deterministic fixture, not statistical sampling
**Dependencies:** none
**Test seam:** `tests/test_dp_sgd.c` (NEW)
**DoD:** full suite passes, ASan-clean, -Werror clean, /verify PASS, /aspect-panel CLEAN
**Out of scope:** Federated / cross-device DP composition. Privacy amplification
beyond the Poisson subsampling model in the Opacus fixture.

---

### US-42.2 (P1): Persona encryption-at-rest with Keychain/keyfile key storage

**As a** user whose persona files live at `~/.human/personas/*.json` in plaintext,
**I want** those files encrypted with libsodium secretbox (XSalsa20-Poly1305) with
the key stored in macOS Keychain on darwin or a 0600 keyfile on Linux,
**so that** a stolen laptop or disk image does not expose my personal AI identity,
communication style, and example banks in cleartext.

**Context:** `src/security/keystore.c:536` marks `keychain_available = false`
("OS-keychain not implemented yet"). The existing keystore has PBKDF2-HMAC-SHA256
key derivation and ChaCha20+HMAC-SHA256 AEAD (`include/human/crypto.h`). The new
path uses `include/human/persona/crypto.h` (NEW header). `src/doctor_fix.c` already
has a `personas_directory` slot for surfacing encryption status.

**Acceptance criteria:**

- **AC-42.2.1** GIVEN `HU_PERSONA_ENCRYPT=1` in config and `hu_persona_save()`
  writing a persona to disk,
  WHEN the file is read back as raw bytes,
  THEN `json_parse` on those bytes fails (not valid UTF-8 JSON) and the first four
  bytes equal the sentinel magic `\x68\x75\x70\x65` ("hupe").

- **AC-42.2.2** GIVEN an encrypted persona file on disk,
  WHEN `hu_persona_load()` is called with correct key material,
  THEN the returned `hu_persona_t` is field-for-field identical to the original
  pre-encrypt struct.

- **AC-42.2.3** GIVEN a plaintext (pre-migration) persona file,
  WHEN `hu_persona_crypto_migrate()` is called,
  THEN (a) the plaintext file is overwritten with zeros and unlinked, (b) the
  encrypted file is written atomically via tmp+rename (matching the pattern in
  `src/memory/personal_model.c`), and (c) a `.migration_done` sentinel is written;
  any subsequent call to `hu_persona_load()` for a plaintext file returns
  `HU_ERR_SECURITY_DENIED` without reading the file.

- **AC-42.2.4** GIVEN a darwin build with `Security.framework`,
  WHEN `hu_persona_crypto_key_open()` is called,
  THEN the key is retrieved from macOS Keychain (`kSecClassGenericPassword`,
  service `"hu.persona.key"`) and `hu_keystore_status_t.keychain_available`
  returns `true`; on Linux the key is read from `~/.human/.persona_key` with
  mode asserted as 0600 at open time, returning an error if mode is wider.

- **AC-42.2.5** GIVEN a migration interrupted mid-write (simulated by a test that
  truncates the tmp file before rename),
  WHEN the process restarts and `hu_persona_load()` is called,
  THEN the sentinel-pending recovery path retries migration cleanly — no corrupted
  data is returned, no old plaintext is silently loaded.

**Estimate:** L
**Risk:** HIGH — key management bugs cause permanent persona data loss; atomic
migration and shred-on-disk must be ASan-verified
**Dependencies:** none (libsodium; fallback to `include/human/crypto.h` ChaCha20
if libsodium is unavailable — must be documented in `include/human/persona/crypto.h`)
**Test seam:** `tests/test_persona_crypto.c` (NEW)
**DoD:** full suite passes, ASan-clean, -Werror clean, /verify PASS, /aspect-panel CLEAN
**Out of scope:** Keychain biometric (Touch ID) unlock. Encryption of the personal
model SQLite database (separate from persona JSON files).

---

### US-42.3 (P1): Reproducible build flags and deterministic archive generation

**As a** security auditor or downstream packager,
**I want** `human` binaries to be bit-for-bit reproducible when built from the same
source at the same revision,
**so that** I can independently verify that a distributed binary matches its claimed
source tree and that no build-time backdoor has been introduced.

**Context:** `CMakeLists.txt` has no `SOURCE_DATE_EPOCH` plumbing, no
`-Werror=date-time`, no `-ffile-prefix-map`, and no `ar -D`. A grep of `src/`
for `__DATE__` and `__TIME__` returns zero results, meaning non-reproducibility
comes from build-system object file timestamps, not explicit macro usage — the
code-level fix is clean.

**Acceptance criteria:**

- **AC-42.3.1** GIVEN `SOURCE_DATE_EPOCH` set in the environment,
  WHEN `cmake --preset release` and `cmake --build --preset release` run,
  THEN `-DSOURCE_DATE_EPOCH=$(SOURCE_DATE_EPOCH)` is passed to all units via
  `target_compile_definitions` and `-Werror=date-time` is in the compile flags,
  so any future `__DATE__`/`__TIME__` usage fails the build immediately.

- **AC-42.3.2** GIVEN a Linux release build,
  WHEN static archives are produced,
  THEN `ar -D` (deterministic mode, strips timestamps and UIDs/GIDs) is used;
  confirmed by `ar tv libhuman_core.a | awk '{print $4}'` returning all-zero
  timestamps in `scripts/verify-reproducible-build.sh` (NEW).

- **AC-42.3.3** GIVEN a macOS release build,
  WHEN static archives are produced by `libtool` or `ar`,
  THEN the build probes for `ar -D` support and sets `ZERO_AR_DATE=1` via
  `CMAKE_AR_FLAGS` as the darwin fallback; the CI script confirms no
  non-deterministic timestamps in the archive.

- **AC-42.3.4** GIVEN two sequential `cmake --build --preset release` runs on the
  same source tree with `SOURCE_DATE_EPOCH=1700000000`,
  WHEN `sha256sum` compares the two `human` binaries,
  THEN both hashes are identical; this two-build diff is the acceptance test
  run by `scripts/verify-reproducible-build.sh --two-build-diff`.

- **AC-42.3.5** GIVEN `-ffile-prefix-map=$(pwd)=.` in compile flags,
  WHEN debug info embeds source paths,
  THEN `strings build/human | grep "$(pwd)"` returns no matches.

**Estimate:** M
**Risk:** HIGH — misconfigured flags silently reintroduce timestamps; the two-build
diff CI check is the only reliable gate
**Dependencies:** none
**Test seam:** `scripts/verify-reproducible-build.sh` (NEW); CI job in
`benchmark.yml` or new `reproducible.yml` workflow
**DoD:** two-build diff passes in CI, -Werror=date-time enforced, dev ASan build
unaffected, /verify PASS, /aspect-panel CLEAN
**Out of scope:** SBOM generation (separate epic). Reproducibility of Python
fine-tuning scripts (`scripts/finetune-gemma.py` is out of scope per sprint
constraints).

---

### US-42.4 (P1): Ed25519-signed release artifacts with fail-closed init

**As a** user installing h-uman from a release artifact,
**I want** the binary to verify its own Ed25519 signature at startup against an
embedded public key, failing with `HU_ERR_SECURITY_DENIED` if the signature is
absent or invalid,
**so that** a tampered or unsigned binary cannot silently run as a trusted personal
AI daemon.

**Context:** `src/security/audit.c:540` references an audit-log signing key at
0600; no artifact-level signing exists today. The fail-closed pattern
(`rc = HU_ERR_SECURITY_DENIED` as baseline; happy path overwrites) matches the
security predicate discipline in `.claude/rules/security-predicate-extraction.md`.
Pure predicate `hu_artifact_sign_must_reject()` in `src/security/artifact_sign.c`
(NEW) must be testable without spawning a subprocess. `HU_OK`, `HU_ERR_NOT_FOUND`,
and `HU_ERR_SECURITY_DENIED` are distinct return values so `doctor --privacy` can
distinguish "never signed" from "tampered."

**Acceptance criteria:**

- **AC-42.4.1** GIVEN `hu_artifact_verify_signature()` in
  `src/security/artifact_sign.c` (NEW) with the embedded public key,
  WHEN called with a binary that has no appended signature block,
  THEN the return value is `HU_ERR_NOT_FOUND` and the pure predicate
  `hu_artifact_sign_must_reject(HU_ERR_NOT_FOUND)` returns `true`.

- **AC-42.4.2** GIVEN a binary whose appended signature block has been bit-flipped
  at offset +3,
  WHEN `hu_artifact_verify_signature()` is called,
  THEN the return value is `HU_ERR_SECURITY_DENIED` and
  `hu_artifact_sign_must_reject(HU_ERR_SECURITY_DENIED)` returns `true`;
  `hu_artifact_sign_must_reject(HU_OK)` returns `false` (full truth table covered).

- **AC-42.4.3** GIVEN a binary signed with a test key pair generated
  deterministically from a fixed seed in `tests/test_artifact_sign.c`,
  WHEN `hu_artifact_verify_signature()` is called,
  THEN the return value is `HU_OK` and startup proceeds normally.

- **AC-42.4.4** GIVEN any startup path (`human init` or daemon start),
  WHEN signature verification returns anything other than `HU_OK`,
  THEN the process exits non-zero and logs `"artifact signature: DENIED (code=%d)"`
  to stderr before any user data is read or any network connection is opened.

- **AC-42.4.5** GIVEN the binary is unsigned or tampered,
  WHEN `human doctor --privacy` (US-42.5) runs,
  THEN `signature-verified: NO (NOT_FOUND)` or `signature-verified: NO (TAMPERED)`
  is printed, with error code sourced from `hu_artifact_verify_signature()` —
  not re-implemented in the doctor command.

**Estimate:** M
**Risk:** HIGH — fail-open bugs in artifact signing are catastrophic; the pure
predicate must cover the full truth table (NOT_FOUND / SECURITY_DENIED / HU_OK)
per `.claude/rules/security-predicate-extraction.md`
**Dependencies:** US-42.3 (signing a non-reproducible binary undermines the chain)
**Test seam:** `tests/test_artifact_sign.c` (NEW)
**DoD:** full suite passes, ASan-clean, -Werror clean, pure predicate truth table
fully covered, /verify PASS, /aspect-panel CLEAN
**Out of scope:** HSM signing infrastructure. Certificate transparency or TOFU
pinning. Automatic binary update verification (deferred; `HU_ENABLE_UPDATE` is
already a separate flag in `CMakeLists.txt:30`).

---

### US-42.5 (P2): Privacy posture command — `human doctor --privacy`

**As a** user or security auditor,
**I want** `human doctor --privacy` to report the current privacy posture
(DP-epsilon if a training run has occurred, persona-encrypted yes/no,
build-reproducible yes/no, signature-verified yes/no) and exit non-zero if any
claim is false,
**so that** I can verify at a glance that the "privacy by architecture" guarantee
holds on this installation and catch regressions immediately.

**Context:** `src/doctor_fix.c` already has a stub result-array pattern with a
`personas_directory` slot. The `hu_doctor_fix_result_t` struct
(`include/human/doctor_fix.h`) can be extended with four new boolean fields. The
`--privacy` subcommand must not disturb the existing `doctor --fix` path.

**Acceptance criteria:**

- **AC-42.5.1** GIVEN an installation where persona files are encrypted, the binary
  is signed, the build is reproducible, and a training run has occurred with
  `dp_epsilon <= 8.0`,
  WHEN `human doctor --privacy` runs,
  THEN stdout contains all four lines: `persona-encrypted: YES`,
  `build-reproducible: YES`, `signature-verified: YES`, `dp-epsilon: <value>`
  and the process exits 0.

- **AC-42.5.2** GIVEN persona files are NOT encrypted,
  WHEN `human doctor --privacy` runs,
  THEN stdout contains `persona-encrypted: NO`, the process exits 1, and stderr
  contains a remediation hint (e.g. `"run: human persona encrypt"`).

- **AC-42.5.3** GIVEN no prior training run (accountant has zero queries),
  WHEN `human doctor --privacy` runs,
  THEN the dp-epsilon line reads `dp-epsilon: N/A (no training run recorded)` and
  this alone does NOT cause a non-zero exit (absence of training is not a privacy
  failure).

- **AC-42.5.4** GIVEN the binary fails artifact signature verification returning
  `HU_ERR_NOT_FOUND` or `HU_ERR_SECURITY_DENIED`,
  WHEN `human doctor --privacy` runs,
  THEN `signature-verified: NO (NOT_FOUND)` or `signature-verified: NO (TAMPERED)`
  is printed and the process exits 1.

- **AC-42.5.5** GIVEN `human doctor --privacy --json`,
  WHEN the command runs,
  THEN stdout is valid JSON with keys `persona_encrypted`, `build_reproducible`,
  `signature_verified`, `dp_epsilon` (null if no run) and `ok` (boolean AND of
  all checks); `jq .ok` on the output returns the correct boolean in the test.

**Estimate:** S
**Risk:** HIGH — a posture command that returns false-positives is worse than none;
each sub-check must call the real production predicate, not re-implement it
**Dependencies:** US-42.2 (persona-encrypted check), US-42.3 (reproducible check),
US-42.4 (signature-verified check), US-42.1 (dp-epsilon check)
**Test seam:** `tests/test_doctor_privacy.c` (NEW); extends `src/doctor_fix.c`
pattern
**DoD:** all four sub-checks call production predicates (no stubs), exit codes
correct for all combinations tested, JSON output validates with jq, full suite
passes, ASan-clean, /verify PASS, /aspect-panel CLEAN
**Out of scope:** Automated remediation (the command reports, does not fix).
Integration with external compliance dashboards or SBOM tooling.

---

## Non-goals
- We will NOT implement federated learning or cross-device DP composition.
- We will NOT implement full hardware TEE or Secure Enclave attestation (macOS
  Keychain is the v1 darwin substitute; 0600 keyfile for Linux).
- We will NOT generate or publish an SBOM (ed25519 sign + SOURCE_DATE_EPOCH is
  the v1 artifact integrity story).
- We will NOT touch `src/persona/persona.c`, `src/ml/dpo*`,
  `scripts/finetune-gemma.py`, `src/agent/*`, or `src/world_model/*`.
- We will NOT change the existing `doctor --fix` command behavior.

## Open questions for stakeholder
- **Libsodium availability:** US-42.2 prefers libsodium for secretbox. If it is
  not an approved dependency, should the fallback be the existing ChaCha20+HMAC-SHA256
  in `include/human/crypto.h`? This affects the US-42.2 DoD.
- **Signing key custody:** For US-42.4, where does the Ed25519 private key live in
  CI? (GitHub Actions secret? HSM?) The embedded public key is in-tree; the private
  key workflow is out of scope but the CI job needs a decision before implementation.
- **Reproducibility CI placement:** Should the two-build diff check (AC-42.3.4)
  block `release.yml` or run as a separate `reproducible.yml` nightly? This affects
  the enforcement point in US-42.3's DoD.

RESULT_product-owner=READY
