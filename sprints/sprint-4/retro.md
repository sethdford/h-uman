# Sprint 4 Retrospective — Validator Chain Hardening Follow-up

**Status:** CLOSED. Sprint-auditor: `PASS_WITH_NOTES`. 18 of 21 ACs DELIVERED clean; 2 ACs PARTIAL (carry to Sprint 5).

**Stories delivered:** US-4, US-5, US-6, US-9, US-10 (5 of 5 P1+P2+XS in scope).

**Commits (8):**
- `3429d068` US-9 — Pattern C fallback annotation + Sprint 3 DoD clarification
- `3eab2730` US-10 — hookify rule preventing "test inlines production" anti-pattern
- `52e3c611` housekeeping (PO + designs)
- `bc32a082` US-5 — validator decision telemetry events
- `8a9f1e66` US-4 — cache validator chain on hu_persona_t
- `ee448c84` US-6 — end-to-end E2E test via mock provider
- `088cd019` critic HIGH fix — telemetry coverage at 5 remaining sites
- `72b79214` critic MED fix — check-test-references content-scan fallback

**Final test count:** 10,325/10,325 passing. ASan clean.

## What worked

- **Continuous critic + sprint-auditor catching real gaps.** Critic flagged that US-5 telemetry only covered 5 of 16 chain call sites — the implementer's "tests pass" claim hid the gap. Fix landed before sprint close. Auditor then independently caught that the fix still left 5 daemon paths uncovered.
- **The new hookify rule (US-10) self-validated.** Sprint 4's own test files were the first to be exercised by the rule; the rule found one real gap (the test_pattern_c_paths.c filename mismatch) and the MED fix tightened the script's content-scan fallback.
- **Chain caching (US-4) ownership discipline.** The implementer's `bool chain_owned` flag correctly distinguishes "borrow from persona cache" from "owned inline build". No double-free, no leak under ASan.
- **E2E test (US-6) deletion-sensitivity.** The two-phase mock provider correctly models the REJECT→retry→recover sequence, so removing the chain execute call at agent_turn.c:5605 would actually make the test fail. This closes the Sprint 3 audit's Site A inline-rebuild gap.
- **PO descoping was honest.** Dropping US-7 (retry-slim) + US-8 (fuzz) to Sprint 5 was the right call given Sprint 3's tool-use ceiling experience. The sprint that fits the window is the sprint that ships.

## What broke

- **Implementer tool-use ceiling hit twice.** Wave-2 implementer cut off mid-US-5 after 73 tool uses (observer scaffolding added, no emission, no commit). Wave-3 implementer for the critic fixes cut off after 53 tool uses (4 of 5 sites wired, no script fix, no commit). In both cases a "finisher" subagent picked up the in-progress work successfully. The pattern: complex C work touching many files saturates tool-use budgets faster than smaller scoped work. Sprint 5 should split P1 stories into two passes by default.

- **AC-5.2 partial — 5 daemon sites still uncovered.** Audit caught that `daemon.c:1077, 1738, 9301, 10659, 11699` (scheduled-flush, proactive-checkin, etc.) still execute the chain without emitting telemetry events. Each of these is in a path that lacks an observer pointer in scope. Either: (a) plumb an observer through those paths (refactor scope), or (b) document each site with a `// telemetry: observer not in scope` comment.

- **AC-10.4 negative-case unprovable from shipped artifacts.** The bundled `tests/fixtures/check-test-refs/bad.c` is rejected by the script's `tests/test_*.c` path filter before reaching the content scan. The rule is functionally correct; the smoke test scaffolding is broken. Fix: rename the fixture to a path the filter matches, or invoke the script with a path argument that bypasses the filter.

- **`sprints/sprint-4/critic.md` was created during sprint, not committed until late.** The critic agent wrote findings to a file outside any commit; only the MED-fix commit `72b79214` picked it up. Process improvement: critic findings should commit alongside the fix that addresses them.

## What changed mid-sprint

- PO scope decision: 5 stories instead of 7 (deferred US-7 + US-8 to Sprint 5). Same descoping discipline that Sprint 3 used.
- Tech-lead: persona name treated as immutable post-load (US-4). Documented invariant rather than supporting mutation + chain rebuild.
- Critic HIGH telemetry gap: 5 sites covered after the fix, 5 still uncovered (the daemon.c outbound paths without observer). Carried as note.

## What's next (Sprint 5 candidate backlog)

From original PO backlog + emergent from Sprint 4 retro:
- **US-7 (P2, S, deferred):** Verify retry-slim under chain REJECT produces clean output; add anti-CoT instructions if needed.
- **US-8 (P2, S, deferred):** Fuzz harness for hu_output_validator_chain_execute.
- **US-11 (new, S):** Close AC-5.2 partial — wire observer into the 5 remaining daemon.c sites OR document carve-out per site.
- **US-12 (new, XS):** Fix AC-10.4 smoke-test fixture path so the negative case is actually reachable.
- **US-13 (new, XS):** Fix the doc comment on `persona->outbound_chain` (auditor LOW finding — "NULL means OOM, not zero validator rules").

## Agent tuning candidates

The recurring "implementer cut off mid-work and didn't commit" pattern appeared in both wave-2 and the critic-fix work. Two failures of the same shape on the general-purpose agent. Worth `/tune-agent` proposal:
- Implementer prompts should bias toward "commit checkpoint after each file group" rather than "commit at end of all stories." A working-tree-only DONE is a near miss that costs another dispatch to recover.

Not blocking — pattern is recoverable via finisher dispatch — but improvement candidate.
