# Design for US-9.2: Onboard post-success nextstep (P0)

Story: `sprints/sprint-9/stories.md` (lines 44-64)
Sprint: 9 — Distribution MVP
Risk tier: **LOW** (UX text + post-write config-validation read).
Worktree: `/Users/sethford/Projects/h-uman/.claude/worktrees/sprint-9-distribution`
Branch: `sprint-9-distribution-mvp`

---

## Approach

The current dead-end at `src/onboard.c:492-496` prints exactly one of two generic
lines:

```c
printf("\nConfig written to %s\n", config_path);
if (is_apple_provider(provider))
    printf("Run 'human agent' to start chatting with Apple Intelligence.\n");
else
    printf("Run 'human agent' to start chatting.\n");
```

That tells the user the wizard worked but not what to do *next*. The cheapest fix
is a single new helper, `emit_whats_next(...)`, that:

1. Reads the host platform via a compile-time guard (`__APPLE__`) — same pattern
   already used at `src/onboard.c:309` to gate the provider menu, so no new
   abstraction is introduced.
2. Reads `provider` + `config_path` (already in scope at line 492) and the
   `parsed_ok` result of a post-write `hu_config_load` call.
3. Prints a 3-line "What's next" block whose exact format is locked by tests.

We add a `HU_IS_TEST`-gated string-capture parameter to a new internal helper
(`hu_onboard_nextstep_format`) — a **pure function** that builds the block into a
caller-provided buffer. The wizard calls it twice in production: once with a real
heap buffer (then `fputs` to stdout), and never in tests directly. Tests link
against `hu_onboard_nextstep_format` and assert on the buffer's exact content.

This applies `~/.claude/rules/security-predicate-extraction.md` in spirit: extract
the textual decision (the "what message do we print given (platform, provider,
already-exists, parsed-ok)?" question) into a pure function so we don't have to
re-launch a subprocess and parse stdout to test it. The print path stays inside
the existing `#ifndef HU_IS_TEST` guard at lines 453/488; the format function is
test-callable on both sides of the guard.

We also lift the early-exit "Config already exists" message at line 288 to call
the same helper so AC-9.2.4 is covered by the same code path (one source of
truth for what the wizard tells the user).

### Alternatives considered & rejected

- **String-capture parameter on `hu_onboard_run_with_args` itself.** The story's
  Test seam note (line 62) suggests this. Rejected: changes a public signature in
  `include/human/onboard.h` solely for testability when extracting a smaller pure
  predicate gives the same coverage. The wizard remains thin.
- **Smoke-test only, no unit test.** AC-9.2.5 explicitly accepts this for the
  test under `HU_IS_TEST` because the print is gated. The user's design brief
  (the parent agent's instructions to me) overrides this: it asks for
  `tests/test_onboard_nextstep.c` to pin the exact text. The pure-format-function
  approach satisfies BOTH — `hu_onboard_run` still returns `HU_OK` in test mode
  (existing tests at `tests/test_subsystems.c:122` continue to pass), AND the
  format function is unit-testable without crossing `HU_IS_TEST`.
- **Channel autodetect via `hu_config_get_channels`.** Rejected as overscope: the
  wizard does not write a channels list (line 449 only writes the gateway). The
  channel-specific guidance is platform-derived, not config-derived.

---

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `include/human/onboard.h` | Add prototype for `hu_onboard_nextstep_format` + a typedef `hu_onboard_nextstep_ctx_t` describing inputs (provider, config_path, platform_is_apple, already_exists, parsed_ok) | +35 |
| `src/onboard.c` | Add `hu_onboard_nextstep_format` (pure function, outside `HU_IS_TEST` guard so tests link); replace lines 492-496 with a call to it + `fputs`; replace line 288 with same; call `hu_config_load` on `config_path` post-`fclose` to compute `parsed_ok` for AC-9.2.3 | +90 / -7 |
| `tests/test_onboard_nextstep.c` | New file. 8 test cases pinning exact stdout-bound strings, plus 2 adversarial tests asserting the OLD generic messages are NOT emitted | +210 |
| `tests/CMakeLists.txt` | Wire new test source into `human_tests` target | +1 |
| `tests/test_subsystems.c` | No change. Existing `test_onboard_run_test_mode` still passes because `hu_onboard_run` returns early in `HU_IS_TEST` (line 182-185) | 0 |

Net: roughly +335 / -7 lines.

---

## Contract for `hu_onboard_nextstep_format`

```c
typedef struct hu_onboard_nextstep_ctx {
    const char *config_path;       /* NUL-terminated; required. */
    const char *provider;          /* "apple" | "mlx_local" | "gemini" | ... */
    bool platform_is_apple;        /* compile-time __APPLE__ at call site */
    bool already_exists;           /* true => print early-exit variant */
    bool parsed_ok;                /* result of post-write hu_config_load */
} hu_onboard_nextstep_ctx_t;

/* Writes a NUL-terminated, newline-terminated block to `out` (size `out_sz`).
 * Returns HU_OK on success, HU_ERR_BUFFER_TOO_SMALL if truncated.
 * Pure function: no I/O, no globals, no allocation. */
hu_error_t hu_onboard_nextstep_format(const hu_onboard_nextstep_ctx_t *ctx,
                                      char *out, size_t out_sz);
```

### Output truth table (pin these exactly)

| `already_exists` | `parsed_ok` | `platform_is_apple` | Block content (line-by-line) |
|---|---|---|---|
| true | (n/a) | true | `Config already exists at <path>.\nRun 'human doctor' to check status, or 'human doctor imessage' to pair iMessage.\n` |
| true | (n/a) | false | `Config already exists at <path>.\nRun 'human doctor' to check status.\n` |
| false | false | (any) | `Config written to <path>.\nWarning: config written but failed to parse — run 'human doctor --fix' to repair\n` and the wizard returns `HU_ERR_IO` (the caller, not the formatter, enforces the return). |
| false | true | true | `Config verified OK\nConfig written to <path>.\nWhat's next:\n  1. Pair iMessage:  human doctor imessage\n  2. Start the agent: human agent\n` |
| false | true | false | `Config verified OK\nConfig written to <path>.\nWhat's next:\n  1. Start the agent: human agent\n  (Tier-1 channels other than iMessage require manual config — see docs/guides/channels.md)\n` |

The leading `\n` is omitted from the formatter; the production caller prefixes
one before `fputs` to match the existing blank-line spacing.

### Why exact-match, not substring

`~/.claude/rules/tests-that-pin-bugs.md` warned that loose substring assertions
let the old buggy text slip through. Our adversarial tests use `strcmp` on the
full buffer (post-formatting, with `<path>` substituted to a fixed test value).
A test named `test_apple_path_emits_imessage_step` asserting
`strstr(buf, "human") != NULL` would pass even with the old "Run 'human agent'"
message — so we don't do that.

---

## Implementation steps (for the implementer agent)

1. **Add the typedef + prototype to `include/human/onboard.h`.** No body yet.
2. **Stub `hu_onboard_nextstep_format` in `src/onboard.c`** outside the
   `HU_IS_TEST` guard — just `snprintf(out, out_sz, "stub\n"); return HU_OK;`.
   Confirm `./build/human_tests` still passes (10,000+ tests, 0 ASan).
3. **Create `tests/test_onboard_nextstep.c`** with the 8 happy-path cases (one
   per row of the truth table × Apple/non-Apple where applicable) plus 2
   adversarial cases:
   - `test_apple_success_does_not_emit_old_generic_message` — asserts
     `strstr(buf, "Run 'human agent' to start chatting with Apple Intelligence.") == NULL`.
   - `test_nonapple_success_does_not_emit_old_generic_message` — asserts
     `strstr(buf, "Run 'human agent' to start chatting.\n") == NULL` AND that
     the exact-match block is what's present.
   Wire into `tests/CMakeLists.txt`. All 10 NEW tests should FAIL.
4. **Implement `hu_onboard_nextstep_format` truth-table by truth-table.**
   Re-run the new test file after each branch; the rest of the suite continues
   to pass at every step.
5. **Wire the call sites in `src/onboard.c`:**
   - Line 288 early-exit: replace with a `hu_onboard_nextstep_format` call
     using `already_exists=true`.
   - Lines 492-496: after `fclose(f)`, call `hu_config_load` on `config_path`
     into a stack `hu_config_t`, capture `parsed_ok`, then call
     `hu_onboard_nextstep_format` with `already_exists=false`. If
     `parsed_ok == false`, the wizard returns `HU_ERR_IO` per AC-9.2.3.
   - Guard the actual `fputs(buf, stdout)` with `#ifndef HU_IS_TEST` so
     `tests/test_subsystems.c::test_onboard_run_test_mode` (line 122) keeps
     passing unchanged.
6. **Run `scripts/agent-preflight.sh`** to catch the local build issues, then
   `./build/human_tests --filter=onboard_nextstep` to confirm the new file
   passes targeted, then `./build/human_tests` full suite (per
   `.claude/rules/tests-that-pin-bugs.md`: never trust targeted-only on a
   public-contract change).
7. **`/verify`** to capture evidence: `RESULT_verifier=PASS` required.
8. **Manual smoke-test** on macOS per AC-9.2.5's PR-description requirement.
   Record terminal session showing the new block; attach to PR description.

---

## Risks

- **Backward compat (LOW/SMALL):** The function signatures of `hu_onboard_run`
  and `hu_onboard_run_with_args` are unchanged. Header adds one new public
  symbol — additive, not breaking. Mitigation: confirm by `git grep
  hu_onboard_` across `apps/`, `ui/`, and external consumers; expect zero
  external callers since onboard is invoked only from `src/main.c`.
- **Test pinning regression (LOW/MED):** The 8 exact-match tests will lock the
  wording. A future copy-edit "Pair iMessage:" → "Pair iMessage —" breaks
  the suite. Mitigation: that is the **point** per the user brief, and
  `~/.claude/rules/tests-that-pin-bugs.md` rewards this. Implementers who
  change the wording must update the tests in the same PR — a healthy gate.
- **Post-write `hu_config_load` side effects (LOW/SMALL):** `hu_config_load`
  allocates into the passed allocator. AC-9.2.3 requires we call it; we must
  free or reuse a scoped allocator. Mitigation: use `alloc` (already in
  scope at line 284) and immediately `hu_config_free` the result. Pin the
  no-leak property with ASan via the existing dev preset.
- **HU_IS_TEST guard regression (LOW/LARGE if hit):** The production
  `hu_onboard_run_with_args` body lives inside `#else` to the `#ifdef
  HU_IS_TEST` at line 181. The new pure formatter MUST be defined BEFORE
  the `#ifdef HU_IS_TEST` block or inside both arms. Mitigation: put the
  formatter at file-scope before line 181, alongside `hu_starter_persona_json`
  (which uses the same pattern per the comment at lines 218-220).
- **Concurrency (none):** Wizard is a single-process interactive command.
- **Migration (none):** No on-disk format change. `config.json` schema
  unchanged.
- **Observability (LOW):** The new block IS the observability. If a user
  pastes their terminal output, support can diagnose from the printed
  "What's next" lines.

---

## Test strategy

- **New unit tests** in `tests/test_onboard_nextstep.c` (10 cases):
  - 5 truth-table happy paths (one per row above).
  - 2 adversarial "old generic message absent" assertions.
  - 1 `HU_ERR_BUFFER_TOO_SMALL` case (pass `out_sz = 4`).
  - 1 `NULL` ctx → returns `HU_ERR_INVALID_ARGUMENT`.
  - 1 cross-platform check: same `(already_exists=true, platform_is_apple=true)`
    block on both macOS and non-macOS hosts (the formatter is compile-time
    pure; portability is structural).
- **Test file MUST reference production symbol** per
  `.claude/rules/test-references-production-symbol.md`: tests include
  `"human/onboard.h"` and call `hu_onboard_nextstep_format` directly. No
  local reimplementation. (Implied production module:
  `tests/test_onboard_nextstep.c` → `src/onboard.c`.)
- **Existing tests** in `tests/test_subsystems.c` (lines 117-130) and
  `tests/test_new_modules.c` (lines 593-606) remain unchanged and must
  continue to pass. They exercise the early-return-in-test-mode path
  (`hu_onboard_run` returns `HU_OK` immediately) which is unchanged.
- **Full suite** required pre-merge per AC-9.2 DoD: 10,000+ tests, 0 failures,
  0 ASan errors.
- **Manual smoke-test** for AC-9.2.5: implementer runs `./build/human` from
  a temp `$HOME`, captures stdout for (a) fresh-install macOS, (b) fresh-install
  non-macOS (Linux CI runner is enough), (c) repeat-run with existing config.
  Pastes transcript into PR description.

---

## Acceptance criteria mapping

| AC | Covered by |
|---|---|
| AC-9.2.1 (macOS success → config path + `human doctor imessage` + `human agent` in order) | Truth-table row "false / true / true"; unit test `test_apple_success_emits_full_whats_next_block`; smoke-test transcript in PR |
| AC-9.2.2 (non-macOS success → no iMessage step) | Truth-table row "false / true / false"; unit test `test_nonapple_success_omits_imessage_step`; adversarial counterpart asserts `strstr(buf, "imessage") == NULL` |
| AC-9.2.3 (post-write `hu_config_load`; `Config verified OK` or warning + `HU_ERR_IO`) | New `hu_config_load` call at the new call site in `src/onboard.c`; truth-table rows for `parsed_ok=true` and `parsed_ok=false`; unit tests `test_parsed_ok_emits_verified_line` and `test_parsed_fail_emits_warning_and_caller_returns_io` (the latter also asserts the wizard return code via a separate small test that drives the call site directly under `HU_IS_TEST` — possible because the call site is what returns `HU_ERR_IO`, not the formatter) |
| AC-9.2.4 (existing-config early exit → path + doctor + imessage hints) | Truth-table rows "true / (n/a) / *"; unit tests `test_already_exists_apple_shows_imessage_hint` and `test_already_exists_nonapple_omits_imessage_hint`; adversarial `test_already_exists_does_not_emit_old_bare_message` asserts `strcmp` against the old `"Config already exists. Run 'human doctor' to check status.\n"` |
| AC-9.2.5 (`hu_onboard_run_with_args` under `HU_IS_TEST` returns `HU_OK`, no stdout assertions) | Existing `tests/test_subsystems.c::test_onboard_run_test_mode` (line 122) continues to pass unchanged; new tests target the pure formatter, not the wizard wrapper, so they neither rely on nor exercise stdout from `hu_onboard_run_with_args` |

---

## Anti-patterns checked against

- `~/.claude/rules/tests-that-pin-bugs.md` — adversarial tests assert the OLD
  text is **absent** (`strstr == NULL` and `strcmp != 0`), not merely that the
  NEW text is present. Test names match assertions.
- `.claude/rules/test-references-production-symbol.md` — new test file
  includes `human/onboard.h` and calls `hu_onboard_nextstep_format`. No local
  helper that reimplements the formatter.
- `.claude/rules/security-predicate-extraction.md` — text-formatting decision
  extracted into a pure function so tests don't have to cross the
  `HU_IS_TEST` boundary nor spawn a subprocess.
- KISS/YAGNI (project CLAUDE.md) — one new typedef, one new function. No
  config-flag for "what's next style". No persona-conditional wording. The
  channel hint is platform-derived, not user-configurable.

---

## Out of scope (parking lot)

- Multi-channel selection in the wizard (per story line 63).
- Wizard GUI / TUI (per story line 63).
- Localization of the "What's next" block — English only for Sprint 9.
- `human doctor imessage` itself — that command is exercised in US-9.4
  (P1, separate design).
