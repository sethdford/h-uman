---
title: Human-level cut-over kit — one command, go/no-go per gate
created: 2026-10-02
status: operator-facing
---

# Human-level cut-over kit

One command measures the whole new humanness stack against production on
Seth's real held-out conversations. It writes a report with a PROMOTE or HOLD
verdict for each gate, plus the PlistBuddy commands that would set the
PROMOTE gates live. **The kit never runs those commands.**

```bash
scripts/cutover/run_cutover.sh --estimate-only   # gates, arms, runtime; no work
scripts/cutover/run_cutover.sh                   # the real run (~4h with 7 gates)
scripts/cutover/run_cutover.sh --dry-run         # the whole pipeline on fakes
```

It builds on the real-turn replay harness (`docs/guides/replay-harness.md`,
PR #594) and the memory probes (`docs/guides/memory-benchmarks.md`, PR #593).
Until those merge, point the kit at their worktrees:
`HU_CUTOVER_HARNESS_DIR=<replay-harness>/scripts/blind_ab` and
`HU_CUTOVER_MEMORY_DIR=<memory-benchmarks>/scripts/datasets`.

## What it does

| Step | What runs | Where the data goes |
|---|---|---|
| 0 | Gate detection: the candidate gate names found in `build/human`'s strings. `human replay` has no `--list-gates`. | stdout |
| 0 | Runtime estimate, printed before any work starts | stdout |
| 1 | `replay_export_turns.py`: 60 held-out 1:1 turns from the last 30 days. chat.db is opened read-only, and turns the daemon answered are dropped. A snapshot of `~/.human` is taken read-only. 50 LoCoMo probes (`--per-category 10`). | `~/blind_ab_run/cutover-<date>/`, 0700/0600 |
| 2 | `replay_driver.py run`, strictly sequential: every arm, every turn, `--delay-ms 3000`, `--temperature 0`, production's plist env as the base. The probes go through arms A and B (`--memory-all-arms` runs them through every arm). | `out/<arm>.jsonl`, `<name>-mem/out/` |
| 3 | `replay_feed.py --sheets`: one blind 2AFC sheet per arm | `feed/<arm>/` |
| 4 | `judge_local.py`: Gemma 4 26B on Ollama (`gemma4-26b-mmap`), loopback only, `reasoning_effort=none`, 2 s between calls | `feed/<arm>/judged.csv` |
| 5 | `cutover_report.py`: CIs, ablation deltas, verdicts, commands | `report.md`, `report.json` |

The arms are:

- **A**: every candidate gate in the binary is set `off`.
- **B**: every candidate gate is set `live`.
- **B-no-\<gate\>**: B with that one gate set `off`.

Candidate gates: `HU_THREAD_CONTEXT`, `HU_HISTORY_BUDGET`,
`HU_IMMERSIVE_CONTEXT`, `HU_LENGTH_POLICY`, `HU_LEARNED_STYLE`,
`HU_DIRECTOR_V2`, `HU_CONTEXT_RELEVANCE`. A gate that is not in the binary is
listed as skipped.

Two gates stay at production's value in every arm:

- `HU_GRIEF_DECAY` is not on the reactive reply path.
- `HU_POST_SEND_DEFER` is a latency change, not a humanness one.

The fragment/guard fixes (#592) and the reply-noise fixes (#595) have no gate.
They are in every arm, so they show up in absolute numbers, not in deltas.

## Safety

- **Generation reaches :8741 only through the harness.** The harness sandboxes
  `HOME` and the state dir, allows loopback only, sends on a null channel, and
  aborts if any outbound call is counted. Calls are strictly sequential and
  paced.
- **The judge is local.** Any non-loopback endpoint is refused, and so is
  `--api`. The preflight checks that Ollama serves the judge model before
  any work starts.
- **Real data stays private.** It lives only in the run dir, outside the repo,
  under `umask 077`. The kit prints only counts and aggregates.
- **No gate file is written.** The kit never calls `score.py` and never writes
  `~/.human/blind_ab_gate.json`. It checksums that file before and after the
  run and exits 3 if the file changed.
- **Nothing is executed on production.** The kit prints the PlistBuddy and
  `launchctl` lines and does not run them. A test checks that no code line in
  `run_cutover.sh` invokes either.

## Runtime estimate

`cutover_plan.py estimate` uses these costs:

- **18 s per turn-arm.** The harness runbook measured 11–19 s on :8741 at a
  3 s delay. The kit plans with the slow end.
- **22 s per probe-arm.** Probe prompts are longer.
- **6 s per judged item.** That is about 4 s of Gemma 4 26B plus the 2 s
  pacing.

With all 7 gates present there are 9 arms:

| Phase | Work | Time |
|---|---|---:|
| Generation | 540 turn-arms | 2h42m |
| Memory | 100 probe-arms | 37m |
| Judge | about 459 items | 46m |
| **Total** | | **about 4h05m** |

With only the gates merged today (`HU_LENGTH_POLICY`), there are 3 arms and
the run takes about 1h46m. `--estimate-only` prints the real figure for your
binary.

## Pre-registered decision rules

The rules are fixed in `scripts/cutover/cutover_report.py`'s header. If they
change after a run's numbers have been seen, that run's verdicts are void.

**Statistics.**

- Every CI is a 95% percentile bootstrap: 2000 resamples, fixed seed.
- Arm deltas are **paired**: the same resampled turns feed both arms.
- Lower is better for judge detection (0.50 = indistinguishable), length KS D
  vs Seth, fragment rate and deflection rate.
- Higher is better for memory accuracy.

**Deltas.** For gate g, X is the arm B-no-g:

- dDetect = detect(X) − detect(B). Positive means g helps.
- dKS = KS(X) − KS(B). Positive means g helps.
- dFrag = frag(B) − frag(X). Positive means g hurts.
- dDefl = defl(B) − defl(X). Positive means g hurts.

**Rules.**

| Rule | Condition |
|---|---|
| R1 benefit | dDetect's CI lies entirely above 0 (with ≥ 30 items judged in both arms), OR dKS's CI lies entirely above 0 (with ≥ 30 text replies in both arms). |
| R2 harm | Checked for dFrag and dDefl. A rate is worse when its CI lies entirely above 0, OR its point estimate is ≥ 0.10. |
| R3 inert | X and B sent byte-identical reply requests, with the same action and bubbles, on every turn. The gate never reached the reply path. |
| R4 stack | B vs A must not be worse on fragment rate, deflection rate or memory accuracy (R2's test). Probes skipped with `--skip-memory` are NOT EVALUATED and do not block. Probes that were attempted and are incomplete do block. |
| R5 complete | Every arm is COMPLETE in the driver's manifest and covers the same turns. |

**PROMOTE(g)** requires all of the following: R5 and R4 pass, g is not inert,
R1 holds, and neither R2 check fires. Everything else is HOLD, and the report
says why. Cut-over is **GO** when at least one gate is PROMOTE, and it covers
exactly those gates.

Tapback share, question rate and the median length ratio are reported but not
gated. The replay runs its director on the replay endpoint, not on
production's classifier.

## Reading the report

`report.md` has five parts:

- The verdict table.
- One row per arm with CIs. Seth's own numbers on the same turns come first.
- Memory accuracy by LoCoMo category.
- The ablation-delta table.
- The commands.

The commands set each PROMOTE gate to `live` in
`~/Library/LaunchAgents/ai.human.service-loop.plist` and reload the service.
The rollback restores each gate's value as it was when the run started, read
from the driver manifest's base env. A gate that was absent is deleted.
`human service install` regenerates the plist, so re-apply the commands after
a reinstall.

With 60 turns, a CI on a rate is about ±0.12 wide. A HOLD for "no benefit
outside the noise" means the measurement could not separate the arms. It does
not mean the gate hurts. The next step for such a gate is more turns, run with
`--turns`, not a looser rule.

## Dry run

`--dry-run` builds fixtures in a temp dir:

- a synthetic chat.db with eight contacts and 555 numbers;
- a state dir and a plist;
- ten LoCoMo-shaped probes.

It also starts a single fake loopback server that answers the director, the
replies and the judge. The pipeline then runs end to end with
`scripts/cutover/dryrun_fake_human.py`, a stub binary with three gates. Pass
`--human <build>/human` to use a real replay binary instead. The report flags
its thresholds as overridden, because the fixture is too small for the real
ones.

Tests:

```bash
python3 -m pytest -q tests/test_cutover_report.py tests/test_cutover_plan.py \
    tests/test_cutover_dryrun.py
```

`test_cutover_dryrun.py` skips until the harness and the probe scorer are
present. To include the real binary, set `HU_CUTOVER_TEST_BIN=<build>/human`.

## Rollback

The kit is offline tooling and changes nothing in the daemon. To roll back a
promotion, run the rollback block from the report.
