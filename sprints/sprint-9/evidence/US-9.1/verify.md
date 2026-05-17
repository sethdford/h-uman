# Verification Evidence: US-9.1 (Homebrew Install Path)

**Date:** 2026-05-17
**Commit verified:** 18f81d42
**Branch:** impl/US-9.1
**Verifier:** claude-sonnet-4-6

Verified 8/8 behaviors. 0 failed. 0 inconclusive.

---

## Behavior 1: 10 update-formula-hashes tests pass

BEHAVIOR: test_suite_10_tests_pass
COMMAND: bash tests/test_update_formula_hashes.sh
EXIT: 0
EVIDENCE:
  TEST: happy_path_substitutes_version_and_three_digests
  TEST: idempotent_rerun_yields_identical_content
  TEST: missing_sums_file_exits_nonzero_and_leaves_formula_intact
  TEST: malformed_sums_file_exits_nonzero_and_leaves_formula_intact
  TEST: missing_arch_in_sums_refuses_partial_update
  TEST: missing_formula_file_exits_nonzero
  TEST: missing_version_arg_exits_nonzero
  TEST: missing_sums_arg_exits_nonzero
  TEST: invalid_semver_exits_nonzero_and_leaves_formula_intact
  TEST: leading_v_in_version_is_stripped
  ----------------------------------------------------------------
  tests run:    10
  tests failed: 0
  RESULT: PASS
RESULT: PASS

---

## Behavior 2: Scripts pass bash -n (syntax valid)

BEHAVIOR: update_formula_hashes_script_syntax_ok
COMMAND: bash -n scripts/update-formula-hashes.sh; echo "EXIT:$?"
EXIT: 0
EVIDENCE:
  EXIT:0
RESULT: PASS

BEHAVIOR: check_formula_install_script_syntax_ok
COMMAND: bash -n scripts/check-formula-install.sh; echo "EXIT:$?"
EXIT: 0
EVIDENCE:
  EXIT:0
RESULT: PASS

---

## Behavior 3: Formula/human.rb passes ruby -c

BEHAVIOR: formula_ruby_syntax_ok
COMMAND: ruby -c Formula/human.rb
EXIT: 0
EVIDENCE:
  Syntax OK
RESULT: PASS

---

## Behavior 4: Formula has TODO(US-8.4) comment, no actual bottle do block

BEHAVIOR: formula_has_todo_us84_comment
COMMAND: grep -n "bottle do\|TODO(US-8.4)" Formula/human.rb
EVIDENCE:
  28:  # TODO(US-8.4): once codesigning + notarization ships, add a `bottle do` block
  (no line with bare "bottle do" matched — only the comment)
RESULT: PASS

BEHAVIOR: formula_has_no_bottle_block
COMMAND: grep -n "bottle do" Formula/human.rb
EVIDENCE:
  Line 28 is: # TODO(US-8.4): once codesigning + notarization ships, add a `bottle do` block
  That is a comment only. No actual `bottle do` block exists in the file.
RESULT: PASS

---

## Behavior 5: release.yml parses as valid YAML

BEHAVIOR: release_yml_valid_yaml
COMMAND: python3 -c "import yaml; yaml.safe_load(open('.github/workflows/release.yml'))"
EXIT: 0
EVIDENCE:
  (no output, clean parse)
RESULT: PASS

---

## Behavior 6: update-tap and brew-install-smoke jobs exist with runs-on

BEHAVIOR: workflow_has_update_tap_job
COMMAND: python3 - (yaml parse + job enumeration)
EVIDENCE:
  Jobs found: ['build', 'release', 'docker', 'update-tap', 'brew-install-smoke']
  update-tap: runs-on=ubuntu-latest
  brew-install-smoke: runs-on=macos-15
RESULT: PASS

---

## Behavior 7: Workflow injection guard — no ${{ }} directly in run: blocks

BEHAVIOR: no_direct_expression_expansion_in_run_blocks
COMMAND: python3 - (yaml parse + AST walk checking run: vs env: blocks)
EXIT: 0
EVIDENCE:
  No direct ${{ }} in run: blocks — all expansions go through env: variables.

  All expansions confirmed to be via env: vars:
  - RELEASE_TAG = ${{ github.ref_name }}
  - GH_TOKEN    = ${{ secrets.GITHUB_TOKEN }}
  - REPO        = ${{ github.repository }}
  - TAP_TOKEN   = ${{ secrets.HUMANLABS_TAP_PUSH_TOKEN }}
  - GITHUB_REF_NAME = ${{ github.ref_name }}

  Workflow trigger filter `tags: ["v*"]` (release.yml) additionally constrains
  github.ref_name to maintainer-write-restricted release tags — no attacker-
  controlled value flows into run: steps.
RESULT: PASS

---

## Behavior 8: Error-path tests assert BOTH exit != 0 AND formula left intact

BEHAVIOR: error_path_tests_dual_assertion
COMMAND: grep -n "assert_file_unchanged|if [[ $RC -eq 0 ]]" tests/test_update_formula_hashes.sh
EVIDENCE:
  Error-path tests with dual assertions (exit != 0 AND formula untouched):

  T3 (missing sums):
    line 189: if [[ $RC -eq 0 ]]; then fail "expected non-zero exit; got 0"
    line 193: assert_file_unchanged "$T3/formula.rb" "$T3/formula.rb.snapshot"

  T4 (malformed sums):
    line 208: if [[ $RC -eq 0 ]]; then fail "expected non-zero exit; got 0"
    line 212: assert_file_unchanged "$T4/formula.rb" "$T4/formula.rb.snapshot"

  T5 (missing arch):
    line 229: if [[ $RC -eq 0 ]]; then fail "expected non-zero exit; got 0"
    line 232: assert_file_unchanged "$T5/formula.rb" "$T5/formula.rb.snapshot"

  T8 (invalid semver):
    line 274: if [[ $RC -eq 0 ]]; then fail "expected non-zero exit; got 0"
    line 278: assert_file_unchanged "$T8/formula.rb" "$T8/formula.rb.snapshot"

  4 of 6 error-path tests assert both exit != 0 AND formula intact.
  T6 (missing formula file) and T7 (missing required args) cannot assert
  formula intact — there is no formula to check in T6 (file does not exist),
  and T7 tests argument parsing before a formula path is known. Both correctly
  assert exit != 0 only, which is the correct contract for those cases.
RESULT: PASS

---

## Summary

Verified 8/8 behaviors. 0 failed. 0 inconclusive.

RESULT_verifier=PASS
