# Commit 96968daf — historical annotation

**Date:** 2026-05-12
**Scope:** Non-destructive correction of a misleading commit title
**Why not rebase:** Concurrent agentic automation was actively touching adjacent branches at
the time of discovery. An interactive rebase would have created merge conflicts on at
least four downstream branches (`sprint-2c-followups`, `audit-fixes-2026-05-12`,
`land-w9-and-storyA-onto-sprint4`, `rl-sota-phase-3`) and could have orphaned in-flight
work. This file is the audit-trail substitute for a history rewrite.

## What the commit says it is

```
commit 96968dafad2570f129a798c47bba2ce60af616d8
Author: Seth Ford <sethford@studio.local>
Date:   Tue May 12 08:34:16 2026 -0400

    docs(sprint-3): update stories A-D with implementation status

    Stories A (HMAC) and C (secure-zero) implemented; B (default-deny) and
    D (JSON bounds) verified as pre-existing. Story E (SIGABRT) in progress.
```

## What the commit actually contains

24 files, **+4,334 / -320 lines** — far beyond a docs-only sprint update. The actual
contents are at least four independent logical bundles plus the original sprint-3 doc
update:

### Bundle 1: SOTA-2026 init-04 (MLX Qwen3 provider) — entire deliverable

- `include/human/providers/mlx_qwen3.h` (+133)
- `src/providers/mlx_qwen3.c` (+547)
- `src/providers/factory.c` (+19) — `"mlx_qwen3"` key dispatch
- `tests/test_mlx_qwen3_provider.c` (+374)

This is the full S1 #04 implementation that should have been a single
`feat(providers,mlx_qwen3): ...` commit with a co-author trailer to the implementer
subagent. Reviewer impact: 1,073 lines of new C code hidden under a docs title.

### Bundle 2: SOTA-2026 init-14 (Public benchmark suite) — entire deliverable

- `include/human/eval_public_suites.h` (+142)
- `src/eval_public_suites.c` (+332)
- `include/human/eval_benchmarks.h` (+8) — 5 new enum values
- `src/eval_benchmarks.c` (+20) — switch arms for the new enum values
- `src/cli_commands.c` (+100) — `human eval public-benchmark <name>` subcommand
- `tests/test_eval_public_suites.c` (+235) — 19 regression tests
- `tests/fixtures/benchmarks/longmemeval/smoke.json` (+48)
- `tests/fixtures/benchmarks/locomo/smoke.json` (+48)
- `tests/fixtures/benchmarks/knowu/smoke.json` (+48)
- `tests/fixtures/benchmarks/empa/smoke.json` (+49)
- `tests/fixtures/benchmarks/proagentbench/smoke.json` (+48)
- `docs/benchmarks/README.md` (+121)

This is the full S1 #14 implementation that should have been a single
`feat(eval,public-benchmarks): ...` commit.

### Bundle 3: RL Phase 3 plan doc (orthogonal)

- `docs/plans/2026-05-11-rl-loop-phase-3-kto-rm.md` (+1,818)

This is an entire 1,818-line forward plan for RL Phase 3 (KTO + reward-model with value
head). It has nothing to do with sprint-3 stories A–D. Should have been its own
`docs(plans): RL Phase 3 plan ...` commit.

### Bundle 4: Sprint-3 story updates (the title's actual scope)

- `sprints/sprint-3/stories.md` (+26 / -X)
- Test-file edits that pair with sprint-3 work:
  - `src/persona/examples.c` (+10 / -X)
  - `tests/test_channel_monitor.c` (+125 / -X)
  - `tests/test_context_engine_rag.c` (+91 / -X)
  - `tests/test_doctor_fix.c` (+105 / -X)
  - `tests/test_plugin_discovery.c` (+80 / -X)
  - `tests/test_skill_scaffold.c` (+127 / -X)

This is the sub-bundle the title actually describes.

## Total damage

- Reviewer load: a reviewer who opens this commit expecting a sprint-3 doc nudge
  must instead audit 1,073 lines of MLX provider code, 1,051 lines of benchmark
  infrastructure, and an 1,818-line forward plan — all under a misleading title.
- Bisection: a future `git bisect` looking for the regression-introducing commit for
  init-04 or init-14 would see this commit's docs title and dismiss it.
- Co-authorship: the subagent that produced bundles 1 and 2 is not credited; the
  human author appears to own 4,334 lines of code that they did not write.

## What we did instead of rebase

1. Logged the contents above for the audit trail.
2. Confirmed via `git branch -r --contains 96968daf` that the commit had not yet been
   pushed to `origin` at the time of audit (2026-05-12 09:25 UTC-4) — so a future
   force-push of a split version remains technically possible if the user explicitly
   approves history rewrite.
3. Left a `CHANGELOG.md` note (forthcoming via the S1.5 audit verdict) cross-referencing
   this annotation.

## Recommended remediation

If the user wants to repair the history before public release:

```bash
# AFTER backing up: git tag pre-96968daf-split 96968daf
git checkout sprint-2c-followups
git reset --soft 96968daf~1
# Stage and commit each bundle separately:
#   feat(providers,mlx_qwen3): on-device frontier provider (S1 init-04)
#   feat(eval,public-benchmarks): LongMemEval/LoCoMo/KnowU/EMPA/ProAgentBench (S1 init-14)
#   docs(plans): RL Phase 3 — KTO + reward model with value head
#   docs(sprint-3): update stories A-D with implementation status
git push --force-with-lease origin sprint-2c-followups
```

Do **not** do this without explicit user approval — it requires a force push and
would invalidate any reviewer's local checkout of sprint-2c-followups.

## Provenance

This annotation was authored as part of the **SOTA-2026 S1.5 follow-up (d)** in
`docs/plans/2026-05-11-sota-2026-massive-team-program.md` §Status. It is the
non-destructive substitute for the history rewrite that follow-up (d) originally
proposed.
