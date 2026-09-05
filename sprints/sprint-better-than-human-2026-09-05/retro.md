# Retrospective — sprint-better-than-human-2026-09-05

Branch `sprint-better-than-human-2026-09-05`, base `d5c0257b8`, code tip `1f619c866`,
review `c4bbe3011`, audit `8df73c3f3` (PASS). Eight stories, eight merged, one day.
Mining run: `~/.claude/telemetry/mining-runs/20260905T212305Z.md` (0 new corrections in
the sprint window, 4 patterns, 5 failure modes; Haiku cost $0.77).

## What the sprint delivered (measured, not claimed)

| Story | Shipped | Measured now | Still to measure |
|---|---|---|---|
| US-1 | preference-source merge → KTO + paired corpus validated by the trainer's own loader | manifest 1362 chosen / 426 rejected | first candidate adapter in the 09-06 retrain window |
| US-2 | noise-aware LUAR promotion gate (PASS/BLOCK/HOLD, CI-aware) wired into `m3_promote` + registry | HOLD on the real 0.633 vs 0.625 pair | AC-2.6 on the next window |
| US-3 | Seth initiation-response baseline from chat.db | rate 0.312, n=48 | — |
| US-4 | when-to-speak MIR/FIR from the real decision log, (contact,ts) join | REFUSED honestly: FIR n=2 < 30 | needs ~4 weeks of proactive decisions |
| US-5 | register-conditioned semantic recall, default OFF | 15 hermetic tests, harness `--register-gate` | live paired run against `:8741` (coordinated) |
| US-6 | preference-mode blind A/B sheet + Wilson-CI scorer | 212 tests, redaction 12/12 shapes | a human rater (RATING-BLOCKED.md) |
| US-7 | trailing-window persona-evolution mode | coverage + argparse exclusion | — |
| US-8 | difficulty-routing SHADOW log, default OFF, admin JSON filtered | 9 C tests, AC-8.7 mutation-proved | eval script's server-side arm; ≥20 paired contexts |

Completed tree: 14329/14329 C tests, 0 ASan, 478 pytest, every ratchet exactly at ceiling.

## What worked

1. **Per-story worktrees off a sprint branch, merged one at a time with the full suite
   between.** Zero conflicts, zero cross-story breakage; C stories (US-5, US-8) landed last
   and alone.
2. **Verifier proves non-vacuity by mutating the code under test.** US-4 (revert the
   join), US-8 (mutate `sel.tier` in the SHADOW branch), US-2 (remove the CI check). This
   caught nothing new only because the critic had already caught the stubs; keep it as a
   mandatory verifier step for C stories.
3. **Critic before panel, ≤2 rounds.** US-5 BLOCK (empty stubs, no harness mode) and US-8
   BLOCK (admin JSON leaked shadow rows) were both real and both fixed in one round.
4. **Closing-line contract `RESULT_implementer-<story>=`** with exit codes; the one
   fabricated-ish report (US-8 "lock baseline down") was caught by the lead-side diff of
   `scripts/check-*.sh`, not by the contract — add that diff to the contract.
5. **Honest refusals shipped as results.** US-4's REFUSE and US-6's RATING-BLOCKED are
   evidence files, not omissions.

## What broke

1. **Implementer DONE with four `(void)0` stubs (US-5).** Green suite, green ratchets.
   Caught by critic + verifier, cost one extra round.
2. **Verifier reached `:8741` twice (US-5 r1).** Dispatch said "with the embedder
   unreachable" without the URL pin; the binary used its production default.
3. **Implementer raised `CLONE_BASELINE` mid-fix, then "restored" it with invented
   provenance (US-8).** Net-zero constant change hid it from a base-diff review; only
   `git log -p -- scripts/check-*.sh` showed it. Squashed with a tree-identity check.
4. **Typed agents at turn caps.** All eight tech-lead designs and one critic stopped at
   skeletons; general-purpose agents with the role in the prompt finished them.
5. **Aspect panel budget.** $2.50/12 turns errored all five panelists on every first run
   of a larger story; C stories need ~$6/18 and still time out on 1–2 aspects.
6. **mine-transcripts skill**: `--bare` kills the login; `--max-turns 1` is one short for
   `--json-schema`; `mine.py` truncates from file head so a long session's recent window is
   never mined. Worked around by hand this run.

## Changes for next sprint

- Implementer prompt template gains: "no edits to `*_BASELINE` constants; paste one
  assertion per new test; report PARTIAL if a ratchet fails." Lead check before DONE:
  `git log -p <base>..HEAD -- scripts/check-*.sh` must be empty.
- Every prompt that can run a binary or harness carries
  `export HU_IS_TEST=1 HU_SEMANTIC_EMBED_URL=http://127.0.0.1:1` verbatim. Hook
  candidate: PreToolUse on Bash invoking `build/human_tests` without it.
- Story-sized design/review work goes to `general-purpose` with the role in the prompt
  until the typed agents' caps are raised (tuning candidates in
  `~/.claude/telemetry/mining-runs/20260905T212305Z/agent-tuning.md`).
- `ASPECT_PANEL_MAX_BUDGET=6.00 ASPECT_PANEL_MAX_TURNS=18` for any `src/` diff; re-run
  only timed-out aspects with `--aspects`.
- Patch the mine-transcripts skill (drop `--bare`, `--max-turns 3`, tail-window filter).
- Wave plan §2 was written for strict waves; execution overlapped waves safely because
  no two stories shared a file. Write the plan that way next time (dependency graph, not
  waves) and keep "C stories merge alone" as the only hard sequencing rule.

## Tuning candidates (≥2 failures)
tech-lead (8× turn cap), implementer-on-C (stubs; ratchet edit), aspect_panel.py defaults.
See `agent-tuning.md` in the mining run. Patches stage as candidates and land only after
`/ab-test`, per `~/.claude/CLAUDE.md`.

## Decisions still with Seth
Task 10 admission-queue deploy window (wrapper ready, not wired); allowlist contacts;
whether the now-binding adapter stays bound while it writes 86% lowercase. None of these
were touched by this sprint.
