# Design for US-8.5: Privacy Posture Command (`human doctor --privacy`)

> **Status:** DESIGN_READY. Wave-2 story. **Do not start implementation
> until US-8.2, US-8.3, and US-8.4 have landed on the sprint branch.**
> US-8.1 lands in parallel (Wave-1) and is optional — the DP-ε field
> reports `NaN` if the accountant log is absent (see "DP-ε path" below).

## Approach

`human doctor --privacy` adds one focused diagnostic surface on top of
the existing `doctor` subcommand pattern. The new translation unit lives
at `src/security/privacy_posture.c` (per the story's "What must change"
section — *not* `src/doctor_privacy.c` as the tech-lead prompt suggested,
because privacy is a security concern and the file naturally co-locates
with the other security predicates).

The boring design wins here: this is a thin aggregator. Every leaf check
is a pure predicate per the project's
`.claude/rules/security-predicate-extraction.md`. The aggregator's only
job is to call each predicate, populate a struct, and never silently
collapse a failure into "OK". The printer/JSON layer lives in `src/main.c`
alongside the existing `cmd_doctor` subcommand printers — same pattern as
`doctor imessage`, `doctor verifier`, `doctor scheduler`, `doctor responses`.

### Why not extend `src/doctor.c`?

`src/doctor.c` is 1,020 LOC and already aggregates ~9 unrelated checks
through the `hu_diag_item_t` interface. Adding privacy through that pipe
would (a) force the privacy posture into a flat severity list, losing the
typed struct that AC-8.5.1 requires (`hu_privacy_posture_t` with named
fields), and (b) widen `doctor.c` further. A separate TU keeps the
privacy posture testable in isolation and makes the dependency graph
explicit (`privacy_posture.c` → `persona_crypto.c`, `artifact_sign.c`,
`reproducible_build.c`, `sbom.c`).

### Schema reconciliation

The story names the fields:
`personas_encrypted, build_reproducible, signature_valid, sbom_present,
dp_epsilon_spent, dp_epsilon_budget`. The tech-lead prompt names them
`persona_encrypted, build_reproducible, signature_verified, sbom_present,
dp_epsilon, doctor_overall`. **The story is the source of truth.** The
JSON serializer adds a derived `summary` string (`"ok"` | `"fail"`) — the
analogue of `doctor_overall` — computed from the booleans, not stored.

## Files to modify

| File                                       | Change                                                                 | Est LOC |
| ------------------------------------------ | ---------------------------------------------------------------------- | ------- |
| `include/human/privacy_posture.h`          | **New.** Public struct + 5 function prototypes (1 aggregator + 4 leaf) | +60     |
| `src/security/privacy_posture.c`           | **New.** 4 pure predicates + `hu_privacy_posture_check` aggregator     | +250    |
| `src/main.c`                               | Add `doctor privacy [--json]` branch in `cmd_doctor`                   | +90     |
| `tests/test_privacy_posture.c`             | **New.** Unit tests for each predicate + aggregator                    | +400    |
| `tests/test_doctor_privacy_cli.c`          | **New.** Shell-level exit-code test for AC-8.5.4                       | +120    |
| `CMakeLists.txt`                           | Add `src/security/privacy_posture.c` to `human_lib` sources            | +1      |
| `CMakeLists.txt` (test section)            | Add both new test files to `human_tests`                               | +2      |
| `sprints/sprint-8/evidence/US-8.5-evidence.md` | Verifier evidence (filled in by implementer)                       | +30     |

Wave-2 caveat: the headers `human/persona_crypto.h` (US-8.2),
`human/artifact_sign.h` (US-8.4), and the `.reprocheck` sidecar format
(US-8.3) are dependencies. The implementer **stubs them with
forward-declared externs** if those PRs are not yet merged, and rebases
on top before the final commit. See Risk R3.

## Public API (header sketch)

```c
/* include/human/privacy_posture.h */
typedef struct hu_privacy_posture {
    bool personas_encrypted;       /* AC-8.5.3 — strict: mixed state is false */
    bool build_reproducible;       /* AC-8.5.1 — .reprocheck sidecar matches sha256 */
    bool signature_valid;          /* AC-8.5.1 — hu_artifact_sign_verify == HU_OK */
    bool sbom_present;             /* AC-8.5.2 — bomFormat=="CycloneDX" required */
    double dp_epsilon_spent;       /* NaN if accountant log absent */
    double dp_epsilon_budget;      /* 0.0 if DP not configured */
} hu_privacy_posture_t;

/* Aggregator. Independent leaves: each leaf's failure sets its field
 * to false/NaN and does NOT abort the others. Returns HU_OK unless
 * the allocator fails. Per AC-8.5.5, missing binary/files → field
 * false, return HU_OK. */
hu_error_t hu_privacy_posture_check(hu_allocator_t *alloc,
                                    const char *state_dir,
                                    hu_privacy_posture_t *out);

/* Pure predicates — testable per security-predicate-extraction.md. */
bool hu_privacy_personas_encrypted(const char *personas_dir);
bool hu_privacy_build_reproducible(const char *binary_path);
bool hu_privacy_signature_valid(const char *binary_path);
bool hu_privacy_sbom_present(const char *state_dir);
```

## Predicate contracts (each is independently testable)

### `hu_privacy_personas_encrypted(personas_dir)`
- Iterates `personas_dir/*.json` via `opendir`/`readdir`.
- For each entry: open, read first 3 bytes.
- Returns `false` if **any** file's first byte is `{`, `[`, or `"`
  (plaintext JSON markers). Mixed state is explicitly false (AC-8.5.3).
- Returns `false` if the directory cannot be opened (no silent OK).
- Returns `true` only if every `.json` file starts with a non-JSON byte
  (assumed to be a libsodium nonce / encryption header).
- Skips non-regular files and dot-files.

### `hu_privacy_build_reproducible(binary_path)`
- Looks for `<binary_path>.reprocheck` (sidecar file written by
  `verify-reproducible-build.sh`).
- Returns `false` if the sidecar is missing, unreadable, or empty.
- Otherwise, computes SHA-256 of the binary (via existing
  `hu_hash_sha256_file` if present, otherwise small streaming impl).
- Returns `true` iff `sha256(binary) == contents-of-sidecar` (trimmed).

### `hu_privacy_signature_valid(binary_path)`
- Thin wrapper. Calls `hu_artifact_sign_verify(binary_path)`.
- Returns `true` iff `HU_OK`.
- Per AC-8.5.5: if the upstream call fails or binary path is missing,
  return `false` — never crash.

### `hu_privacy_sbom_present(state_dir)`
- Looks for `<state_dir>/sbom.json`.
- Returns `false` if file is absent, empty, or fails to parse as JSON
  via existing `hu_json_parse`.
- Returns `false` if the parsed object lacks a `"bomFormat"` string
  field whose value is exactly `"CycloneDX"` (AC-8.5.2 pin).
- Returns `true` only on the strict happy path.

### `hu_privacy_posture_check` (aggregator)
- Computes each leaf, populates the struct.
- Resolves binary path via `/proc/self/exe` (Linux) or
  `_NSGetExecutablePath` (macOS); if neither available, sets
  `build_reproducible=false`, `signature_valid=false`.
- Resolves `personas_dir` as `<state_dir>/personas`.
- DP-ε path: reads `<state_dir>/dp_accountant.log` if present (US-8.1
  format). Absent → `spent=NaN, budget=0.0`. Parse error → same.
- Returns `HU_OK` even if every leaf is false. Returns
  `HU_ERR_OUT_OF_MEMORY` only on allocator failure.

## Implementation steps (for the implementer agent)

1. **Wait for Wave-1.** Confirm `feat(security): persona encryption`
   (US-8.2), `feat(build): reproducible build script + sidecar`
   (US-8.3), and `feat(release): ed25519 signature verify` (US-8.4) are
   merged to `sprint-8-verifiable-privacy`. If not, do not start.
2. Create `include/human/privacy_posture.h` with the struct and 5
   prototypes (skeleton only, no bodies yet).
3. Create empty `src/security/privacy_posture.c` with stub bodies that
   `return false;` for each predicate and `HU_OK` for the aggregator
   (no real behavior).
4. Wire `src/security/privacy_posture.c` into `CMakeLists.txt`.
5. Add `tests/test_privacy_posture.c` with all 15 test cases below
   (every test should currently fail or hit the stub return value).
6. Implement `hu_privacy_sbom_present` (smallest, no platform deps).
   Run the 3 sbom tests to green.
7. Implement `hu_privacy_personas_encrypted`. Run 4 persona tests.
8. Implement `hu_privacy_build_reproducible`. Run 3 build tests.
9. Implement `hu_privacy_signature_valid` (thin wrapper). Run 2 sig tests.
10. Implement `hu_privacy_posture_check` aggregator. Run 3 aggregator
    tests including AC-8.5.1 happy path and AC-8.5.5 missing-binary.
11. Add CLI branch in `src/main.c`: `human doctor privacy [--json]`.
    Pretty printer + JSON printer parallel to the existing imessage/
    verifier pattern. Exit code: 1 if any boolean is false.
12. Add `tests/test_doctor_privacy_cli.c` — spawns `human doctor privacy`
    via `popen` (guarded by `HU_IS_TEST`-aware harness; reuse the
    pattern from `tests/test_doctor_cli.c` if present, else use
    a fixture-driven invocation).
13. Run full suite (`./build/human_tests`). Must be 0 failures, 0 ASan.
14. Run `/verify` with the AC contract list.

## Test strategy

### Unit tests in `tests/test_privacy_posture.c` (must reference `hu_privacy_posture_check`, `hu_privacy_personas_encrypted`, `hu_privacy_sbom_present` per `.claude/rules/test-references-production-symbol.md`)

| # | Name                                                            | AC      |
|---|-----------------------------------------------------------------|---------|
| 1 | `sbom_present_returns_true_when_cyclonedx`                      | 8.5.1   |
| 2 | `sbom_present_returns_false_when_bomformat_missing`             | 8.5.2   |
| 3 | `sbom_present_returns_false_when_file_absent`                   | 8.5.2   |
| 4 | `personas_encrypted_returns_true_when_all_encrypted`            | 8.5.1   |
| 5 | `personas_encrypted_returns_false_for_mixed_plaintext_and_enc`  | 8.5.3   |
| 6 | `personas_encrypted_returns_false_when_dir_unreadable`          | 8.5.5   |
| 7 | `personas_encrypted_returns_true_when_dir_empty`                | edge    |
| 8 | `build_reproducible_returns_true_when_sidecar_matches`          | 8.5.1   |
| 9 | `build_reproducible_returns_false_when_sidecar_missing`         | 8.5.5   |
| 10| `build_reproducible_returns_false_when_sidecar_mismatches`     | 8.5.5   |
| 11| `signature_valid_returns_true_on_hu_ok`                         | 8.5.1   |
| 12| `signature_valid_returns_false_on_missing_binary`               | 8.5.5   |
| 13| `posture_check_all_green_returns_all_true`                      | 8.5.1   |
| 14| `posture_check_missing_binary_returns_hu_ok_with_false`         | 8.5.5   |
| 15| `posture_check_independent_failures_do_not_abort_others`        | 8.5.5   |

### CLI test in `tests/test_doctor_privacy_cli.c`

| # | Name                                                            | AC      |
|---|-----------------------------------------------------------------|---------|
| 16| `doctor_privacy_exits_1_when_signature_missing`                | 8.5.4   |
| 17| `doctor_privacy_json_emits_strict_schema`                       | 8.5.1   |

## Risks

- **R1 — Trust collapse if posture lies (HIGH / LARGE).** If any field
  reports OK when it isn't, the entire sprint's "verifiable privacy"
  claim is undermined. **Mitigation:** every leaf is a pure predicate
  derived from PRIMARY EVIDENCE (file bytes on disk, not cached state).
  Adversarial test #16 pins the "exits nonzero" contract.
  Adversarial test #5 pins the "mixed state → false" contract.
  Code review checkpoint: the implementer must NOT add a code path
  where a leaf returns `true` on the error branch.

- **R2 — Silent failure on read errors (MED / LARGE).** A check that
  can't open a file might default to `[OK]`. **Mitigation:** the
  predicate contracts above are explicit: every error-path returns
  `false`. Test #6 (dir unreadable), #9 (sidecar missing), and #12
  (binary missing) pin this directly. Reviewer must reject any branch
  shaped like `if (err) return true;` or `if (!file) goto ok;`.

- **R3 — Wave-1 dependency stubs leak (MED / MED).** If implementation
  starts before US-8.2/8.3/8.4 land, the implementer may need to
  forward-declare upstream headers. **Mitigation:** the design
  explicitly requires the implementer to wait for Wave-1 (Step 1).
  If Wave-1 slips, the design degrades gracefully: each predicate
  returns `false` on missing dependency rather than failing to compile
  (predicates are independent TUs).

- **R4 — Cross-platform binary-path resolution (LOW / SMALL).**
  `/proc/self/exe` is Linux-only; macOS needs `_NSGetExecutablePath`.
  **Mitigation:** wrap in existing `hu_self_exe_path()` if present;
  else `#if defined(__APPLE__)` / `#elif defined(__linux__)` /
  `#else` (return `false`). The CI build matrix already covers both.

- **R5 — Reading binary for SHA-256 is slow on first run (LOW / SMALL).**
  Binary is ~1.75 MB; SHA-256 streaming is ~5–10 ms on modern HW.
  Acceptable for a one-shot diagnostic. Do not cache (caching would
  violate R1).

- **R6 — JSON injection from filenames (LOW / SMALL).** The JSON printer
  must escape any path/filename it includes in output. **Mitigation:**
  reuse the existing escape loop in `cmd_doctor` (lines 629–649 of
  `src/main.c`).

- **R7 — Backward compat with existing `doctor` flags (LOW / SMALL).**
  `--privacy` was named in the prompt but the story uses `doctor privacy`
  as a subcommand verb (matching `doctor imessage`, `doctor verifier`).
  **Mitigation:** implement as a subcommand verb (matches the existing
  pattern). Accept both `doctor privacy` and `doctor --privacy` for
  ergonomic parity (one-line `argc` check; cheap).

## Acceptance criteria mapping

| AC        | Covered by                                                                                  |
|-----------|---------------------------------------------------------------------------------------------|
| AC-8.5.1  | Tests #1, #4, #8, #11, #13 + #17 (JSON schema)                                              |
| AC-8.5.2  | Tests #2, #3                                                                                |
| AC-8.5.3  | Test #5 (the adversarial mixed-state pin)                                                   |
| AC-8.5.4  | Test #16 (shell-level exit code)                                                            |
| AC-8.5.5  | Tests #6, #9, #10, #12, #14, #15 (graceful degradation across all four predicates)          |

## Out of scope (deferred per story)

- `--fix` auto-remediation (encrypt personas, generate sbom).
- DP-ε reporting from real training runs (returns NaN until sprint-9).
- Continuous monitoring / alerting on posture degradation.

## Anti-patterns to avoid (explicit reminders for implementer)

- ❌ Returning `true` from a predicate on the error branch.
  See `~/.claude/rules/quality-gates.md` "no silent failures."
- ❌ Caching posture result across calls. Every invocation re-reads
  primary evidence. See R1.
- ❌ Letting one leaf failure short-circuit the others. Aggregator
  must call all four leaves unconditionally. Test #15 pins this.
- ❌ Adding a "permissive" mode or env-var override. Posture is
  binary: OK or FAIL, full stop.
- ❌ Writing the test before reading the production header (per
  `.claude/rules/test-references-production-symbol.md` — tests must
  reference `hu_privacy_*` symbols, not local helpers).
