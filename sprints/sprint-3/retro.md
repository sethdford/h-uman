# Sprint 3 Retrospective — Validator Chain Hardening

**Status:** CLOSED. Sprint-auditor verdict: `PASS_WITH_NOTES`. Stories closed: US-1, US-2, US-3 (3 of 8 — P0 only, P1+P2 deferred to Sprint 4).

**Commits delivered (7):**
- `6656f1d8` US-3 remove dead channel params
- `3dc7f0fb` US-1 wire structured-output per persona opt-in
- `08112101` US-2 cover Pattern C escape paths (iMessage initial)
- `a80d8be9` critic CRITICAL+MED: daemon silent-skip + silent-failure fix
- `60b3a40f` critic HIGH #2 + MED #6: format.c Slack/Discord/Telegram + vacuous assertion
- `3a9afc57` critic HIGH #3: replace inline-copy tests with mock-provider integration

**Final test count:** 10,312/10,312 passing. 0 ASan errors. 5 validator-related suites all green (output_validator 15/15, validators_builtin 10/10, validators_persona_safety 12/12, stop_sequences 8/8, structured_output 9/9, pattern_c_paths 5/5).

## What worked

- **Scope discipline.** PO drew the scope-down line clearly when context budget tightened: P0 only this sprint, P1+P2 to Sprint 4. The full 8-story sprint would have run out of context mid-execution.
- **Adversarial critic catching real bugs.** Critic found a CRITICAL silent-leak path on bus messages ≥4096 bytes that the implementer's "tests pass" claim hid. Without critic, this would have shipped.
- **Tech-lead pruning markdown stripper out of the chain.** The original design instinct was to migrate ALL of format.c's processing through the chain. Tech-lead recognized that markdown stripping is formatting (not safety) and kept it out of the chain. Smaller blast radius, cleaner separation.
- **Auditor catching DoD literalism issue.** The DoD said "grep returns zero hits" but the implementation has legitimate fallback paths. Auditor flagged this as documentation needing update, not as a defect. Right call.
- **Iterative critic round.** Critic → fix → re-verify cycle landed 3 critic findings in 3 small commits without disturbing the wave-1 commits. Each finding fix is bisectable.

## What broke

- **Tests inline-copying production code (critic HIGH #3).** The first implementer wrote 3 tests that duplicated the production conditional locally. If the conditional was deleted from `agent_turn.c`, the tests would still pass. This is a recurring anti-pattern — implementers reach for "test the logic" instead of "exercise the code path." Worth a hookify rule: any new test in `tests/test_*` should grep for the function name from the production path it claims to exercise.
- **Static callback testability gap (US-2 Site A).** `daemon.c`'s stream callback is static and can't be invoked from a test. The test had to rebuild the chain inline — same self-reference anti-pattern, with a structural cause this time. Mitigation: when Sprint 4 ships US-6 (E2E daemon integration test), Site A finally gets real production-path coverage.
- **Implementer tool-use budget.** Wave 1 implementer hit 136 tool uses across 3 stories — close to the budget cap. Future P0+P1 batches risk truncation. Split P0 from P1 in Sprint 4 dispatch.
- **Diagnostic noise from clang-tidy.** Every implementer session emitted ~10 clang-tidy warnings on pre-existing code (narrowing conversions, screaming-snake variables). Distracts from real issues. Worth a follow-up: add a `.clang-tidy` block to silence the known-noise checks at the project level.

## What changed mid-sprint

- **Original plan: 8 stories.** Realized at planning: that's too many for one execution window. Scoped to P0 (3 stories) for this sprint. PO documented the cut explicitly.
- **Pattern C scope reinterpretation.** AC2 said "format.c channel path" — critic correctly read this as "all channels" not "any one channel". Implementer expanded coverage from iMessage to Slack/Discord/Telegram in the critic-fix commit.
- **DoD literalism.** Auditor flagged that a literal `grep` check in DoD conflicts with legitimate fallback paths. Resolution deferred: either PO updates the DoD or the fallbacks are reformulated as separate functions. Sprint closes with note.

## What's next (Sprint 4 candidate backlog)

From the original PO backlog, deferred to Sprint 4:
- **US-4 (P1, M):** Cache validator chain on `hu_persona_t` to eliminate per-message allocations. Depends on US-1 (now done) for the chain's persona-driven composition.
- **US-5 (P1, S):** Telemetry hook for chain REJECT/REWRITE events via `hu_observer_t`.
- **US-6 (P1, M):** End-to-end daemon integration test injecting LEAK_F1 → assert wire content. Closes the Site A inline-rebuild gap from this sprint.
- **US-7 (P2, S):** Verify retry-slim under chain REJECT produces clean output; add anti-CoT instructions to slim prompt if not.
- **US-8 (P2, S):** Fuzz harness for `hu_output_validator_chain_execute`.

Plus emergent from this sprint's findings:
- **US-9 (new):** Resolve DoD literalism on Pattern C (auditor note). Either update DoD or refactor fallbacks.
- **US-10 (new):** Hookify rule — new test files must reference the production function path they exercise.

## Agent tuning candidates

No agent had ≥2 verifier failures. No `/tune-agent` recommendation this sprint.

Worth noting for the general-purpose implementer agent: the recurring "tests inline-copy production code" anti-pattern is a learnable lesson. If it recurs in Sprint 4, propose tuning.
