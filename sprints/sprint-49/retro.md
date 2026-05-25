# Sprint 49 Retrospective

**Sprint:** 49 — Distribution / macOS installable artifacts (C1)
**Branch:** sprint-49-distribution
**Closed:** 2026-05-24
**Outcome:** 7/7 stories closed · 36/36 ACs delivered · auditor PASS · 11,842/11,842 tests passing

## What worked

1. **Worktree-per-implementer for Wave 2.** Five orthogonal stories ran concurrently in five separate worktrees off `sprint-49-distribution`. Merge-back conflicts were minimal because each story owned a distinct directory (`Formula/`, `.github/workflows/`, `scripts/release/sign-and-notarize.sh` etc.). The only real conflicts (test_main.c + CMakeLists.txt + stub-script collisions) resolved in <5 minutes each.

2. **Per-story critic immediately after verifier (not batched).** Caught 2 CRITICAL + 1 HIGH + 2 MEDIUM findings BEFORE the next story built on top:
   - US-C1.1 verify_bundle_dependencies shell-logic bug (`||` swallowing exit code)
   - US-C1.1 silent-pass test (binary-presence assertion gated on existence — tests-that-pin-bugs antipattern)
   - US-C1.2 launchd tilde expansion silent log loss (would have shipped to production with operators unable to see daemon logs)
   - US-C1.2 sed unescaped `$VERSION` corruption on special chars
   - US-C1.2 tests grep-not-execute (same tests-that-pin-bugs antipattern as US-C1.1 — recurrence flagged)

3. **Quality-gate discipline held under iteration pressure.** US-C1.1 needed 3 rounds (initial impl → test path fix → critic finding fix). Verifier+critic re-runs caught real bugs each round. The protocol's "FAIL re-opens, don't advance" rule prevented closure on broken work.

4. **Sprint-auditor caught what was missed at story level.** Auditor independently verified:
   - US-C1.3a cherry-pick brought the FULL 154-line implementation + 5 fixture JSONs (not the 47-line US-C1.5 stub)
   - US-C1.5 stub-vs-real script conflict resolved correctly (US-C1.3's 275-line `sign-and-notarize.sh` wins, US-C1.5's stub discarded)
   - No cross-story regressions despite 11 commits in Wave 2

## What broke

1. **Aspect-panel infrastructure misfired** (CRITICAL for protocol completeness). `python3 ~/.claude/rl/aspect_panel.py --target <text>` returned INCONCLUSIVE on both attempts for US-C1.1. All 5 sub-`claude -p` verifiers returned `verdict="unknown" confidence=0.5 rationale="" elapsed_s=3.5-9.6` — meaning sub-processes launched but never produced the `RESULT_<aspect>-verifier=PASS|FAIL` lines the aggregator expects. **Workaround used:** rely on verifier + critic gates (which DID catch real bugs); aspect-panel skipped for US-C1.2+ to avoid burning timer. **Root cause unknown** — sandbox? PATH? prompt template length? Needs separate debug session before next high-stakes sprint.

2. **Critic / verifier / sprint-auditor agents systematically trail off without RESULT line** (HIGH for retro). On EVERY invocation in this sprint (verifier 2×, critic 4×, sprint-auditor 1×), the specialist agent returned mid-investigation prose without the protocol's required `RESULT_<role>=...` line. SendMessage nudge always recovered, but adds wall-clock. **Workaround discovered mid-sprint:** prompt them with "write the single-line verdict at the TOP of your response, then justify below." This worked first-try when applied (US-C1.2 critic re-review, sprint-auditor after nudge). **Recommend tune-agent for all three specialist roles to bake "verdict-first" formatting into their prompt templates.**

3. **tests-that-pin-bugs antipattern recurred twice** (MEDIUM for code quality). US-C1.1 initial implementer wrote `test_daemon_binary_present_and_executable` that silently passed when binary absent. US-C1.2 implementer wrote 15 tests that greped a shell script for tokens instead of executing it. Both caught by critic. **The pattern is documented at `.claude/rules/tests-that-pin-bugs.md` — implementers don't read project rules before writing tests.** Recommend pre-commit hook that lints for "if (file_exists(X)) { HU_ASSERT(...) }" pattern in test files.

4. **US-C1.3a implementer committed to the WRONG BRANCH** (`claude/strange-lamport-3d7b29` instead of `sprint-49-impl-us-c1-3a`). The scrum protocol's mandatory `git log <sprint-branch> ^<base>` check caught it during the merge attempt ("Already up to date" when it shouldn't have been). Recovered via cherry-pick. **Root cause:** the agent's worktree was set up by the spawner to be the implementer's workspace, but the agent ran `git commit` from a path that resolved to the spawning session's worktree (maybe via `cd` into a different absolute path, or HOME-relative confusion). **Recommend implementer-agent prompt template explicitly says: "Run `pwd && git rev-parse --abbrev-ref HEAD` BEFORE every commit and verify they match the expected sprint worktree + branch."**

5. **CMake reported success while production binary was stale** (LOW, mostly avoided). Cross-implementer `touch <src> && cmake --build` was needed multiple times after merges to force linker pickup. Documented at `.claude/rules/cmake-build-stale-binary.md`. Now well-internalized.

## What to change next sprint

| Action | Owner | Target |
|---|---|---|
| Tune critic agent for "verdict-first" formatting | `/tune-agent critic` | Sprint 50 |
| Tune verifier agent for "verdict-first" | `/tune-agent verifier` | Sprint 50 |
| Tune sprint-auditor for "verdict-first" | `/tune-agent sprint-auditor` | Sprint 50 |
| Debug aspect-panel infrastructure (sub-`claude -p` calls not producing RESULT lines) | Manual investigation | Before next high-stakes sprint |
| Add implementer prompt template: "verify pwd + branch before commit" | `/tune-agent general-purpose` (?) | Sprint 50 |
| Consider pre-commit hook for tests-that-pin-bugs pattern | `/hookify` | Sprint 50 background |

## Notes for /tune-agent

Recommend invocation order based on highest-impact-per-fix:

1. **critic (highest)** — 4 invocations in this sprint, all needed nudge; "verdict-first" format works deterministically. Patch: add explicit "Output ONE single line `RESULT_critic=...` as the FIRST line of your response, BEFORE any prose investigation. Then justify below." to the critic agent's `.md` prompt.

2. **verifier** — 2 invocations, both trailed off in different ways. Same patch shape as critic.

3. **sprint-auditor** — 1 invocation, same pattern. Same patch.

## Sprint stats

- **Duration:** ~4 hours wall-clock (with parallel agents)
- **Implementer rounds:** 7 stories × 1-3 iterations = 12 implementer dispatches
- **Quality-gate dispatches:** 4 critic, 2 verifier, 1 sprint-auditor (+ 5 nudge SendMessages)
- **Real bugs caught + fixed BEFORE merge:** 5 (2 CRITICAL silent-failure, 1 HIGH shell escaping, 2 MEDIUM antipattern)
- **Bugs found post-merge:** 0 (sprint-auditor PASS without findings)
- **Wave 2 merge conflicts:** 3 (test_main.c registrations × 2, sign-and-notarize.sh stub-vs-real × 1)
- **Total commits:** 16 on sprint-49-distribution

The discipline paid for itself. The 5 bugs caught at story-level would have shipped without the per-story critic gate.
