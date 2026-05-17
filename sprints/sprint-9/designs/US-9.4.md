# Design for US-9.4: `human doctor --install` gate (P1, Sprint 9)

## Approach

Add a focused **install-readiness predicate** as a new exported function
`hu_doctor_check_install` in `include/human/doctor.h` and `src/doctor.c`,
following the exact prototype shape already established by
`hu_doctor_check_imessage`, `hu_doctor_check_verifier`,
`hu_doctor_check_scheduler`, and `hu_doctor_check_response_pipeline`
(allocator + diag-item out-array + count + cap). This is the boring
option: no new vtable, no new file, no new subsystem — just one more
diagnostic in the same shape as the existing five.

The CLI surface is **additive**: a new `--install` flag detected in the
existing `cmd_doctor` argv loop, in the same style as `--fix` and
`--json` are already detected (lines 677–681 of `src/main.c`). Critically,
**`--install` is a non-exclusive flag, not a subcommand**, so it composes
with sprint-8's pending `--privacy` flag and the existing `--json` and
`--fix` flags without N×M argv parsing. The dispatch rule is documented
inline: subcommand `imessage|verifier|scheduler|responses` takes
precedence (already-shipped), then `--install` (new), then `--fix`
(already-shipped), then the legacy full-report default. This keeps
sprint-8's `--privacy` slot free to add itself at the same level with a
single `else if (do_privacy)` branch.

The four checks (binary path resolvable, config dir exists, ≥1 channel
configured, persona file present + parses) each read **primary
evidence**: `realpath(argv[0])` and `access(F_OK)`, `stat()` on the
resolved `~/.human/` dir, the loaded `hu_config_t`'s channel array length,
and a real `hu_persona_load_from_file()` call against the configured
persona path. No check trusts a cached `last_known_good` flag — that is
the failure mode `.claude/rules/tests-that-pin-bugs.md` warns about
(reporting green from stale state). Each check appends one
`hu_diag_item_t` with `HU_DIAG_OK` or `HU_DIAG_ERR` and a
user-actionable message. The CLI maps any `HU_DIAG_ERR` to a
nonzero exit code via `return err_n > 0 ? HU_ERR_INTERNAL : HU_OK;` —
the same pattern the subcommand branch already uses on line 674. Note:
AC-9.4.5 also requires the **predicate itself** to return
`HU_ERR_NOT_FOUND` when a check fails — that's a separate return path
distinct from the per-item severity, so the function signature returns
that error after populating items (see step 3 below).

Alternative considered: a separate `human doctor install` subcommand
(matching `imessage`/`verifier`). Rejected because (a) AC-9.4.1 says
`human doctor` (the bare command) must print `install: READY` — so the
install gate is part of the **default** report, not a separate
subcommand, and (b) sprint-8's `--privacy` is also a cross-cutting
modifier of the default report, not a separate subcommand. Treating
them as flags lets both compose.

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `include/human/doctor.h` | Add `hu_doctor_check_install` prototype + doc-comment | +18 |
| `src/doctor.c` | Implement `hu_doctor_check_install` (4 sub-checks) + 2 static helpers (`resolve_binary_path`, `check_persona_parses`) | +180 |
| `src/main.c` | Add `--install` argv detection; new branch in `cmd_doctor` that calls the predicate and prints results in both text and `--json` form; map err_n>0 to nonzero exit | +90 |
| `tests/test_doctor_install.c` | **NEW** — 6 tests (green path, 4 red paths, exit-code adversarial) | +220 |
| `CMakeLists.txt` | Add `tests/test_doctor_install.c` to `human_tests` target | +1 |

Total: ~510 LOC across 5 files.

## Implementation steps (for the implementer agent)

1. **Header prototype only.** Add the `hu_doctor_check_install`
   prototype to `include/human/doctor.h` with a doc-comment that names
   the four sub-checks and the contract: "Returns `HU_OK` if all four
   checks pass; returns `HU_ERR_NOT_FOUND` if any check is red (with
   error items appended); returns `HU_ERR_OUT_OF_MEMORY` on alloc
   failure. Each item carries `category` ∈ `{"binary","config_dir","channel","persona"}`."
   Build must still pass.

2. **Failing tests first.** Create `tests/test_doctor_install.c` with
   six tests, each calling the real `hu_doctor_check_install` symbol
   (satisfies `.claude/rules/test-references-production-symbol.md`):
   - `install_check_all_green_returns_ok_and_marks_ready` — happy path
   - `install_check_missing_binary_returns_not_found` — synthesize an
     unresolvable path via `HU_IS_TEST` override
   - `install_check_missing_config_dir_returns_not_found` — temp HOME
     with no `~/.human/`
   - `install_check_no_channel_returns_not_found` — config with empty
     channels array
   - `install_check_unparseable_persona_returns_not_found` — write
     garbage JSON to the persona path, confirm `HU_DIAG_ERR`
   - `install_check_missing_persona_emits_run_doctor_fix_hint` —
     asserts the specific message text required by AC-9.4.3
   Plus one CLI-level adversarial test
   `install_cli_exits_nonzero_on_any_red` — invokes `cmd_doctor` with
   `argv = {"human","doctor","--install"}` against a synthesized broken
   config and asserts the return value is non-`HU_OK`. Per
   `tests/test_doctor_install.c` rule: the assertion is
   `HU_ASSERT_NE(rc, HU_OK)` **not** "returns some integer" — pinning
   the contract that red MUST be nonzero. All seven tests must compile
   and fail before step 3.

3. **Predicate implementation, one branch at a time.** In `src/doctor.c`:
   - Add static helper `resolve_binary_path(char **out)` that calls
     `realpath("/proc/self/exe", ...)` on Linux and `_NSGetExecutablePath`
     on macOS; on `HU_IS_TEST`, reads `HU_TEST_BINARY_PATH` env override.
   - Add static helper `check_persona_parses(alloc, cfg, char **err_out)`
     that calls the existing persona loader and reports parse errors
     verbatim.
   - Implement `hu_doctor_check_install` as four sequential appends:
     binary → config dir → channel count → persona. Each appends one
     item; any `HU_DIAG_ERR` causes the function to return
     `HU_ERR_NOT_FOUND` **after** all four checks have run (don't
     short-circuit — the user wants to see all red lines at once).
   - Run the targeted suite: `./build/human_tests --filter=install_`
     should now be green.

4. **CLI wiring.** In `src/main.c::cmd_doctor`:
   - Add `bool do_install = false;` next to `bool do_fix = false;` and
     detect `--install` in the same argv loop.
   - Add `else if (do_install) { ... }` BEFORE the legacy default
     report. The branch:
     - Calls `hu_doctor_check_install`.
     - Prints `install: READY` (AC-9.4.1) when err_n == 0, or one line
       per red check with the messages required by AC-9.4.2 and AC-9.4.3.
     - If `--json` is also set, emits `{"status":"READY"|"NOT_READY","checks":[...]}`
       (AC-9.4.4) using the same JSON-escape logic already inlined at
       lines 626–650 (copy-paste is acceptable here; do not refactor in
       this PR — see Risks).
     - Returns `err_n > 0 ? HU_ERR_INTERNAL : HU_OK`.

5. **Coexistence guard.** Add a unit test
   `install_flag_composes_with_json_flag` that passes both `--install`
   and `--json` and asserts the output is valid JSON with the
   `status` field present. Also assert that the prototype scaffolding
   for sprint-8 `--privacy` is not blocked: parsing the argv loop for
   an unknown flag does NOT abort the install branch. (This is the
   "additive subcommand pattern" requirement.)

6. **Full suite + `/verify`.** Run `./build/human_tests` (full suite),
   then run `/verify` with the contract "AC-9.4.1 through AC-9.4.5 pass
   AND `human doctor --install` exits nonzero with any of the four
   sub-checks broken." Do NOT close the task without
   `RESULT_verifier=PASS`.

## Risks

- **Coexistence with sprint-8 `--privacy` (MED probability / SMALL
  impact)**: sprint-8 is adding `--privacy` to the same `cmd_doctor`
  function. If sprint-8 lands first with an exclusive `if/else if`
  structure that returns early, sprint-9's `--install` branch may be
  unreachable, or vice versa. **Mitigation**: design step 4 specifies
  the exact branch order (`imessage|verifier|...` subcommand →
  `--install` → `--fix` → default). Both sprints must follow this
  order. Add a one-line comment at the top of `cmd_doctor` documenting
  the dispatch precedence so a future flag-add doesn't re-introduce
  exclusivity by accident. Surface this to sprint-8 tech-lead at
  merge time.

- **Doctor lies (LOW probability / LARGE impact)**: if a check reads
  cached state instead of primary evidence (e.g., "channel configured"
  meaning "channels array non-empty in JSON" without verifying the
  channel can actually open its DB / socket / pair file), the doctor
  reports READY when the install is broken — exactly the failure the
  story is designed to prevent. **Mitigation**: each sub-check
  documented in the predicate doc-comment as *which file/syscall it
  reads*. The persona check calls the real loader; the channel check
  requires at least one channel with both a name **and** a non-empty
  credential or paired-state file (not just a config stanza). Code
  review must verify each sub-check has a corresponding test that
  passes "config says yes but reality says no" through the predicate
  and confirms it reports red.

- **Tests pinning a bug (LOW / LARGE)**: per
  `.claude/rules/tests-that-pin-bugs.md`, the adversarial test
  `install_cli_exits_nonzero_on_any_red` must assert
  `HU_ASSERT_NE(rc, HU_OK)` — NOT `HU_ASSERT_EQ(rc, SOME_INT)`. If it
  asserted equality with an integer, a future change that returned 0
  on red would still match `0 == 0` if someone "fixed" the constant.
  **Mitigation**: explicit guidance in step 2 above; PR review
  checklist item.

- **JSON duplication (LOW / SMALL)**: the JSON emission for the
  subcommand branch (lines 615–653 of `src/main.c`) will be duplicated
  for the install branch. **Mitigation**: accept the duplication for
  this PR (≈40 LOC). A follow-up `DEBT-` task can extract
  `hu_diag_emit_json(FILE*, items, count)` once sprint-9 stabilizes.
  YAGNI per project CLAUDE.md.

- **Persona path resolution (LOW / SMALL)**: if persona path is
  unconfigured, distinguishing "no persona configured yet" from
  "persona configured but file missing" matters for AC-9.4.3's "MISSING
  — run `human doctor --fix`" hint. **Mitigation**: predicate emits
  distinct messages for the two states; both are red.

- **Backward compat (LOW / SMALL)**: existing `human doctor` (no
  flags) currently always returns `HU_OK` regardless of internal
  errors. Story DoD says "exits nonzero on any red check (currently it
  does not)." Changing the default-branch exit code would break
  scripts that pipe `human doctor` somewhere. **Mitigation**: change
  the exit code only for the `--install` flag in this PR. Leave the
  default-branch exit-code behavior alone; surface the broader
  "default exits nonzero on red" question as a follow-up RFC.
  Story DoD references `human doctor --install` exiting nonzero
  (the new flag), not the legacy default.

- **Observability (LOW / SMALL)**: in production we want to know when
  `--install` fires and what failed, but doctor is run interactively
  on user machines — no telemetry. **Mitigation**: print the failing
  check categories to stderr in addition to stdout when any check is
  red, so users pasting the output into a bug report include the
  diagnostic. No new logging infrastructure.

## Test strategy

- **Unit tests in `tests/test_doctor_install.c`** (7 tests above).
- **CLI integration via `cmd_doctor` direct invocation** is acceptable
  because `cmd_doctor` is exposed as a static function — bring it under
  test via `HU_IS_TEST` extern, OR factor the dispatch into a tiny
  pure function `hu_doctor_dispatch_install(argc, argv, &flags)` per
  `.claude/rules/security-predicate-extraction.md`. Implementer's choice
  — both satisfy AC-9.4.5.
- **No new integration test infra needed** — the four sub-checks all
  read filesystem state, which the test harness already mocks via
  temp HOME (pattern in existing `tests/test_doctor_*` files).
- **Full suite must run** before commit, per `tests-that-pin-bugs.md`:
  this change modifies a public contract (exit code semantics for
  `--install`), so other suites may pin the old behavior. Grep
  `git grep "human doctor" tests/` before merging.

## Acceptance criteria mapping

| AC | Test that covers it | Notes |
|---|---|---|
| AC-9.4.1 (all green → `install: READY`, exit 0) | `install_check_all_green_returns_ok_and_marks_ready` + CLI assertion of stdout `"install: READY"` and rc=`HU_OK` | Exit 0 verified via `HU_ASSERT_EQ(rc, HU_OK)`. |
| AC-9.4.2 (no channel → exact message + exit 1) | `install_check_no_channel_returns_not_found` + CLI assertion of stdout substring `"channel: NONE — run 'human doctor imessage' to pair iMessage"` and rc != `HU_OK` | Substring match on the canonical message; rc adversarial test (NE not EQ). |
| AC-9.4.3 (missing persona → exact message + exit 1) | `install_check_missing_persona_emits_run_doctor_fix_hint` + CLI assertion of `"persona: MISSING — run 'human doctor --fix' to restore defaults"` and rc != `HU_OK` | Substring match; rc adversarial. |
| AC-9.4.4 (--json any red → `{"status":"NOT_READY","checks":[...]}`) | `install_flag_composes_with_json_flag` (red case) + parse the emitted JSON and assert `status == "NOT_READY"` and each `check` has `name`, `ok`, `message`. | Use existing JSON parser in tests or naive substring check. |
| AC-9.4.5 (predicate contract under `HU_IS_TEST`) | All 7 unit tests; specifically (a) green → `HU_OK`, (b) missing persona → `HU_ERR_NOT_FOUND`, (c) missing channel → `HU_ERR_NOT_FOUND` | Predicate return code is distinct from per-item severity; both are asserted. |

## Out of scope

- Auto-repair for the install gate (delegated to existing `--fix`
  path; story explicitly notes this).
- Signature/notarization verification (sprint-8 territory).
- Changing the exit code of the **default** `human doctor` (no flag);
  see Risks. This story is `--install`-only.
- Refactoring the duplicated JSON emission (DEBT follow-up).
- Telemetry on doctor invocations (privacy thesis; not appropriate).

---

RESULT_tech-lead=DESIGN_READY
