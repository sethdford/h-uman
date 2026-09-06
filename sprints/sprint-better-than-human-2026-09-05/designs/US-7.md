# Design for US-7: Windowed, re-derivable persona style card with honest event-shift reporting

**Status:** COMPLETE (verified against HEAD, 2026-09-05)
**Story:** US-7 (P2), `sprints/sprint-better-than-human-2026-09-05/stories.md:228-265`
**Estimate:** S (revised down from the skeleton's M — see "Scope note")

## Scope note — the ground has moved since the story was written (read first)

Two commits landed on this branch **the same day** the sprint stories were authored,
before this design started, and materially shrink US-7's remaining surface:

- `8fc8a022d` (AC-7.1, already excluded by the story) — style card is the single source.
- `5e00683f5`, 2026-09-05 06:16:30 — **not mentioned in stories.md or the skeleton** —
  already added `--min-covered-days` (default 20) to `eval_persona_evolution.py`'s refusal
  contract, already retired the move event as `INSUFFICIENT_DATA` (pre-window
  `covered_days=5.1 < 20`), and already added `scripts/eval_persona_evolution_gate.py`
  (+ `scripts/test_eval_persona_evolution_gate.py`, 10 hermetic tests), which implements
  spec.md §5's "directional-fidelity gate" end-to-end (VERIFIED:
  `docs/plans/2026-09-02-persona-evolution/gate-2026-09-05-post-prompt-fix.json`, status
  `FAIL`, real run, no `:8741` writes).

**What this means for US-7's remaining scope**, mapped to the ACs:

| AC | State before this design | Remaining work |
|---|---|---|
| 7.1 | Done (`8fc8a022d`) | none |
| 7.2 | **Not done** — see Finding 1 | new `--trailing-days` flag |
| 7.3 | **Not done** — see Finding 2 | `coverage` field for the trailing/full-range path |
| 7.4 | Contract exists (`min_n`, `min_covered_days`), but not wired to full-range/trailing | reuse verbatim in the new code path |
| 7.5 | Partially done — move is retired (§ "2026-09-05 follow-up" in spec.md), but the two events' `lowercase_start_rate` deltas are never stated side by side | append spec.md §8 (below) |
| 7.6 | Out of scope, unchanged | none |
| 7.7 | Already true of every existing `results-*.json` (verified: no message text in any of them) | keep true in new output |

The remaining work is genuinely small: one new CLI mode reusing existing functions, one
new report field, ~10 new hermetic tests, and a written comparison of two numbers that
already exist in committed JSON. Estimate revised **M → S**.

## Finding 1 (blocks the skeleton's literal approach): `--window-days` is already taken, and it is a no-op under `--full-range`

`--window-days` (default 30) already exists in `scripts/eval_persona_evolution.py:729`.
**VERIFIED** by `grep -n window_days scripts/eval_persona_evolution.py`: every use is
inside `bucket_by_window()` (event pre/post half-width, `eval_persona_evolution.py:379-387`)
or the report's echo field (`:620`). The `--full-range` block
(`eval_persona_evolution.py:691-704`) **never reads `args.window_days`** — it aggregates
over the entire `[--start, --end)` range (default `--start=2026-03-01`).

This is not academic: `docs/plans/2026-09-02-persona-evolution/spec.md:421` prescribes the
nightly command `--event none --full-range --window-days 30` as the "windowed" mode. Run
today, that command reports a `full_range_summary` covering **2026-03-01..2026-09-05**
(the whole default range), silently mislabeled by the adjacent `"window_days": 30` field —
exactly the shape `.claude/rules/reports-success-does-nothing.md` and
`no-number-without-a-measurement.md` warn about (a well-formed number from a computation
that didn't measure what its label claims). It has caused no damage yet because
**VERIFIED** no cron/launchd job invokes this script (`grep -rl eval_persona_evolution
--include=*.plist --include=*.sh .` outside the script/test pair itself: zero hits) — the
nightly log in spec.md §5 was designed but never scheduled.

**Resolution:** do not repurpose `--window-days` (it is load-bearing for `--event
move/job/both`, and 6 existing tests pin its pre/post semantics). Add a new,
non-colliding flag, **`--trailing-days N`**, that is the actual "windowed re-derivation"
mode AC-7.2 asks for. The AC's own text allows this ("A `--window-days N` mode is added
... or a thin wrapper") — this is the wrapper, implemented as one new `argparse` option
in the same file rather than a separate script, so it inherits `--source`, `--db`,
`--min-n`, `--min-covered-days`, `--n-resamples`, `--seed`, `--out` for free instead of
re-declaring them.

## Finding 2: `full_range_summary` has no `coverage` field at all

**VERIFIED** by reading `eval_persona_evolution.py:691-704`: `fr_entry` carries only `n`,
`status`, `reason`/`axes` — no `window_coverage()` call, unlike the pre/post windows
(`:657`, `:663`) which already report `{first, last, covered_days}`. This is the second
half of AC-7.3's gap: even a hypothetical windowed run today would not surface "how many
of the requested days actually have data" the way the event windows already do.
`window_coverage()` (`:509-520`) is already a pure function taking `(messages, lo, hi)` —
it needs no changes, only a new call site.

## Finding 3: the chat.db retention floor is a live, rolling number — never hardcode it

The skeleton's "~33 days" (from `2026-08-03`) is already stale. **VERIFIED live,
read-only, aggregate-only** (no message text touched, matching this script's own
contract), 2026-09-05 ~16:00 local:

```
min(is_from_me=1 date) = 2026-08-06T14:42:13 UTC
today                   = 2026-09-05T15:59:43 UTC
days available          = 30
```

`docs/plans/2026-09-02-persona-evolution/spec.md:251` independently recorded the floor at
`2026-08-05` two runs earlier the same day ("chat.db retention rolled two days between
runs"), and `sprints/sprint-better-than-human-2026-09-05/designs/US-3.md` (a sibling
design in this same sprint, same day) independently live-probed the identical floor
(`2026-08-06T14:42:13`) for a different story. Three independent measurements agree: the
floor advances roughly one day per day (a rolling "Keep Messages" window), not a fixed
date. **Consequence for the design:** the windowed mode must report its *own* observed
`covered_days` every run, never assume or hardcode a day count — a `--trailing-days 60`
run today reports `covered_days ≈ 30`, a run next week will report a different (larger,
if the nightly log in spec.md §5 is ever scheduled; otherwise still capped near the
rolling floor) number. This is exactly AC-7.3's requirement and exactly why Finding 2
must be fixed.

## Approach: `--trailing-days N`

```
python3 scripts/eval_persona_evolution.py --event none --trailing-days 60 \
    --source data/imessage/training_pairs.jsonl \
    --source ~/.human/logs/eval-archive/imessage-corpus-backup-20260725-113543/training_pairs.jsonl \
    --out sprints/sprint-better-than-human-2026-09-05/evidence/us7-trailing-60d.json
```

Semantics (all reusing existing code, no axis math reimplemented):

1. **New flag, mutually exclusive with a non-`none` `--event`.** If `--trailing-days N` is
   given and `args.event != "none"`, `main()` calls `p.error(...)` (argparse-level, exit
   2) with a message telling the user to pass `--event none`. This is a deliberate hard
   failure, not a silent override — the reader of a report should never wonder whether
   `--trailing-days` silently disabled event analysis they thought they asked for.
2. **Computes `trailing_start = end_dt - datetime.timedelta(days=N)`, overriding
   `--start`.** If the user also passed an explicit non-default `--start`, print one
   stderr note that `--trailing-days` takes precedence (still not silent).
3. **Reuses `fetch_outbound_messages`, `merge_sources`/`--source` handling,
   `export_frame_filter`, `aggregate_window` (which itself reuses `bootstrap_ci`)
   verbatim** — identical call shape to the existing `--full-range` block, just fed
   `trailing_start` instead of `args.start`.
4. **Adds the missing `coverage` field** (Finding 2): `window_coverage(messages,
   trailing_start, end_dt)`, same helper the pre/post windows already call.
5. **Refusal contract reuses `DEFAULT_MIN_N` (100) and `DEFAULT_MIN_COVERED_DAYS` (20)
   verbatim** (AC-7.4) — no new magic number. Refuses (exit 1, writes nothing) when
   `n < args.min_n` **or** `coverage.covered_days < args.min_covered_days`, in the same
   `INSUFFICIENT_DATA` shape the event windows already use, with a `reason` string naming
   which condition failed.
6. **Report shape** — a new top-level key alongside the existing `full_range_summary`
   shape (does not replace it; `--full-range` and `--trailing-days` remain independent,
   orthogonal flags — a caller can still ask for the all-time summary):
   ```json
   "trailing_window_summary": {
     "requested_days": 60,
     "window": {"start": "2026-07-07", "end": "2026-09-05"},
     "n": 1013,
     "coverage": {"first": "2026-08-06T14:42:13", "last": "2026-09-05T15:41:34", "covered_days": 30.0},
     "status": "OK",
     "axes": { "...": "same 9-axis shape as aggregate_window()" }
   }
   ```
   The `requested_days` vs `coverage.covered_days` split is the literal fix for AC-7.3:
   a `--trailing-days 60` run's `requested_days` field says 60, its `coverage.covered_days`
   says ~30 — the two are never conflated into one number.
7. **Does not touch `--full-range`'s existing behavior.** `full_range_summary` is
   unchanged (still no coverage field, still whole-range) to avoid breaking
   `test_run_full_range_reports_summary_independent_of_event_refusal` and
   `test_run_full_range_only_succeeds_when_event_analysis_skipped`, which pin its current
   shape. Fixing Finding 2 for `--full-range` itself is out of scope for this story
   (nobody schedules `--full-range` today; `--trailing-days` is the sanctioned windowed
   entry point going forward — see the spec.md correction below).

## Event-shift detection: what spec.md defines, and what is measurable now

Per `docs/plans/2026-09-02-persona-evolution/spec.md` §5, "directional fidelity" means:
for every axis whose human delta moves beyond its own window's 95% CI (`moved_beyond_ci`
in `delta_report()`, `eval_persona_evolution.py:390-410`), a matched-prompt
before/after generation pass must reproduce the *same sign* of delta. §5 also names the
prerequisite this depends on: 30 days of pre-event chat.db history, which chat.db itself
cannot supply (Finding 3) without the `--source` merge.

**Measurable now** (VERIFIED, `docs/plans/2026-09-02-persona-evolution/results-2026-09-05.json`
+ `gate-2026-09-05-post-prompt-fix.json`):

| Event | Windows | Status | Directional-fidelity gate |
|---|---|---|---|
| **job** (Vanguard → Raymond James, 2026-07-26) | pre n=1,324/30.0d, post n=522/29.5d | `OK` | Ran: `FAIL`. `length_chars` sign-matches (human −3.01, gen −20.41); `emoji_rate` and `warmth_hits_per_100_words` do NOT (human +0.039/+0.42, gen 0.0/−0.72). `lowercase_start_rate` excluded (device/autocapitalization artifact, spec §3b point 2 — 0.00→0.26→0.02→0.36 week-to-week inside one store). |
| **move** (St. Petersburg, 2026-07-01) | pre n=180/**5.1d** | `INSUFFICIENT_DATA` (`covered_days=5.1 < min_covered_days=20`) | Not run; not measurable on any data this repo currently holds. |

**Not measurable until history recovery (AC-7.6, out of scope):** the move event's
pre-window is bounded by the two static `--source` exports already inventoried in
spec.md §3b (1,303 + 59 rows); those files do not grow, so `covered_days=5.1` is a
permanent ceiling on *this* event from *this* data, not a transient gap the nightly log
(spec.md §5) can backfill retroactively. It becomes measurable only if a new,
previously-unknown Seth-authored export surfaces (a genuinely new data-recovery task,
filed separately per the closing report) — not something this story attempts.

## How the style card is consumed by the prompt (file:line, all VERIFIED by reading the source)

1. `src/persona/persona.c:117` — `hu_persona_build_absolute_rules_fmt()` calls
   `hu_style_card_resolve(pname, pname_len, &card)` for the casual (non-formal) register.
2. `src/persona/style_card.c:129-149` (`hu_style_card_resolve`) calls
   `hu_style_card_load_for_persona()` → `hu_file_slurp` on
   `~/.human/personas/<persona>.style-card.json` → `hu_style_card_parse()`
   (`style_card.c:64-101`, reads `axes.<name>.value` for `lowercase_start_rate`,
   `no_terminal_punct_rate`, `question_rate`, `exclamation_rate`, `emoji_rate`, plus `n`
   and `window.{start,end}`). On any read/parse failure, falls back to
   `hu_style_card_default()` (`style_card.c:32-43`, pinned to the 2026-09-03 measurement,
   n=977) and logs once (`hu_log_warn_once`, `style_card.c:142-148`) naming
   `measure_style_card.py` as the fix.
3. `src/persona/persona.c:119` — `hu_style_card_render_casual_rules()`
   (`style_card.c:161-182`) turns the five axes into the casual register's rule 2 prose
   ("a lowercase start is about 1 in N texts", etc.).
4. `hu_persona_build_absolute_rules_fmt` is the single source both the reactive
   (`agent_stream`) and proactive (`daemon_proactive`) send paths call (comment at
   `persona.c:44-46`), so this is the only place style-card numbers reach a live prompt.

**This story's `--trailing-days` output never reaches any of the above.** It writes only
to `--out` (an evidence file / nightly log path), never to
`~/.human/personas/<persona>.style-card.json`. That path is written exclusively by
`scripts/measure_style_card.py`, which this story does not modify (AC-7.1).

## How a re-derived card is validated before it replaces the live one

**Current state (VERIFIED by reading `measure_style_card.py:135-157`):** there is no
human-approval gate. `run()` refuses on `n < min_n` (default 300, a *different*,
stricter, and independently-chosen threshold from this story's `min_n=100` — deliberately
higher because this file drives every live turn, not just a report), then on success goes
straight from computing the card to `os.replace(tmp, out)` — an atomic file swap, but not
a *reviewed* one. There is also no scheduler invoking it (Finding 1's grep found zero
cron/launchd references), so today the only actor who can overwrite the live card is a
human running the script by hand and looking at its printed JSON before it's already
written (the print happens before the write, but nothing blocks the write pending review).

**This is a real gap, but it predates this story and is not one of its ACs** — AC-7.1
explicitly scopes US-7 to build on top of the single-source reconciliation, not extend
it, and none of AC-7.2–7.7 mention the live card's write path. Per
`.claude/rules/feature-gate-requires-measurement.md`, a promotion gate belongs on
anything that *changes what gets sent* — the style card qualifies (it shapes the casual
register's rule 2 on every casual-register turn) — so the missing gate is worth flagging
explicitly rather than silently leaving it unmentioned:

- **What this story does about it:** nothing to the write path itself, by design — the
  new `--trailing-days` mode is report-only and cannot overwrite the card (different
  script, different output field, `min_n=100` vs the card's `min_n=300`, no `--out`
  default pointing at the personas directory).
- **Recommended follow-up (out of scope, filed as a chip, not implemented here):** if
  spec.md §5's nightly log is ever scheduled, `measure_style_card.py` should gain a
  `--candidate-out` mode that writes to a side path (e.g.
  `<card>.candidate.json`) plus a diff-against-live summary, with the atomic
  `os.replace` onto the live path gated behind an explicit human step (a `--promote`
  flag or a separate `scripts/promote_style_card.sh`) — mirroring the human-approved
  promotion pattern already used for the blind-A/B gate
  (`~/.human/blind_ab_gate.json`, `scripts/check_capability_gates.py`) rather than
  inventing a new one.

## Files touched

| File | Change | LOC (est.) |
|---|---|---|
| `scripts/eval_persona_evolution.py` | `--trailing-days` argparse option + mutual-exclusion check with `--event`; `run()` branch computing `trailing_start`, calling `window_coverage`, reusing the `min_n`/`min_covered_days` refusal contract, emitting `trailing_window_summary`; docstring `Usage` section updated to show the corrected command | +55 / -0 |
| `scripts/test_eval_persona_evolution.py` | New hermetic tests (list below) | +140 |
| `docs/plans/2026-09-02-persona-evolution/spec.md` | New `## 8.` section (below) — honest event-shift comparison + corrects §5's `--window-days 30` example to `--trailing-days 30` | +~45 lines |
| `.github/workflows/ci.yml` | Add `python3 scripts/test_eval_persona_evolution.py` to the `capability-gate-check` job's stdlib-only pins (line ~971 area) — see "CI gap" below | +1 |
| `sprints/sprint-better-than-human-2026-09-05/evidence/us7-trailing-window-2026-09-05.json` | Committed aggregate-only output of one real `--trailing-days` run (AC-7.7) | new file |

### CI gap found in passing (VERIFIED)

`grep -n "eval_persona_evolution\|measure_style_card" .github/workflows/ci.yml` shows
only `test_measure_style_card.py` is wired into CI (`capability-gate-check` job).
`test_eval_persona_evolution.py` (649 lines, the file this story extends) and
`test_eval_persona_evolution_gate.py` (from `5e00683f5`) are **not run in CI at all**,
despite `measure_style_card.py` importing `aggregate_window`/`fetch_outbound_messages`
directly from `eval_persona_evolution.py` — a regression to either file today would only
be caught by whoever happens to run the test file by hand. Adding this story's new tests
to the existing `capability-gate-check` job (same job, same "hermetic, stdlib-only, no
creds" property as its sibling `test_measure_style_card.py` line) closes that gap for
free; it is a 1-line CI change bundled with this story rather than a separate chip
because the tests being added are exactly the ones exercising the new code.

## Honest write-up (AC-7.5) — text to append as spec.md `## 8.`

```markdown
## 8. US-7: honest event-shift comparison + windowed re-derivation (2026-09-05)

**Side-by-side, not sequential:** the two events' `lowercase_start_rate` deltas move in
*opposite* directions — move event +0.158 (95% CI-crossing, `results-2026-09-03.json`),
job event −0.088 (95% CI-crossing, `results-2026-09-05.json`). Presented without
qualification these read as one persona-wide trend; they are not comparable at equal
confidence:

- **job**: pre n=1,324 (30.0 covered days), post n=522 (29.5 covered days) — both windows
  meet `--min-covered-days=20`. This delta is real, load-bearing, and is the one the
  directional-fidelity gate ran against (status: FAIL — length_chars sign-matches,
  emoji_rate and warmth_hits_per_100_words do not; lowercase_start_rate itself is
  excluded from the gate as a device-autocapitalization artifact, spec §3b point 2).
- **move**: pre n=180 spans only **5.1** of an intended 30 covered days
  (`--min-covered-days=20` refuses it as of the 2026-09-05 coverage-floor fix). Its
  +0.158 delta is retired, not disproven — it is simply not backed by enough spread of
  days to rule out one or two atypical days dominating the mean. Treat every number
  attributed to the move event in this document (§3b, §4) as illustrative history, not a
  standing measurement.

**Corrected nightly command.** §5 above prescribes
`eval_persona_evolution.py --event none --full-range --window-days 30` as the rolling
nightly log command. **This does not do what it says**: `--window-days` is read only by
the event pre/post bucketer, never by `--full-range`, so that command reports the entire
`--start`..`--end` range (default since 2026-03-01), not a 30-day trailing window. The
corrected, verified command is:

    python3 scripts/eval_persona_evolution.py --event none --trailing-days 30 \
        --source data/imessage/training_pairs.jsonl \
        --source ~/.human/logs/eval-archive/imessage-corpus-backup-20260725-113543/training_pairs.jsonl

which reports `trailing_window_summary.coverage.covered_days` honestly (≈30 today,
bounded by the rolling chat.db retention floor — see below) instead of silently
mislabeling the full historical range as a 30-day window. Nightly *scheduling* of this
command remains unimplemented (§5's "what to log" was designed, never cronned) and is
unchanged by this story.

**chat.db retention floor is a moving target, verified live 2026-09-05:**
`min(is_from_me=1 date)` = 2026-08-06 (was 2026-08-03 on 2026-09-02 per §3, 2026-08-05 on
2026-09-05 morning per the follow-up above) — the floor has advanced 3 days in 3 days.
A `--trailing-days 60` run today can report at most `covered_days ≈ 30` from chat.db
alone; the `--source` merge extends the *pre-event* windows for the two named events
(static exports, does not extend the *trailing* window, which is always anchored at
"now") but does not and cannot change this fact for any future rolling window. Recovering
more pre-August history (AC-7.6) is the only way to raise this ceiling, and is explicitly
out of scope here.
```

## Privacy (AC-7.7)

Reuses `eval_persona_evolution.py`'s existing contract unchanged: chat.db opened
`mode=ro&immutable=1`; `--source` files parsed for `(timestamp, text)` in-process; text
is used only to compute the 9 numeric axes (`compute_features`) and is never printed,
logged, or written to `--out`. **VERIFIED** by inspecting every existing `results-*.json`
in `docs/plans/2026-09-02-persona-evolution/` — none contain message text, phone numbers,
or contact names, only counts, dates, and floats. The new `trailing_window_summary` field
follows the identical `aggregate_window()` output shape, so it inherits the same
guarantee structurally, not by a new ad hoc check. The evidence file committed under
`sprints/.../evidence/` is a copy of exactly this shape.

## Hermetic tests — `scripts/test_eval_persona_evolution.py`

All follow the existing `_Args`/`monkeypatch fetch_outbound_messages` pattern
(`test_eval_persona_evolution.py:334-427`) — no chat.db, no model, no network:

1. `test_trailing_days_and_nonnone_event_is_a_cli_error` — `--trailing-days 30 --event job`
   exits 2 via `p.error`, no `fetch_outbound_messages` call (mirrors
   `test_run_explain_dates_makes_no_db_call`'s `_boom` pattern).
2. `test_trailing_days_computes_start_from_end_minus_n` — feed messages spanning a known
   range, assert `trailing_window_summary.window.start == end - N`.
3. `test_trailing_days_reports_requested_days_and_true_coverage_separately` — feed data
   covering only 10 of a requested 60 days; assert `requested_days == 60` AND
   `coverage.covered_days ≈ 10`, distinct fields, neither overwriting the other (the
   literal AC-7.3 pin).
4. `test_trailing_days_refuses_below_min_n` — reuses `DEFAULT_MIN_N`; n=50 → exit 1, `--out`
   file not written (mirrors `test_run_refuses_and_writes_nothing_when_n_below_min`).
5. `test_trailing_days_refuses_below_min_covered_days` — n sufficient but all messages on
   one day → exit 1, reason names `covered_days`.
6. `test_trailing_days_succeeds_and_writes_out` — sufficient n and spread → exit 0,
   `trailing_window_summary.status == "OK"`, 9 axes present (reuses
   `aggregate_window`'s own shape, not re-asserted per-axis).
7. `test_trailing_days_reuses_source_merge` — pass a `--source` fixture, assert
   `source_stats`/`source_caveats` still populate exactly as they do for `--event`/
   `--full-range` today (proves no fork of the merge logic).
8. `test_trailing_days_independent_of_full_range` — both flags absent-vs-present combos
   don't interact; `--full-range` output is byte-identical whether or not
   `--trailing-days` is also given (pins Finding 2's deliberate non-fix of
   `full_range_summary`).
9. `test_trailing_days_explicit_start_warns_and_is_overridden` — passing both `--start`
   and `--trailing-days` prints the precedence note to stderr and uses the computed start.

`@covers-none` not needed — every test calls `epe.run()`/`epe.main()`, the production
entry points (satisfies `.claude/rules/test-references-production-symbol.md`).

## Risk analysis

- **Silent-success (the central risk this story exists to fix):** covered by Finding 2's
  fix (dedicated `coverage` field) and test 3 above, which pins the two numbers staying
  distinct rather than collapsing into one.
- **Reimplementing axis functions:** avoided structurally — the new code path calls
  `fetch_outbound_messages`, `merge_sources`, `aggregate_window`, `window_coverage`
  directly; no new metric math.
- **Editing persona JSONs:** not touched by any file in this design; `--trailing-days`
  writes only to `--out`.
- **PII leakage into committed artifacts:** covered under Privacy above; the one new
  committed file (`evidence/us7-trailing-window-*.json`) is the same shape as five
  existing, already-audited `results-*.json` files.
- **Breaking `--full-range`'s pinned tests (Finding 2's scope decision):** mitigated by
  explicitly NOT changing `full_range_summary`'s shape; test 8 above pins that decision
  so a future editor doesn't "fix" `--full-range` and break two existing tests as a
  side effect.

## Conflicts with other stories

None. `scripts/eval_persona_evolution.py`, `scripts/test_eval_persona_evolution.py`, and
`docs/plans/2026-09-02-persona-evolution/spec.md` do not appear in any other story's
"Files likely touched" list in `stories.md` (checked US-1, US-6, US-8 — the only other
stories with a "Files likely touched" section visible in the read range — none name
these paths). `sprints/sprint-better-than-human-2026-09-05/designs/US-3.md`'s own
"Conflicts" section independently confirms it touches only `eval_when_to_speak.py` and a
new `eval_seth_initiation_baseline.py`, disjoint from this story's files. The
`.github/workflows/ci.yml` edit (adding one line to the existing `capability-gate-check`
job) is additive and touches no line any other story's design has claimed.

## Out of scope

- AC-7.6: recovering pre-August chat.db history beyond the two `--source` exports already
  inventoried in spec.md §3b.
- Modifying `measure_style_card.py`, `~/.human/personas/*.style-card.json`, or
  `src/persona/style_card.c` / `persona.c` (AC-7.1; the "validated before it replaces the
  live one" gap above is documented, not fixed, here).
- Scheduling the nightly log (spec.md §5) as an actual cron/launchd job — this story fixes
  the command so that *when* it is eventually scheduled it reports honestly, but does not
  install the schedule.
- Re-running the directional-fidelity gate for the move event — impossible on current
  data (Finding: "Event-shift detection" above).
- Fixing `--full-range`'s missing `coverage` field (Finding 2 is fixed only for the new
  `--trailing-days` path, by design — see Risk analysis).

## Estimate

**S.** One new argparse option + `run()` branch reusing four existing functions verbatim
(~55 LOC), ~140 LOC of hermetic tests following an established pattern in the same file,
one CI line, and a ~45-line prose section citing numbers that already exist in committed
JSON. No new files except the tests-adjacent evidence JSON and no C/build-system changes.

RESULT_tech-lead=READY
US-7 design complete: `--trailing-days` mode (reusing existing axis/CI/refusal-contract functions verbatim) fixes two verified gaps (`--window-days` no-op under `--full-range`; missing `coverage` field), and the spec.md write-up states the job-vs-move lowercase_start_rate comparison (+0.158 vs −0.088) with the move event correctly flagged low-confidence/retired.
