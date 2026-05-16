# Critic findings — Sprint 5 validator-chain final follow-up

## CRITICAL (0)

None.

## HIGH (1)

- `tests/test_response_guard_retry.c:315-318` — AC-7.2 token assertions are structurally too weak: `strstr(ic.system_msg, "reasoning")` matches the word "reasoning" anywhere, including in a comment, a negative prohibition like "avoid reasoning" that was removed, or a confusingly worded instruction. The test cannot distinguish "do not narrate your reasoning" (correct) from "always show your reasoning" (wrong). The actual regression vector — third-person Seth narration — is not covered by the "third-person" token check because the instruction says `"Do not refer to yourself in third-person by name (e.g. 'As Aria, I...')"`: the word "Seth" (the leaked persona name from the original bug) never appears. A test that proves "the word third-person is in the prompt" does not prove "the model is prohibited from using Seth's name." Fix: assert a substring that is specific enough to fail if the prohibition is absent or inverted, e.g. `strstr(ic.system_msg, "Do not refer to yourself in third-person") != NULL`.

## MED (2)

- `fuzz/fuzz_output_validator_chain.c:21` — The harness calls `hu_output_validator_chain_execute` exactly once per fuzz corpus entry and then destroys the chain. It never exercises multi-call ownership transitions: REWRITE-then-REJECT, REJECT-then-rebuild, or double-free on a rewritten buffer. The story contract (AC-8.3) requires exercising "ownership transition bugs" but a single-shot harness cannot discover them. The chain is rebuilt fresh on every `LLVMFuzzerTestOneInput` invocation, so concurrent or sequential ownership state is never stressed. Fix: within a single invocation, call `hu_output_validator_chain_execute` twice on the same chain with different subsets of `data`, simulating back-to-back validation decisions.

- `tests/fixtures/check-test-refs/run-smoke-test.sh:33` — The bad.c check uses `bash "$SCRIPT" "$BAD" 2>/dev/null`. Suppressing stderr means if the script crashes (e.g. `set -euo pipefail` trips on an unset variable in a future edit), the wrapper will see a non-zero exit and report `PASS bad.c → exit 1` — the correct answer for the wrong reason. The negative assertion is unfalsifiable against script crashes. Fix: redirect stderr to a temp file and assert the exit code is exactly 1, not just non-zero.

## LOW (2)

- `fuzz/fuzz_output_validator_chain.c:17` — The persona name `"Seth"` is hardcoded in `hu_validators_build_default_outbound_chain`. If any validator uses the persona name for matching (e.g. an anti-leak rule), the corpus will only exercise that one name. Fix: derive the name from the fuzz input bytes instead.

- `sprints/sprint-4/audit.md:78-82` — The AC-5.2 follow-up table lists line numbers 1077, 1739, 9304, 10664, 11706 but the Sprint 4 original finding cited 1077, 1738, 9301, 10659, 11699. The paragraph does not acknowledge that line numbers shifted or explain why. A future auditor comparing the two documents will see a discrepancy with no explanation. Fix: add one sentence noting that earlier Sprint 5 commits shifted line numbers by small amounts.

## Per-story verdict

| Story | Verdict | Rationale |
|-------|---------|-----------|
| US-7 | NEEDS_FIXES | HIGH: token assertions cannot prove the specific CoT shape that leaked was prohibited |
| US-8 | NEEDS_FIXES | MED: single-shot harness does not cover ownership-transition bugs that are the stated reason for the harness |
| US-11 | APPROVED | All 5 annotated sites are correct; the 2 uninstrumented sites at 2103/2130 are instrumented and Pattern-C respectively; annotation wording is exact per AC-11.1; grep count 5 confirmed |
| US-12 | APPROVED | Smoke wrapper correctly exits 0/1; `set -euo pipefail` protects against silent pass; stderr suppression is a MED hygiene issue, not a correctness failure |
| US-13 | APPROVED | Comment at `include/human/persona.h:452-455` accurately states NULL = chain build failure, explicitly states empty rule set produces non-NULL chain; AC-13.1/13.2/13.3 satisfied |

RESULT_critic=HAS_FINDINGS_0_1
