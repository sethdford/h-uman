---
title: Sprint 54 Backlog
sprint: 54
branch: sprint-54-tier-1-2-3-cleanup
status: planned
---

# Sprint 54 Backlog

## Goal
Complete the next wave of Sprint C foundation consumers (Tier 1) + start the biggest remaining M3 work (Tier 2) + clear plan-dir hygiene (Tier 3).

## Scope Discipline

### In Scope
- Provider smoke-check doctor implementation (US-C3.3)
- Provider setup onboarding step (US-C2.3) — reuses US-C3.3 smoke impl
- Doctor exit-code contract (US-C3.9)
- Doctor --json output (US-C3.7)
- MLX streaming wire (US-M3-B4)
- Plan directory frontmatter normalization (US-CLEAN-1)

### Out of Scope
- New doctor checks beyond exit-code and JSON output
- Additional onboarding steps beyond provider setup
- Provider implementation changes or refactoring
- MLX provider subprocess rewrite (extend only)
- Plan archive migration or deep curation

## Wave Plan

**Dependencies & sequence:**
- US-C3.3 (Provider smoke check) is the prerequisite for US-C2.3 (the latter reuses the smoke implementation)
- US-C3.9 (exit codes) and US-C3.7 (JSON output) both modify `src/doctor.c` → must serialize
- US-M3-B4 (MLX streaming) is independent
- US-CLEAN-1 (frontmatter) is a pure documentation pass, safe to parallelize

**Suggested wave execution:**

1. **Wave 1 (Warmup, parallel-safe):** US-CLEAN-1
   - Low risk, mechanical, no cross-dependencies
   - Validates build + test infrastructure before heavy lifting

2. **Wave 2 (Parallel, independent paths):** US-C3.3 + US-M3-B4
   - US-C3.3 (provider smoke) touches `src/doctor/` only
   - US-M3-B4 (MLX streaming) touches `src/providers/mlx.c` only
   - Can land simultaneously without merge conflicts

3. **Wave 3 (Sequential, same file):** US-C3.9 → US-C3.7
   - Both modify `src/doctor.c`; serialize to avoid conflicts
   - Exit-code contract first (simpler), then JSON output (builds on exit-code logic)

4. **Wave 4 (Depends on Wave 2):** US-C2.3
   - Requires US-C3.3 landed to reuse smoke implementation
   - Plugs into onboarding dispatcher once provider smoke is stable

---

## Stories (in priority order)

### US-CLEAN-1 (P0): Normalize plan-directory frontmatter entries

**As a** sprint planner, **I want** all 26 UNCERTAIN plan entries to have consistent `status:` frontmatter, **so that** the plan directory is machine-readable and humans can quickly identify plan state.

**Acceptance criteria:**
- AC-1.1: Each of 26 files identified in `docs/plans/STATUS.md` UNCERTAIN table receives exactly one canonical `status:` value (closed | active | deferred | superseded | archived) in frontmatter
- AC-1.2: `status:` value matches the intent documented in the file or commit history (if ambiguous, defaults to "deferred")
- AC-1.3: New frontmatter does not alter content, comments, or any non-frontmatter lines
- AC-1.4: Single commit titled "docs(plans): normalize 26 UNCERTAIN entries per STATUS.md schema" lands cleanly

**Files expected to change:**
- 26 files under `docs/plans/` (specific list in STATUS.md audit table)

**Estimate:** XS (1-2 hours, pure mechanical frontmatter edits)
**Priority:** P0
**Dependencies:** none
**Risk:** None (documentation-only, no code change)
**DoD:** `git diff` reviewed for correctness, no content loss, single clean commit

---

### US-C3.3 (P0): Provider smoke-check doctor implementation

**As a** daemon operator, **I want** the doctor to verify the configured provider is reachable and credentials are valid, **so that** I know before running the agent whether the provider will work.

**Acceptance criteria:**
- AC-1.1: New doctor check registered in `src/doctor/check_provider.c` via `hu_doctor_registry_register_defaults`
- AC-1.2: PASS verdict when: provider configured AND `hu_provider_create` succeeds AND a 1-token `complete("ok")` returns in <10s wall-clock
- AC-1.3: FAIL verdict surfaces one of: not_configured | credentials_missing | credentials_invalid_401 | rate_limited_429 | unreachable — with diagnostic text naming the specific failure mode
- AC-1.4: Network I/O gated behind `#ifdef HU_IS_TEST` to silence timeouts in default CI
- AC-1.5: Mock provider with failure-injection used in tests (pattern sourced from existing `src/providers/mock.c`)
- AC-1.6: Test coverage: PASS case, each of the 5 FAIL modes, timeout case
- AC-1.7: Aspect-panel review completed and approved before merge (security-sensitive credential handling)

**Files expected to change:**
- `src/doctor/check_provider.c` (new file, ~150 LoC)
- `tests/test_doctor_check_provider.c` (new file, ~250 LoC)
- `src/doctor/registry.c` (register the check)
- `CMakeLists.txt` (add test source)

**Estimate:** M
**Priority:** P0
**Dependencies:** none
**Risk:** Network timeout in test suite if not properly guarded; mitigated by HU_IS_TEST gate + 10s hard limit
**DoD:** /verify PASS, full test suite passes, /aspect-panel CLEAN, no security-sensitive logging of actual credentials

---

### US-C2.3 (P0): Provider setup onboarding step

**As a** new user, **I want** the onboarding wizard to guide me through provider selection and credential entry, **so that** I can configure the agent to talk to my chosen AI model without manual JSON editing.

**Acceptance criteria:**
- AC-1.1: New step file `src/onboard/step_provider.c` + `include/human/onboard/step_provider.h` + test `tests/test_onboard_step_provider.c`
- AC-1.2: Step plugs into `hu_onboard_dispatcher_t` step_table at slot HU_ONBOARD_STEP_PROVIDER
- AC-1.3: Reuses US-C3.3's provider smoke-check implementation (calls `hu_doctor_check_provider` for validation, does not reimplement)
- AC-1.4: Step invites user to choose from available providers (Gemini, Claude, OpenAI, MLX) and prompts for credentials
- AC-1.5: Persists provider choice + credentials to `hu_onboard_state_t.provider.{name, api_key, ...}` fields
- AC-1.6: Validates credentials via smoke check; re-prompts on failure with actionable error message
- AC-1.7: Test injection via `user_data` callback per the `step_welcome.c` reference pattern (see that file for call shape)
- AC-1.8: Aspect-panel review completed and approved before merge (credential handling at user touchpoint)

**Files expected to change:**
- `src/onboard/step_provider.c` (new file, ~300 LoC)
- `include/human/onboard/step_provider.h` (new file, ~40 LoC)
- `tests/test_onboard_step_provider.c` (new file, ~400 LoC)
- `src/onboard/dispatcher.c` (register step in step_table)
- `CMakeLists.txt` (add sources + test)

**Estimate:** L
**Priority:** P0
**Dependencies:** US-C3.3 (must land first; this story reuses smoke check)
**Risk:** Credential leakage if error messages capture full API key; mitigated by sanitizing output before logging (aspect-panel enforces)
**DoD:** /verify PASS, full test suite passes, /aspect-panel CLEAN, smoke check reuse verified via code review

---

### US-C3.9 (P0): Doctor exit-code contract

**As a** script author, **I want** `human doctor` to return specific exit codes, **so that** I can script conditional logic based on the diagnosis (e.g., abort deploy on bug-grade failures, prompt user on user-action failures).

**Acceptance criteria:**
- AC-1.1: `human doctor` returns exit code 0 when all checks PASS
- AC-1.2: Returns exit code 1 when any check FAIL with severity "user-action-required" (FDA denied, credentials missing, provider unreachable)
- AC-1.3: Returns exit code 2 when any check FAIL with severity "bug-grade" (binary file missing, config unparseable, bad CMake preset)
- AC-1.4: Returns exit code 64 when doctor itself crashes (e.g., segfault, out-of-memory)
- AC-1.5: New doc `docs/guides/doctor.md` (~80 lines) with complete exit-code table, severity definitions, and examples
- AC-1.6: Test file `tests/test_doctor_exit_codes.c` (~200 LoC) with one test per exit code, using mock checks
- AC-1.7: Pre-commit script `scripts/check-doctor-exit-codes-in-sync.sh` enforces that exit codes in source (`src/doctor.c`) match documented codes in `docs/guides/doctor.md`
- AC-1.8: Pre-commit script wired into `.githooks/pre-commit`

**Files expected to change:**
- `src/doctor.c` (add exit-code logic at main return path)
- `tests/test_doctor_exit_codes.c` (new file, ~200 LoC)
- `docs/guides/doctor.md` (new file, ~80 LoC)
- `scripts/check-doctor-exit-codes-in-sync.sh` (new file, ~40 LoC, bash)
- `.githooks/pre-commit` (wire in the script)
- `CMakeLists.txt` (add test source)

**Estimate:** M
**Priority:** P0
**Dependencies:** none (orthogonal to US-C3.7, but both modify `src/doctor.c` → serialize in wave 3)
**Risk:** Changing exit codes can break existing automation; mitigated by semantic bump in `--version` output and documented migration path in `docs/guides/doctor.md`
**DoD:** /verify PASS, full test suite passes, script passes on all realistic check states, docs are in sync with code per pre-commit check

---

### US-C3.7 (P0): Doctor `--json` output

**As a** observability engineer, **I want** `human doctor` to emit structured JSON output, **so that** I can parse diagnostics into monitoring dashboards and alert on specific failure modes.

**Acceptance criteria:**
- AC-1.1: New `--json` flag on `human doctor` command
- AC-1.2: JSON schema v1 locked:
  ```json
  {
    "version": 1,
    "ts": "<ISO 8601 timestamp>",
    "checks": [
      {
        "name": "<check name>",
        "verdict": "pass|fail",
        "reason": "<diagnostic message>"
      }
    ],
    "aggregate": "pass|fail"
  }
  ```
- AC-1.3: `aggregate` is true iff every check verdict is "pass"
- AC-1.4: stdout emits the JSON blob; stderr is empty on PASS, may contain debug info on FAIL (tested)
- AC-1.5: Test fixtures: `tests/fixtures/doctor_pass_all/` (all checks pass) + `tests/fixtures/doctor_fail_provider/` (provider check fails)
- AC-1.6: Tests in `tests/test_doctor_json_output.c` verify schema structure, field presence, timestamp format, aggregate calculation

**Files expected to change:**
- `src/doctor.c` (add --json flag parsing + output formatter)
- `tests/test_doctor_json_output.c` (new file, ~250 LoC)
- `tests/fixtures/doctor_pass_all/` (new directory with mock config)
- `tests/fixtures/doctor_fail_provider/` (new directory with invalid-provider config)
- `CMakeLists.txt` (add test source + fixture paths)

**Estimate:** M
**Priority:** P0
**Dependencies:** US-C3.9 (exit-code contract should land first to establish severity semantics; both modify `src/doctor.c`)
**Risk:** JSON output changes in future versions could break parsers; mitigated by explicit `version` field and append-only schema evolution rules documented in `docs/guides/doctor.md`
**DoD:** /verify PASS, full test suite passes, JSON output validated against schema in tests, fixture-based round-trip tested

---

### US-M3-B4 (P1): MLX streaming wire

**As a** on-device inference user, **I want** the MLX provider to stream tokens one-by-one from the subprocess, **so that** the agent can respond in real-time without waiting for the full batch completion.

**Acceptance criteria:**
- AC-1.1: New function `mlx_stream_chat` in `src/providers/mlx.c` with signature matching `hu_provider_stream_fn_t`
- AC-1.2: `hu_provider_mlx.supports_streaming` returns true
- AC-1.3: Subprocess stdout read via `select()` in non-blocking mode; each complete token emitted via `hu_stream_chunk_t`
- AC-1.4: Mid-stream cancellation via `kill(pid, SIGTERM)` cleanly terminates subprocess and consumer loop
- AC-1.5: No hanging subprocess on test exit (verified by process accounting)
- AC-1.6: Tests gated on `#ifdef HU_ENABLE_MLX_PROVIDER` + `__APPLE__` + `__arm64__`; skip cleanly with SKIP verdict on default CI (Linux/x86_64)
- AC-1.7: Test cases: `test_mlx_stream_chat_chunks_equal_batch` (streaming + batch produce same output) + `test_mlx_stream_chat_cancellation_terminates_subprocess` (SIGTERM cleans up)

**Files expected to change:**
- `src/providers/mlx.c` (add mlx_stream_chat function, wire into vtable)
- `tests/test_mlx_stream_chat.c` (new file, ~200 LoC, gated on HU_ENABLE_MLX_PROVIDER + macOS + ARM64)
- `CMakeLists.txt` (add test source with appropriate gates)

**Estimate:** M
**Priority:** P1
**Dependencies:** none (independent; touches only MLX provider)
**Risk:** Subprocess not terminating cleanly on SIGTERM could leave zombie processes; mitigated by explicit waitpid() with WNOHANG and 2-second hard timeout before SIGKILL fallback. Gating on macOS + ARM64 limits CI scope.
**DoD:** /verify PASS on CI runners with Apple Silicon (M-series Mac), full test suite passes, no zombie processes in post-test process accounting

---

## Definition of Done

All stories must satisfy:
- Full test suite passes (`./build/human_tests` exit code 0, no ASan errors)
- No `@allow-silent-pass` opt-outs (all assertions active)
- Security-sensitive stories (#1, #2) have passed `/aspect-panel` review with CLEAN verdict
- Pre-commit hooks pass for modified files
- `scripts/agent-preflight.sh` passes on changed files
- Sprint auditor produces no blocking findings

---

## Risk Register

| Risk | Impact | Mitigation |
|------|--------|-----------|
| **Network smoke test hangs in CI** (US-C3.3) | Test suite blocks for >30s | HU_IS_TEST gate + 10s timeout on provider.complete() call |
| **CMakeLists collision on parallel test adds** (US-C3.3 + US-M3-B4 in Wave 2) | Merge conflict on landing | Wave plan serializes landing; second implementer rebases after first merges |
| **MLX provider gating skips too aggressively** (US-M3-B4) | Tests don't run on any CI runner | Tests included in local dev preset (cmake --preset dev runs them on M-series; skip verdict reported cleanly on CI) |
| **Credential logging leaks API keys** (US-C2.3, US-C3.3) | Security incident | Aspect-panel enforces log sanitization; no raw credentials in stderr/logs |
| **JSON schema evolves incompatibly** (US-C3.7) | Parser breakage for observability | `version` field + evolution rules locked in `docs/guides/doctor.md`; only append new fields, never change existing |
| **Doctor exit codes conflict with shell conventions** (US-C3.9) | Script failures misinterpreted | Exit code 64 chosen to avoid conflicts with standard shells; documented in `docs/guides/doctor.md` |

---

## Anti-Goals

- We will NOT implement new doctor checks beyond provider validation and informational queries
- We will NOT add additional onboarding steps beyond provider setup (personas, memory, skill config come in later sprints)
- We will NOT refactor or rewrite the MLX subprocess pattern (extend via streaming only)
- We will NOT migrate or re-organize the plan directory (frontmatter normalization only; archival deferred)
- We will NOT change provider plugin registration or discovery; registration remains static config

---

## Open Questions for Stakeholder

None. All scope, acceptance criteria, and dependencies are explicitly scoped above. Proceed with Wave 1.
