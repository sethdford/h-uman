---
title: "Sprint 2c — Retrospective follow-ups (Sprint 1 + 2a + 2b)"
created: 2026-05-12
status: closed
sprint: 2c
branch: sprint-2c-followups
working_directory: /Users/sethford/Documents/h-uman
---

# Sprint 2c — Retrospective follow-ups

## Sprint goal

Land the five "What to change next sprint" action items from the Sprint 1, 2a, and 2b retrospectives so the SCRUM protocol and tooling improvements actually take effect, instead of accumulating in retro docs.

## Stories

### Story F1 — Refresh CLAUDE.md M1/M2 to match shipped capability

Sprint 2a + 2b retros both flagged that CLAUDE.md mission rows had drifted from shipped capability.

- **AC-F1.1**: M1 row updates to reflect Sprint 2b Story A' (Tier-1 example banks shipped in `hu_starter_persona_json` for telegram / discord / imessage / slack, 12 examples, pinned by two new tests).
- **AC-F1.2**: M2 row updates to reflect Sprint 2b's typed propositional/prescriptive fact extraction via `hu_fact_extract` with confidence + provenance + trust tier + 90-day half-life decay.
- **AC-F1.3**: Competitive table "Personalization" cell updated to match.

**Done:** commit `05e72157` — 1 file, +3/-3.

### Story F2 — `scripts/sprint-status.sh`

Sprint 2a retro: "Read sprints/ for in-flight work BEFORE planning. Consider a script that prints all open sprints + dirty paths in one shot." Sprint 2b retro reiterated.

- **AC-F2.1**: Script prints all `sprints/sprint-*` directories with status frontmatter, artifact presence (review/retro/audit), and branch hint.
- **AC-F2.2**: Script reports dirty-file counts and warns when >5 dirty files (worktree threshold).
- **AC-F2.3**: Script lists all `refs/heads/sprint-*` branches with last-commit subject so a hijacked branch is visible.
- **AC-F2.4**: Script lists active worktrees and recommends worktree-vs-branch based on activity level.
- **AC-F2.5**: Plain text, no side effects, `shellcheck` clean.

**Done:** commit `f7ff3644` — first run on this branch correctly flagged that the sprint-2b-personal-model-honesty branch tip got hijacked by a concurrent agent.

### Story F3 — Strip comments in variant scanner

Sprint 2b retro: the negative test found that `check-memory-query-variant.sh` matched `.variant =` substrings inside comments, allowing a future regression to silently slip past the gate.

- **AC-F3.1**: Strip `/* ... */` and `// ...` from C source text BEFORE regex matching, replacing with same-length whitespace so line numbers stay accurate.
- **AC-F3.2**: Extend Sprint 2b Story D negative test with a comment-defeat fixture exercising both block and line comments.
- **AC-F3.3**: Negative test goes from 4/4 → 6/6 PASS.

**Done:** commit `66fef72d` — 3 files, +57/-2; logs in `sprints/sprint-2b/evidence/D/run-log.txt`.

### Story F4 — SCRUM Phase 0 prefers worktrees under high concurrent activity

Sprint 2a + 2b retros: branch isolation alone isn't enough when concurrent agents rewrite git refs. Three real failure modes are now on record (Sprint 1 wiped 2x; Sprint 2b's branch tip hijacked; Sprint 2b workspace had 18 dirty files).

- **AC-F4.1**: `~/.claude/skills/scrum/SKILL.md` Phase 0 renamed "Branch / worktree isolation"; adds decision matrix (>5 dirty files OR >1 sprint-* branch with recent commits OR >1 active worktree → worktree).
- **AC-F4.2**: `~/.claude/agents/scrum-master.md` Phase 0 mirrors the matrix and trigger language.
- **AC-F4.3**: Both rules require running `scripts/sprint-status.sh` (Story F2) FIRST to assess activity.
- **AC-F4.4**: Sprint plan.md must record both branch name AND working-directory path so audit can verify the sprint stayed in its lane.
- **AC-F4.5**: Evidence file in repo since the changes live outside it.

**Done:** commit `2289bc9f` — `sprints/sprint-2c/evidence/F4/worktree-rule.md` + rule files.

### Story F5 — Lint for test-time-invisible extern symbols

Sprint 1 Story C was blocked ~30 min by `hu_starter_persona_json` declared `extern` in `include/human/onboard.h` but defined inside `#ifdef HU_IS_TEST` `#else` — invisible to the test-build linker. Sprint 1 retro: track this as a recurring failure mode.

- **AC-F5.1**: `scripts/check-test-time-symbol-availability.sh` finds `extern <type> NAME[...];` in `include/**/*.h`, locates definitions in `src/**/*.c`, walks the active preprocessor guard stack at each definition site.
- **AC-F5.2**: Flags definitions inside the four shapes that omit the symbol when `HU_IS_TEST` is defined: `#ifdef HU_IS_TEST … #else <DEF>`, `#ifndef HU_IS_TEST <DEF>`, `#if !defined(HU_IS_TEST) <DEF>`. Variants that mention `_HU_TEST` covered too.
- **AC-F5.3**: Doesn't false-positive on (a) `tests/`-local headers, (b) platform guards (`_WIN32`), (c) extern function declarations.
- **AC-F5.4**: Negative test pins all four positive scenarios + four negative scenarios. 9/9 PASS.
- **AC-F5.5**: Wired into `scripts/verify-all.sh`.

**Done:** commit `45afed8a` — `scripts/check-test-time-symbol-availability.sh`, `sprints/sprint-2c/evidence/F5/negative-test.sh`, `sprints/sprint-2c/evidence/F5/run-log.txt`, `scripts/verify-all.sh` wire-in.

## Out of scope (deferred)

Nothing outstanding from Sprint 1/2a/2b retros remains — all "What to change next sprint" items are addressed.

## Process notes

This sprint dogfooded the new SCRUM rules from Sprint 2a:
- Dedicated branch `sprint-2c-followups` from sprint-2b tip.
- Implementer commits before each story handoff (every story has its own commit and evidence file).

It also ran into the exact concurrent-agent failure modes the new rules are designed to handle:
- A concurrent agent ran `git stash` on the working tree mid-sprint, taking `scripts/sprint-status.sh` with it.
- Same agent merged W9 cells onto `sprint-2c-followups` after F2, adding a divergence.
- Same agent later switched the working-tree branch from `sprint-2c-followups` to `sprint-2b-personal-model-honesty` mid-build.

Recovery: restored `sprint-status.sh` from its commit, reset `sprint-2c-followups` back to F2's tip, cherry-picked F3/F4/F5 onto a clean linear history. The sprint-2c-followups branch now contains F1→F2→F3→F4→F5 with no concurrent agent commits interleaved.

This is exactly the scenario Story F4 captures: when concurrent activity is this high, future sprints should use a separate worktree, not just a branch. Sprint 2c was caught half-converted (branch only, no worktree) and paid the recovery cost; the rule now mandates the worktree.
