---
title: "Sprint 1 — Retrospective"
created: 2026-05-11
sprint: "sprint-1-fidelity-followthrough"
result: PASS_WITH_NOTES
---

# Sprint 1 Retrospective

## What shipped

| Story | Outcome | Evidence |
|---|---|---|
| A — directive telemetry dashboard tile | DONE | `ui/src/components/hu-directive-telemetry-tile.{ts,test.ts}` (7/7 vitest), `ui/src/views/metrics-view.ts` wiring, `ui/src/demo-gateway.ts` mock |
| B — orchestrator canonical AB path | DONE | `scripts/lora-runner-ab.sh` (5/5 driver ACs, shellcheck clean), regex bug fixed |
| C — Tier-1 channel overlays | DONE | `include/human/onboard.h` + `src/onboard.c` + `src/cli_commands.c` centralization, `tests/test_persona_directive_channels.c` 6/6 PASS |
| D — Live LoRA evaluation | DESCOPE_OK | `sprints/sprint-1/evidence/D/` — Category B blocker, 6 independent gating issues documented |

Final sprint binary: `4eb4887e` on branch `sprint-1-fidelity-followthrough`.

## What worked

1. **Spec-first discipline.** Each story had a tech-lead-authored design BEFORE implementation. Story C's design caught two latent bugs in the production starter persona that the implementer would otherwise have papered over.
2. **Story D's built-in escape hatch.** AC-D.1 was authored as `OK | DESCOPE_OK | FAIL`, letting the tech lead descope honestly with rationale evidence rather than forcing a fake "done" or a sprint slip.
3. **Critic-before-audit.** The critic pass caught the BSD-grep regex bug in Story B and the aria-label format gap in Story A — both got fixed before sprint close. Without that pass we'd have shipped Story B in a state where the publish block was *literally unreachable* on macOS in production.
4. **Sprint-auditor independence.** The auditor's FAIL verdict (mid-sprint) caught that the working tree had been wiped by concurrent agents. Without that, we would have closed the sprint thinking the work was durable when in fact most of it was reverted.

## What broke

1. **Workspace contention with concurrent agents.** Multiple parallel Claude Code agents were running on the same workspace, on the same branch (`feat/sota-m1-infra`), doing aggressive rebases / `git reset --hard HEAD` operations that wiped Sprint 1's uncommitted work mid-flight. The reflog shows two `reset: moving to HEAD` operations 10 minutes into the implementer wave; concurrent commits `02f9d546` (W9 ToM) and `f1124d09` (W7 daemon) landed during our quality-gate phase and broke the build for our verification step.
2. **Implementers' "done" reports were not durable.** Each implementer reported PASS based on the working-tree state at the time of their run — but that state was not committed before the next implementer ran, so a later implementer's `git reset` wiped earlier work. The sprint protocol does not currently require commit-before-handoff between implementers.
3. **Story B's pre-existing regex bug masked by the implementer's hermetic test driver.** The Story B implementer worked AROUND the BSD-grep bug in `empty_response_set` by appending a non-standard ` X ,]` trailer to the mock output, rather than fixing the bug. The test driver passed but the production path was still dead. The critic caught it; we fixed it as part of close.
4. **Pre-existing build instability outside Sprint 1.** `src/agent/agent_stream.c` is in a half-refactored state (concurrent W9 work) — at one point during this sprint, it failed to build at all, blocking the targeted-suite verification path even though Sprint 1 didn't touch the file. The instability resolved on its own (the W9 author finished their refactor mid-sprint), but the dependency was real.
5. **`#ifdef HU_IS_TEST` discipline drift.** The original starter persona blob lived inside `#else` of `#ifdef HU_IS_TEST`, so the symbol wasn't compiled in test builds — invisible until Story C's test tried to link against it. Hoisting the literal above the guard fixed it. There may be other shared-data symbols with the same problem.

## What to change next sprint

1. **Implementers commit before handoff.** Each implementer's last action should be `git add` + `git commit` to a sprint-isolated branch, not just leaving the working tree dirty. Add this to the SCRUM specialist prompts.
2. **Sprint runs on a dedicated branch from day one.** Don't run a sprint on `feat/sota-m1-infra` while RL Phase-1 work is also active there. Create `sprint-N-*` worktree at sprint planning, work there, merge at sprint close.
3. **Critic pass lands BEFORE the auditor.** The current ordering (verify each → critic → audit → retro) is right; the failure mode here was that we ran the critic too late (after parallel agent activity had already corrupted the tree). Tighten the loop: critic immediately after each story closes, not at sprint end.
4. **Track "test-time symbol availability" as a recurring failure mode.** Add a lint or test-harness check that catches `extern const char ...[]` declarations whose definition lives behind `#ifdef HU_IS_TEST` `#else` (so it compiles only in production). The hoist-above-guard pattern should be documented as the canonical fix.
5. **Story D follow-up: schema mismatch is ticket-worthy.** AC-D.4 specified a JSON schema (`delta`, `baseline_score`, `candidate_score`, `run_id`) that doesn't match what `human ml fidelity-status` actually emits (`.ab.delta`, `.baseline.mean`, no `run_id`). Either the AC or the writer needs to be reshaped — captured in the descope rationale, but should become a backlog item.
6. **Story B follow-up: orchestrator success-path test against a real provider.** The hermetic driver only tests the publish block via a mock wrapper; it never exercised an end-to-end run with a live provider. Add this when Phase-1 RL lands a working chat path.

## Key metrics

- **Stories shipped:** 3 DONE + 1 DESCOPE_OK (out of 4 planned)
- **ACs delivered:** 24 of 24 in-scope (Story D's D.3, D.4 are N/A under DESCOPE_OK)
- **Sprint commits to durable branch:** 4 (`2c7fd988`, `29c88d4e`, `291a2844`, `4eb4887e`)
- **Tests added:** 1 C suite (6 tests) + 1 vitest file (7 tests) = 13 net new tests
- **Lines of net production code added:** ~600 (component + test + onboard hoist + script fix)
- **Times the working tree was wiped by concurrent agents during the sprint:** at least 2
- **Bugs caught only because of the critic pass:** 2 (B regex, A aria-label format)
- **Bugs caught only because of the auditor pass:** 1 (working tree had been wiped — entire sprint was at risk of false "DONE" at close)

## Acknowledgments

The stash auditor and critic are the load-bearing quality gates this sprint. Without their independent re-derivation, we'd have closed Sprint 1 with all-green status reports while the actual working tree was 50 % reverted.
