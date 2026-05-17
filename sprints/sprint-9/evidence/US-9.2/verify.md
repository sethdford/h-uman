# Verifier Evidence — US-9.2 Onboard Post-Success Nextstep

Date: 2026-05-17
Commit: 21759c51
Branch: impl/US-9.2
Verifier: claude-sonnet-4-6

## Checklist

1. **cmake --build --preset dev (-Werror clean)**
   PASS — build completed with 0 warnings/errors.
   `[100%] Built target human` — final output line, no Werror hits.

2. **./build/human_tests --filter=onboard_nextstep (13 tests)**
   PASS — 13/13 passed via `--suite="Onboard nextstep formatter (US-9.2)"`.
   All 13 named tests individually PASS in output.

3. **./build/human_tests --filter=test_subsystems (AC-9.2.5 preservation)**
   PASS — `--suite="Subsystems (skillforge, onboard, daemon, migration)"` → 45/45 passed.
   `test_onboard_run_test_mode` explicitly PASS at line matching tests/test_subsystems.c:122 contract.

4. **./build/human_tests (full suite — confirm pre-existing flake only)**
   PASS — `Results: 10401/10402 passed, 1 FAILED`.
   Sole failure: `route_populates_global_log` at tests/test_model_router.c:338.
   That is the declared pre-existing flake. No US-9.2 test among failures.

5. **scripts/check-test-references.sh tests/test_onboard_nextstep.c (exit 0)**
   PASS — exit 0. Test file includes `human/onboard.h` and calls `hu_onboard_nextstep_format` directly.

6. **AC mapping: each AC-9.2.1–9.2.5 has a test pinning exact text via strcmp**
   PASS — design doc AC table confirmed:
   - AC-9.2.1: `test_apple_success_emits_full_whats_next_block` — HU_ASSERT_STR_EQ full block.
   - AC-9.2.2: `test_nonapple_success_omits_imessage_step` — HU_ASSERT_STR_EQ full block.
   - AC-9.2.3: `test_parsed_fail_emits_warning` — HU_ASSERT_STR_EQ exact warning line.
   - AC-9.2.4: `test_already_exists_apple_*` and `test_already_exists_nonapple_*` — HU_ASSERT_STR_EQ.
   - AC-9.2.5: covered by pre-existing test_subsystems suite (45/45 PASS above).

7. **Adversarial: >=2 tests assert old generic message absent via strstr==NULL**
   PASS — grep confirmed 4 strstr(...)==NULL assertions across 2 dedicated adversarial tests:
   - `test_apple_success_does_not_emit_old_generic_message`: lines 129, 131
   - `test_nonapple_success_does_not_emit_old_generic_message`: lines 145, 146
   Old strings asserted absent: `"Run 'human agent' to start chatting with Apple Intelligence."` and
   `"Run 'human agent' to start chatting.\n"`.

8. **hu_config_load_from called after fclose in src/onboard.c**
   PASS — grep shows fclose(f) at line 527, hu_config_load_from at line 574.
   Ordering is correct: file closed before re-read.

9. **ASan**
   INCONCLUSIVE — ASan errors would surface during full suite run; none were captured in the
   test output (no ASAN ERROR lines observed). Dev preset enables ASan by default. No explicit
   separate ASan-clean confirmation was captured beyond the absence of errors in the run output.

## Summary

Verified 8/9 behaviors. 0 failed. 1 inconclusive (ASan — no errors observed, absence is
strong evidence but no explicit "0 ASan errors" line was captured from the run output).

RESULT_verifier=PASS story=US-9.2
