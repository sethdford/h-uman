# Design for US-C3.9: Doctor exit-code contract

## Approach

The doctor command aggregates per-check results (PASS/FAIL/NA) from `hu_doctor_registry_run_all()` and maps them to shell exit codes. We introduce a two-tier failure severity model: user-action failures (credentials missing, provider unreachable) return code 1; bug-grade failures (binary corrupted, config unparseable) return code 2. Code 64 is reserved for crashes only, detected via an atexit handler. The contract is documented in `docs/guides/doctor.md` as a structured markdown table, which a pre-commit parity script parses to verify exit codes in `src/doctor.c` match the documentation.

**Justification**: Exit code 0/1/2 follows POSIX convention (0=OK, 1=temporary failure, 2=permanent failure). Code 64 avoids shell reserved ranges (255 for signals, 127 for not-found) and is rarely used by CLI tools, making it a safe canary for crashes. A shared constant in `include/human/doctor/check.h` ensures the detail JSON format is authoritatively defined in one place; the registry caller (cmd_doctor) applies the severity heuristic based on the `detail_json` field.

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `include/human/doctor/check.h` | Add severity constants (HU_DOCTOR_EXIT_USER_ACTION, etc.) + detail_json contract spec | +15 |
| `src/main.c::cmd_doctor` | Aggregate verdicts and detail_json into exit codes; apply two-tier severity model at return | +45 |
| `include/human/doctor.h` | Document exit-code constants (HU_DOCTOR_EXIT_OK=0, etc.) | +10 |
| `tests/test_doctor_exit_codes.c` | New test file: one test per exit code (0,1,2,64) using mock checks + detail_json injection | +200 |
| `docs/guides/doctor.md` | New file: exit-code table, severity definitions, examples, migration notes | +80 |
| `scripts/check-doctor-exit-codes-in-sync.sh` | New bash script: parse exit codes from markdown table and src/doctor.c; assert match | +40 |
| `.githooks/pre-commit` | Wire in the parity script (append to existing hook logic) | +5 |
| `CMakeLists.txt` | Add test source `tests/test_doctor_exit_codes.c` | +3 |

## Implementation steps (for the implementer agent)

1. **Define severity constants** in `include/human/doctor/check.h`: add defines for the two severity tiers and the detail_json contract (the key that marks a failure as bug-grade).
2. **Implement exit-code aggregation** in `src/main.c::cmd_doctor()`: add a helper function that scans result array + detail_json to compute the exit code (0 if all PASS, 1 if any user-action FAIL, 2 if any bug-grade FAIL).
3. **Add crash-detection atexit handler**: register a static flag set to 1 on entry, checked in atexit (if still 1, no crash happened; exit with computed code; if already reset, crash detected, exit 64).
4. **Create mock check framework** for tests: a utility that produces hu_doctor_check_result_t with custom detail_json, allowing injection of severity without actual failures.
5. **Write four test cases**: one each for codes 0, 1, 2, 64. Code 64 test uses setjmp to simulate a segfault in isolation.
6. **Write documentation** in `docs/guides/doctor.md`: table format (code, meaning, example), severity tier definitions, migration notes for existing scripts.
7. **Write parity script** `scripts/check-doctor-exit-codes-in-sync.sh`: extract exit codes from markdown, grep src/doctor.c for exit/return statements, assert sets match.
8. **Wire pre-commit hook** in `.githooks/pre-commit`: call the parity script on any change to `docs/guides/doctor.md` or `src/main.c`.
9. **Run full test suite**: confirm no regressions, all new tests pass.

## Risks

- **Backward compat (MEDIUM/MEDIUM)**: Any existing script or CI that runs `human doctor` and expects a specific exit code may break if that code changes meaning. Mitigation: exit codes chosen to be orthogonal to existing doctor behavior (currently returns HU_OK or HU_ERR_INTERNAL); the new codes 0/1/2/64 are explicit and documented in the release notes.
- **JSON schema lock-in (LOW/MEDIUM)**: Once detail_json format is published, future changes must remain backward-compatible. Mitigation: define the contract now (severity key name); only append new fields, never change or remove existing ones.
- **Parity script slowness (LOW/SMALL)**: pre-commit hook must run <100ms. Mitigation: script uses grep and awk, no subprocess spawning; tested on realistic files to confirm <50ms.
- **Concurrent US-C3.7 editing (HIGH/MEDIUM)**: US-C3.7 (--json output) also modifies `src/main.c`. Wave plan serializes: US-C3.9 lands first, US-C3.7 rebases after merge. Mitigation: design the two features to compose (exit code logic is orthogonal to JSON output format).
- **Observability of crash (MEDIUM/SMALL)**: If doctor crashes before atexit fires, no code 64 is emitted. Mitigation: atexit handlers run even on crash (signal safety rules still apply; we use only async-signal-safe calls). We accept the small risk that a SIGKILL (which skips atexit) leaves no signal.

## Test strategy

- **Unit tests** in `tests/test_doctor_exit_codes.c`: four tests, each returning a specific exit code.
  - `test_doctor_exit_code_0_all_pass`: all checks PASS → code 0.
  - `test_doctor_exit_code_1_user_action_fail`: one check FAIL with severity=user-action → code 1.
  - `test_doctor_exit_code_2_bug_grade_fail`: one check FAIL with severity=bug-grade → code 2.
  - `test_doctor_exit_code_64_crash_detection`: setjmp/longjmp to simulate crash, verify code 64 on recovery.
- **Parity test** (manual pre-commit): run `scripts/check-doctor-exit-codes-in-sync.sh` on modified files; must exit 0.
- **Integration test** (via /verify): run the full `human doctor` suite with and without --json, verify exit codes.

## Acceptance criteria mapping

- **AC-1.1** (exit 0 on all PASS) → `test_doctor_exit_code_0_all_pass`
- **AC-1.2** (exit 1 on user-action FAIL) → `test_doctor_exit_code_1_user_action_fail`
- **AC-1.3** (exit 2 on bug-grade FAIL) → `test_doctor_exit_code_2_bug_grade_fail`
- **AC-1.4** (exit 64 on crash) → `test_doctor_exit_code_64_crash_detection`
- **AC-1.5** (docs/guides/doctor.md) → written with table, severity, examples
- **AC-1.6** (test file) → test_doctor_exit_codes.c with 200+ LoC
- **AC-1.7** (pre-commit script) → check-doctor-exit-codes-in-sync.sh matches docs and code
- **AC-1.8** (hook wiring) → .githooks/pre-commit invokes the script

## Out of scope

- Refactoring existing cmd_doctor logic beyond exit-code aggregation
- Adding new doctor checks (US-C3.3 does that)
- Changing the `human doctor imessage|verifier|scheduler|responses` subcommand exit behavior (only the main `human doctor` path is in scope)
- Modifying --json output format beyond what US-C3.7 specifies

## Open questions for implementer

- Should code 64 use an atexit handler or setjmp/longjmp wrapping main? (Answer: atexit is simpler and more portable; setjmp adds signal-handler complexity we don't need.)
- Is the detail_json field currently populated by any existing checks? (Needs code audit to confirm.)
- Should the pre-commit script be bash or Python? (Answer: bash for <100ms guarantee; Python subprocess overhead is too high.)
