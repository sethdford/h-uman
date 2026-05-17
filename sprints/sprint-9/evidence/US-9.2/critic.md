# Critic findings — US-9.2 Onboard post-success nextstep

## HIGH (1)

- `src/onboard.c:236-238` — Truncation returns `HU_ERR_IO`, but the design contract (US-9.2.md:97 and `include/human/onboard.h:79`) specifies `HU_ERR_BUFFER_TOO_SMALL`. The test at `tests/test_onboard_nextstep.c:228` asserts `HU_ERR_IO` — the test was written to match the wrong implementation, not the contract. The header docblock says `HU_ERR_IO if the message would be truncated`, but the design doc says `HU_ERR_BUFFER_TOO_SMALL`. These are different error codes. The only caller that checks the return of `hu_onboard_nextstep_format` is in `src/onboard.c:363` and `src/onboard.c:594` — both discard the error (`if (... == HU_OK) fputs`). The formatter's own truncation error is silently swallowed: if `nbuf[2048]` is ever too small (e.g. a very long `$HOME` path), the user sees nothing and the wizard returns `HU_OK`. Fix: check the formatter return at both call sites in `src/onboard.c` and surface a diagnostic if truncated; align the error code between header, design doc, and test to whichever is authoritative.

## MED (3)

- `src/onboard.c:594-597` — When `hu_onboard_nextstep_format` returns `HU_ERR_IO` (truncation), the output is silently dropped: `fputs` is never called, `parsed_ok` may still be true, and the function returns `HU_OK` — the user gets a blank nextstep on a path longer than ~1900 bytes. Same silent-drop at line 363. Fix: add `else { fprintf(stderr, "internal error: nextstep buffer too small\n"); }` at both call sites.

- `tests/test_onboard_nextstep.c:57-71` — `test_parsed_fail_emits_warning` covers only `platform_is_apple=true`. The design truth table says `parsed_ok=false` with `already_exists=false` is "(any)" platform — there is no test pinning the non-Apple variant. If a future edit adds a platform branch inside the `!ctx->parsed_ok` arm, the non-Apple warning output goes untested. Fix: add a second `test_parsed_fail_nonapple_emits_same_warning` case with `platform_is_apple=false`.

- `tests/test_onboard_nextstep.c:117-148` — The two adversarial "old message absent" tests only check for the two exact old strings that appeared in the original code. A third old string from the same block — `"Config written to %s\n"` (without "verified OK" prefix) — is not checked for absence in the success path. If the new output were accidentally reverted to just the raw config-written line, neither adversarial test would catch it because neither looks for that string. Fix: add `HU_ASSERT_TRUE(strstr(buf, "Config written to") == NULL || strstr(buf, "Config verified OK") != NULL)` in the success-path adversarial tests, or add a dedicated adversarial case.

## LOW (1)

- `sprints/sprint-9/designs/US-9.2.md:108` vs `include/human/onboard.h:79` — The design doc specifies the formatter returns `HU_ERR_BUFFER_TOO_SMALL` on truncation; the header says `HU_ERR_IO`. These are different claims about the same contract. One of them is wrong. The test aligns with the implementation (HU_ERR_IO), but the design document is now misleading for any future implementer. Fix: update the design doc to reflect `HU_ERR_IO`, or update both the header and implementation to use `HU_ERR_BUFFER_TOO_SMALL`.

## Cross-agent regression risk

None. `hu_onboard_run_with_args` signature is unchanged. The only caller is `src/main.c:952`, which propagates the return to the shell exit code — `HU_ERR_IO` (non-zero) on a bad-parse config is the intended behavior and main.c does not special-case it.
