# Sprint 8 Backlog — Verifiable Privacy

## Header

| Field              | Value                                                                                   |
|--------------------|-----------------------------------------------------------------------------------------|
| Sprint             | 8                                                                                       |
| Goal               | Replace theatrical privacy claims with structurally verifiable ones: real DP accounting, encrypted personas, reproducible builds, signed artifacts, and a single command that reports the truth about all four. |
| Dates              | 2026-05-17 — 2026-05-28 (10 working days)                                              |
| Scrum-master       | TBD                                                                                     |
| Branch             | sprint-8-verifiable-privacy                                                             |
| Working directory  | `/Users/sethford/Projects/h-uman/.claude/worktrees/interesting-engelbart-588b03`       |
| Base SHA           | ea02b08e                                                                                |
| Sprint-7 no-touch  | `src/ml/dpo*`, `scripts/finetune-gemma.py`, `src/ml/cli.c`                             |

---

## Sprint Metadata

| Metric                  | Value                        |
|-------------------------|------------------------------|
| Story count             | 5                            |
| P0 stories              | 2 (US-8.1, US-8.2)          |
| P1 stories              | 2 (US-8.3, US-8.4)          |
| P2 stories              | 1 (US-8.5)                  |
| HIGH-risk stories       | 4 (US-8.1, US-8.2, US-8.4, US-8.5) |
| MEDIUM-risk stories     | 1 (US-8.3)                  |
| Total estimate          | S + M + S + M + S = ~18 dev-days across 2 waves |
| Wave 1 (parallel)       | US-8.1 + US-8.2 + US-8.3    |
| Wave 2 (depends on W1)  | US-8.4 → US-8.5             |

---

## User Stories (in priority order)

---

### US-8.1 (P0): Real DP-SGD with RDP Accounting

**As a** privacy-conscious user running local personalization training,  
**I want** the DP-SGD implementation to apply per-sample gradient clipping and track (ε, δ) via a Rényi Differential Privacy (RDP) moments accountant,  
**so that** the privacy guarantee printed in `human doctor --privacy` is a mathematically meaningful bound, not a post-hoc label.

**Context on the current defect:**  
`src/ml/learner_cpu.c` clips the aggregated gradient of the entire weight vector and calls it "per-sample." There is only one "sample" — the whole batch — so the Gaussian noise added afterward provides zero formal DP guarantee per Abadi et al. 2016. `learner.c::hu_dp_accountant_record_query` accumulates `epsilon_step` additively with no composition theorem; that is naive composition, which is strictly looser than RDP or zCDP. The `epsilon_spent` field is reported to users as if it were a formal ε bound.

**What must change:**  
A new translation unit `src/ml/dp_sgd.c` (gated `HU_ENABLE_ML`) exposes:
- A pure predicate `hu_dp_sgd_noise_sigma(double clip_norm, double target_epsilon, double target_delta, size_t steps, size_t dataset_size, double sample_rate)` → `double` — computes the Gaussian noise multiplier via the RDP moments accountant calibration formula (moments order α from 2 to 64, closed-form RDP for sub-sampled Gaussian mechanism, convert to (ε, δ) via δ = min_α exp((α-1)(RDP_α - log((α-1)/α) - (log δ + log α)/(α-1)))). The predicate is pure; no I/O.
- A pure predicate `hu_dp_rdp_epsilon_from_sigma(double sigma, double sample_rate, size_t steps, double delta)` → `double` — inverse: given a completed run, what ε was actually consumed?
- `hu_dp_sgd_step(...)` — takes a per-sample gradient matrix (batch × params), clips each row independently to `clip_norm`, sums, adds calibrated Gaussian noise, and returns the noisy aggregate gradient. Caller is responsible for the optimizer step. The batch dimension must be ≥ 1; the function returns `HU_ERR_INVALID_ARGUMENT` if `batch_size < 1`.
- `hu_dp_accountant_rdp_record(hu_dp_accountant_t *, double sigma, double sample_rate)` and `hu_dp_accountant_rdp_epsilon(const hu_dp_accountant_t *, double delta)` → `double` — replaces the additive accumulator in `learner.c` with proper RDP composition.

Sprint-7's `src/ml/learner_cpu.c` DP path should remain as-is for now; this story delivers the correct module. Sprint-7 can compose it in a follow-on.

**Acceptance criteria:**

- AC-8.1.1: GIVEN `hu_dp_sgd_noise_sigma` is called with `target_epsilon=8.0, target_delta=1e-5, steps=1000, dataset_size=10000, sample_rate=0.01, clip_norm=1.0`, WHEN the result is checked, THEN `sigma >= 0.5` and `sigma <= 5.0` (sanity-bounds the calibrated multiplier against known good values from the Opacus reference implementation for the same parameters). Test asserts exact inequality; documented expected value is `~1.1` per Abadi et al. Table 1 comparable workload.
- AC-8.1.2: GIVEN a batch of 4 per-sample gradients where sample 0 has L2 norm 3.0 (above `clip_norm=1.0`) and samples 1-3 have norm 0.5 each, WHEN `hu_dp_sgd_step` is called with `seed=42` and the output noise is zeroed out (sigma=0), THEN the returned aggregate gradient has L2 norm ≤ `4 * clip_norm` (4.0) and sample 0's contribution is scaled to norm exactly 1.0 ± 1e-5.
- AC-8.1.3: GIVEN a training run of 100 steps at `sample_rate=0.01, sigma=1.1, delta=1e-5`, WHEN `hu_dp_accountant_rdp_epsilon` is called, THEN the returned ε is strictly less than 8.0 (the budget we calibrated for), confirming RDP composition gives tighter bounds than naive addition.
- AC-8.1.4: GIVEN two calls to `hu_dp_sgd_step` with identical inputs and identical PRNG seed, WHEN the output gradients are compared byte-for-byte, THEN they are identical (determinism contract).
- AC-8.1.5: GIVEN `batch_size=0`, WHEN `hu_dp_sgd_step` is called, THEN it returns `HU_ERR_INVALID_ARGUMENT` without writing output or crashing.

**Estimate:** M  
**Risk tier:** HIGH (DP math correctness is security-sensitive; wrong σ means no real guarantee)  
**Dependencies:** none  
**Test seam:** `tests/test_dp_sgd.c`  
**DoD:** full suite 10,000+ tests green, 0 ASan errors; `/verify` PASS; `/aspect-panel` CLEAN; `tests/test_dp_sgd.c` references `hu_dp_sgd_step`, `hu_dp_sgd_noise_sigma`, `hu_dp_accountant_rdp_epsilon` (production symbols, not re-implementations); adversarial ACs (8.1.2, 8.1.5) assert the dangerous case is BLOCKED, not accepted.

**Out of scope:**  
- Wiring `dp_sgd.c` into `learner_cpu.c` or any existing learner (sprint-7 surface; defer to sprint-9 composition story).
- zCDP or f-DP accountants (RDP is sufficient for v1).
- GPU/MLX implementation of the per-sample clip step.

---

### US-8.2 (P0): Encryption-at-Rest for Persona Files

**As a** user who stores personal identity data in `~/.human/personas/*.json`,  
**I want** those files to be encrypted at rest with a key derived from platform-native secret storage (macOS Keychain on darwin; a 0600-perms key file on Linux),  
**so that** an attacker with disk read access cannot extract my persona without also compromising the Keychain or key file.

**Context on the current state:**  
`hu_persona_load` in `src/persona/persona.c` reads plaintext JSON directly from `~/.human/personas/<name>.json`. There is no encryption layer. The persona struct contains `name`, `traits`, `communication rules`, `values`, `decision style`, and `example_banks` — all identity-sensitive. `src/security/vault.c` and `src/security/keystore.c` exist but are not wired into the persona I/O path.

**What must change:**  
A new translation unit `src/persona/persona_crypt.c` exposes:

- `hu_persona_crypt_derive_key(hu_allocator_t *, const char *persona_name, uint8_t key_out[32], char *err, size_t err_len)` → `hu_error_t` — derives a 32-byte key. On darwin: stores/retrieves key from Keychain under service `ai.human.persona`, account `<persona_name>`, generating a random key on first call. On Linux: derives from a per-install 256-bit secret at `~/.human/secrets/master.key` (mode 0600, generated if absent) using HKDF-SHA256 with info `"persona:<name>"`.
- `hu_persona_save_encrypted(hu_allocator_t *, const char *path, const hu_persona_t *, const uint8_t key[32])` → `hu_error_t` — serializes to JSON, encrypts with XSalsa20-Poly1305 (libsodium `crypto_secretbox_easy`), writes `[nonce_24B][ciphertext]` to `<path>`. Atomic write: write to `<path>.tmp`, fsync, rename.
- `hu_persona_load_encrypted(hu_allocator_t *, const char *path, const uint8_t key[32], hu_persona_t *out)` → `hu_error_t` — reads, decrypts, parses. Returns `HU_ERR_SECURITY_DENIED` if authentication tag check fails (wrong key or tampered file).
- `hu_persona_crypt_migrate(hu_allocator_t *, const char *path, const char *persona_name)` → `hu_error_t` — detects plaintext JSON at `<path>`, encrypts in place, rewrites. Idempotent: returns `HU_OK` if already encrypted. Logs migration to stderr under `HU_IS_TEST` guard.

The guard `HU_PERSONA_ENCRYPTED` compile flag enables the encrypted path. When the flag is set, `hu_persona_load` calls `hu_persona_load_encrypted` and fails fast with `HU_ERR_SECURITY_DENIED` on a plaintext file (no silent downgrade).

**Acceptance criteria:**

- AC-8.2.1: GIVEN a plaintext persona JSON file at a temp path, WHEN `hu_persona_crypt_migrate` is called, THEN the file at that path is no longer valid UTF-8 JSON (it is now binary ciphertext) and `hu_persona_load_encrypted` with the correct derived key reconstructs the original persona with all fields equal.
- AC-8.2.2: GIVEN an encrypted persona file, WHEN `hu_persona_load_encrypted` is called with a key that differs in exactly one byte, THEN the function returns `HU_ERR_SECURITY_DENIED` and does not write any output to the `hu_persona_t` out-param.
- AC-8.2.3: GIVEN two calls to `hu_persona_save_encrypted` with identical persona and key, WHEN the output files are compared, THEN their ciphertexts differ (nonce is fresh per call; deterministic nonces are prohibited by the MAC).
- AC-8.2.4: GIVEN a compile build with `HU_PERSONA_ENCRYPTED` defined, WHEN `hu_persona_load` is called on a plaintext JSON path, THEN it returns `HU_ERR_SECURITY_DENIED` without populating the out-struct.
- AC-8.2.5: GIVEN `hu_persona_save_encrypted` writes to a path whose parent directory is writable, WHEN the function is called, THEN a crash during write (simulated by SIGKILL after the `tmp` file is written but before rename) leaves the original persona file intact and readable.

**Estimate:** M  
**Risk tier:** HIGH (key management; wrong impl = data loss or silent-downgrade to plaintext)  
**Dependencies:** none (libsodium is already in vendor/)  
**Test seam:** `tests/test_persona_crypt.c`  
**DoD:** full suite 10,000+ tests green, 0 ASan errors; `/verify` PASS; `/aspect-panel` CLEAN; `tests/test_persona_crypt.c` references `hu_persona_save_encrypted`, `hu_persona_load_encrypted`, `hu_persona_crypt_migrate`; adversarial ACs (8.2.2, 8.2.4) assert dangerous case is BLOCKED.

**Out of scope:**  
- Migrating all existing persona files automatically on daemon startup (that is an onboarding/migration story; defer).
- Secure Enclave / SEP hardware key wrapping (Keychain is sufficient for v1; TEE deferred per sprint brief).
- Multi-device key sync (federated scope; explicitly excluded by sprint brief).

---

### US-8.3 (P1): Reproducible Binary Builds

**As a** security-conscious user or auditor,  
**I want** the `human` binary to be bytewise reproducible from source (same SHA-256 on two independent builds from the same source tree),  
**so that** I can independently verify a distributed binary matches the published source without trusting the build server.

**Context on the current state:**  
`CMakeLists.txt` has no `SOURCE_DATE_EPOCH` handling, no `-frandom-seed`, and does not suppress `-D__DATE__`/`-D__TIME__`. A search of `src/` found no explicit use of `__DATE__` or `__TIME__`, but CMake can inject build-time macros via `configure_file` or `add_definitions`. Archive member ordering and debug info path embedding also vary by build host.

**What must change:**  
Changes are confined to `CMakeLists.txt` and a new CI step. No production `.c` files change. Specifically:

1. Read `SOURCE_DATE_EPOCH` from environment; if set, pass `-D__DATE__=...` and `-D__TIME__=...` as compile flags that override any baked-in values, and pass `-frandom-seed=<hash-of-source-file>` per translation unit so internal symbol name randomization is deterministic.
2. Pass `-ffile-prefix-map=$(pwd)=.` to strip absolute build-path prefixes from debug info.
3. Pass `-Wno-date-time` to promote accidental `__DATE__`/`__TIME__` use to a warning (informational; the override flags above handle correctness).
4. Add `scripts/verify-reproducible-build.sh`: builds twice with `SOURCE_DATE_EPOCH=1700000000`, diffs with `sha256sum`, exits 0 if identical.
5. Add a new CI job `reproducible-build` in `ci.yml` that runs `scripts/verify-reproducible-build.sh` on linux x86_64 and fails the PR if the SHA-256 differs.

**Acceptance criteria:**

- AC-8.3.1: GIVEN `SOURCE_DATE_EPOCH=1700000000 cmake --preset release && cmake --build --preset release` is run twice from a clean checkout, WHEN `sha256sum build/human` is called after each build, THEN both hashes are identical.
- AC-8.3.2: GIVEN `scripts/verify-reproducible-build.sh` is run on CI (linux x86_64), WHEN it completes, THEN it exits 0 and prints the matching SHA-256.
- AC-8.3.3: GIVEN a source file is modified to introduce `__DATE__` usage, WHEN the build runs with the new compile flags, THEN the compiler emits at least one `-Wdate-time` diagnostic (confirming the warning is active).
- AC-8.3.4: GIVEN two builds with identical `SOURCE_DATE_EPOCH` from the same source but different absolute build paths (e.g., `/tmp/build1` vs `/tmp/build2`), WHEN SHA-256 is compared, THEN they match (confirming `-ffile-prefix-map` strips the path).

**Estimate:** S  
**Risk tier:** MEDIUM (CMakeLists change; no production behavior change)  
**Dependencies:** none  
**Test seam:** `scripts/verify-reproducible-build.sh` + CI job (not a `tests/test_*.c` file; this story's "test" is the diff script itself)  
**DoD:** full suite 10,000+ tests green, 0 ASan errors; `/verify` PASS (verifier runs `verify-reproducible-build.sh` and confirms exit 0); `/aspect-panel` CLEAN.

**Out of scope:**  
- macOS aarch64 reproducibility (linker and `codesign` add non-determinism; linux first, darwin in a follow-on).
- Stripping debug symbols from release binaries (separate perf/size story).
- Hermetic build environment (Nix/Docker pinning; future story).

---

### US-8.4 (P1): Signed Release Artifacts with Embedded Public Key

**As a** user who downloads a `human` release binary,  
**I want** to be able to verify the binary was signed by the project's release key using `human doctor --privacy`,  
**so that** I can detect tampered or counterfeit binaries without trusting a third-party verification service.

**Context on the current state:**  
`scripts/generate-sbom.sh` exists but is unsigned. `scripts/deploy.sh` calls `codesign` for macOS TCC purposes but not for user-verifiable release signing. There is no ed25519 keypair, no detached `.sig` file, and no signature-check code path anywhere in the binary.

**What must change:**  
A new translation unit `src/security/artifact_sign.c` exposes:

- `hu_artifact_sign_verify(hu_allocator_t *, const char *artifact_path, const char *sig_path, char *err, size_t err_len)` → `hu_error_t` — verifies an ed25519 detached signature file `<artifact>.sig` against `artifact_path` using the embedded release public key (`HU_RELEASE_PUBKEY_HEX` compile-time constant). Returns `HU_OK` if valid, `HU_ERR_SECURITY_DENIED` if invalid, `HU_ERR_NOT_FOUND` if `.sig` is absent.
- The public key is embedded in `src/security/artifact_sign.c` as a `static const uint8_t hu_release_pubkey[32]` initialized from a hex literal (32 bytes, ed25519 public key). The private key is NOT in the repo; signing is done offline by the release pipeline.

A new `scripts/sign-artifacts.sh` accepts `<private_key_file> <artifact_path>` and writes `<artifact_path>.sig` using `openssl dgst -sign` with Ed25519, or `libsodium` CLI if available.

`human doctor --privacy` (US-8.5 depends on this) calls `hu_artifact_sign_verify` on the running binary path (`/proc/self/exe` on linux, `_NSGetExecutablePath` on darwin). Reports `signature: VALID | MISSING | INVALID`.

**Acceptance criteria:**

- AC-8.4.1: GIVEN a test fixture file `<tmp>/test_artifact.bin` signed with a known test ed25519 private key, WHEN `hu_artifact_sign_verify` is called with the matching public key compiled in, THEN it returns `HU_OK`.
- AC-8.4.2: GIVEN the same fixture with one byte of the signature flipped, WHEN `hu_artifact_sign_verify` is called, THEN it returns `HU_ERR_SECURITY_DENIED` and the `err` buffer contains a human-readable message.
- AC-8.4.3: GIVEN no `.sig` file exists alongside the artifact, WHEN `hu_artifact_sign_verify` is called, THEN it returns `HU_ERR_NOT_FOUND` (not `HU_ERR_SECURITY_DENIED` — the distinction matters for `doctor --privacy` reporting).
- AC-8.4.4: GIVEN `scripts/sign-artifacts.sh <test_privkey> <artifact>` is run, WHEN the resulting `.sig` file is passed to `hu_artifact_sign_verify` with the matching public key, THEN the function returns `HU_OK` (round-trip: sign + verify).
- AC-8.4.5: GIVEN the embedded public key in `src/security/artifact_sign.c` is a valid 32-byte ed25519 public key (not all-zeros, not all-0xff), WHEN the test reads the compiled-in constant, THEN a sanity assertion passes (`key != {0x00...}` and `key != {0xff...}`).

**Estimate:** M  
**Risk tier:** HIGH (security-sensitive; wrong verify logic = false assurance)  
**Dependencies:** US-8.3 (signing a non-reproducible binary is pointless; run US-8.3 first in wave 1, US-8.4 in wave 2)  
**Test seam:** `tests/test_artifact_sign.c`  
**DoD:** full suite 10,000+ tests green, 0 ASan errors; `/verify` PASS; `/aspect-panel` CLEAN; `tests/test_artifact_sign.c` references `hu_artifact_sign_verify`; adversarial ACs (8.4.2, 8.4.3) assert dangerous case is BLOCKED, not accepted; private key is confirmed absent from repo (CI `git grep` check).

**Out of scope:**  
- CI auto-signing of every PR build (only release builds; too expensive otherwise).
- macOS `codesign` notarization (different trust chain; not user-verifiable without Apple).
- SBOM signing (that is US-8.5).

---

### US-8.5 (P2): Privacy Posture Command (`human doctor --privacy`)

**As a** user or auditor who wants to understand h-uman's privacy guarantees,  
**I want** to run a single command `human doctor --privacy` that reports the live status of every structural privacy claim,  
**so that** I can verify the privacy posture without reading C source or trusting documentation.

**Context on the current state:**  
`src/doctor_fix.c` exists and implements `hu_doctor_fix` which checks state/skills/plugins/personas/config directories. It does not check any privacy-relevant property. There is no `--privacy` flag on the `doctor` subcommand.

**What must change:**  
A new translation unit `src/security/privacy_posture.c` exposes:

- `hu_privacy_posture_t` struct with fields: `bool personas_encrypted`, `bool build_reproducible`, `bool signature_valid`, `bool sbom_present`, `double dp_epsilon_spent` (NAN if no training run), `double dp_epsilon_budget` (0.0 if DP not configured).
- `hu_privacy_posture_check(hu_allocator_t *, const char *state_dir, hu_privacy_posture_t *out)` → `hu_error_t` — populates the struct. Each check is independent; a failure in one check sets that field to `false`/NAN but does not abort the others.
- Each boolean is a pure predicate extractable for unit tests per `.claude/rules/security-predicate-extraction.md`:
  - `hu_privacy_personas_encrypted(const char *personas_dir)` → `bool` — true iff every `.json` file in the directory begins with the 24-byte nonce magic (first 3 bytes are not `{`, `[`, or `"`).
  - `hu_privacy_build_reproducible(const char *binary_path)` → `bool` — reads a `<binary>.reprocheck` sidecar file (SHA-256 written by `verify-reproducible-build.sh`); true iff present and matches `sha256sum` of the binary. Returns false if sidecar absent.
  - `hu_privacy_signature_valid(const char *binary_path)` → `bool` — thin wrapper around `hu_artifact_sign_verify`; true iff `HU_OK`.
  - `hu_privacy_sbom_present(const char *state_dir)` → `bool` — true iff `<state_dir>/sbom.json` exists and is non-empty valid JSON with `"bomFormat":"CycloneDX"`.
- CLI: `human doctor --privacy` calls `hu_privacy_posture_check`, prints a table (`OK` / `FAIL` / `N/A` per row), and exits 1 if any boolean is false (never silently lies).

**Acceptance criteria:**

- AC-8.5.1: GIVEN a temp state dir with all four privacy artifacts in place (encrypted persona, `.reprocheck` sidecar matching the binary SHA-256, valid `.sig`, and a `sbom.json` with `"bomFormat":"CycloneDX"`), WHEN `hu_privacy_posture_check` is called, THEN all four booleans are `true` and `hu_error_t` is `HU_OK`.
- AC-8.5.2: GIVEN a state dir where `sbom.json` exists but contains `{}` (no `bomFormat` field), WHEN `hu_privacy_sbom_present` is called, THEN it returns `false`.
- AC-8.5.3: GIVEN a personas dir containing one plaintext JSON file (starts with `{`) and one encrypted file (binary), WHEN `hu_privacy_personas_encrypted` is called, THEN it returns `false` (mixed state is not encrypted).
- AC-8.5.4: GIVEN `human doctor --privacy` is run on a system where the binary has no `.sig` sidecar, WHEN the command exits, THEN the exit code is 1 and the output line for `signature` contains `FAIL` (not `OK` or `N/A`).
- AC-8.5.5: GIVEN `hu_privacy_posture_check` is called and `hu_privacy_signature_valid` would panic (e.g., binary path does not exist), WHEN the function completes, THEN it returns `HU_OK` with `signature_valid=false` (no crash; graceful degradation).

**Estimate:** S  
**Risk tier:** HIGH (if the command lies — reports OK when not OK — it actively destroys trust)  
**Dependencies:** US-8.2 (persona encryption check), US-8.3 (reprocheck sidecar), US-8.4 (signature verify)  
**Test seam:** `tests/test_privacy_posture.c`  
**DoD:** full suite 10,000+ tests green, 0 ASan errors; `/verify` PASS; `/aspect-panel` CLEAN; `tests/test_privacy_posture.c` references `hu_privacy_posture_check`, `hu_privacy_personas_encrypted`, `hu_privacy_sbom_present`; AC-8.5.4 verified by a shell-level test that checks exit code; AC-8.5.3 asserts mixed state returns false.

**Out of scope:**  
- `human doctor --privacy --fix` auto-remediation (encrypt all personas, generate sbom, etc.) — that is a follow-on story.
- Continuous monitoring / alerting when posture degrades (ops story; defer).
- Reporting DP ε for sprint-7's training runs (dependency on sprint-7 composing `dp_sgd.c`; US-8.5 reports NAN until that wiring lands).

---

## Non-Goals (this sprint will NOT do any of these)

1. We will NOT implement federated learning across user devices. That is a multi-sprint epic explicitly out of scope per sprint brief.
2. We will NOT integrate Secure Enclave / SEP hardware key wrapping. macOS Keychain is the darwin substitute; full TEE is deferred per sprint brief.
3. We will NOT wire `src/ml/dp_sgd.c` into any existing learner. US-8.1 delivers the correct module; composition with sprint-7's surface is a sprint-9 story.
4. We will NOT touch `src/ml/dpo*`, `scripts/finetune-gemma.py`, or `src/ml/cli.c` (sprint-7 no-touch surface).
5. We will NOT auto-run `human doctor --privacy --fix` or migrate all existing persona files on daemon startup. Migration UX belongs in the onboarding flow; that is a separate story.

---

## Open Questions for Stakeholder

1. **libsodium availability:** The persona encryption story assumes libsodium is in `vendor/`. If it is not already linked, the implementer will need to vendor it or fall back to a platform crypto API (CommonCrypto on darwin, OpenSSL on linux). Please confirm whether libsodium is already an approved dependency or whether we should target platform-native APIs exclusively.

2. **ed25519 release keypair:** US-8.4 embeds the public key at compile time via `HU_RELEASE_PUBKEY_HEX`. For the sprint test fixtures, a throwaway keypair is acceptable. For production use, the project needs a release-signing key pair and a decision about where the private key lives (HSM, GPG-encrypted file in a secrets manager, etc.). Does a release keypair already exist, or does the release team need to generate one before this story ships to production?

3. **`--privacy` flag on `doctor` subcommand:** `src/doctor_fix.c` has `hu_doctor_fix` but the CLI routing lives in `src/main.c`. The scrum-master should confirm which engineer owns the `doctor` subcommand dispatch before assigning US-8.5, to avoid a merge conflict with any in-flight sprint-7 CLI work.

---

RESULT_product-owner=READY
