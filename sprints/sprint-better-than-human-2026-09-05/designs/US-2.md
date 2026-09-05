# Design for US-2: Per-cycle LUAR promotion gate that blocks a regressed adapter

**Status:** READY
**Date:** 2026-09-05
**Worktree:** `/Users/sethford/Projects/h-uman/.claude/worktrees/sprint-better-than-human-2026-09-05`

## 0. Corrections to the premise (VERIFIED by reading the actual code)

Before the approach: two assumptions baked into the story text turned out to be wrong
when checked against the code, and they change where the gate has to live.

1. **"Appendix H: `training_loop.py` → `adapter_is_real.py` → registry write" is not a
   real automated sequence.** VERIFIED: `scripts/nightly-retrain.sh:460-504` (base
   training) and `:191-244` (mlx-tune candidate stage, `run_mlxtune_candidate_stage`)
   both stop after `adapter_is_real.py`. Neither calls `register_v6_adapter.py` or
   `m3_promote.py`. Line 243-244 literally prints `"candidate staged at $candidate_dir
   (NOT promoted)"` and `"to promote after human review: python3 .../register_v6_adapter.py
   ..."`. `docs/research/2026-09-02-august-2026-sota-gap-analysis.md:165` (Appendix H
   itself) confirms this in prose: *"The adapter is staged, not promoted."* There is
   today **no automatic registry write to block** — promotion is always a manual,
   human-triggered second step. This means "wire the gate before the registry write"
   cannot mean "insert a line in the nightly script"; it means gate the manual tools
   that DO write the registry.
2. **`scripts/register_v6_adapter.py` never sets `promoted: true`.** VERIFIED by reading
   the whole file: `metrics["promoted"] = False` is hardcoded (line 260) and
   `metrics["human_gate"]` is always `"PENDING"`. Confirmed against the live registry
   (`~/.human/training-data/adapters/registry.json`): all 16 entries with a `training`
   row show `promoted: False/None`; the only entry with a real promotion is
   `seth-glm-air-v6-orpo-real-20260802-190128`, and its evidence comes from a **separate
   top-level `promotion` block**, not from a `training` row. `entry_is_promoted()`
   (register_v6_adapter.py:92-98) checks exactly that: `entry.get("promotion")` OR a
   training row with `metrics.promoted == True`. Grepping the whole repo
   (`grep -rn 'adapter_registry.record_promotion' scripts/*.py`) turns up exactly one
   call site: **`scripts/m3_promote.py:238`**, inside `cmd_promote()`, right after
   `swap_adapter()` (line 226) actually POSTs the live LoRA swap to `:8741`. **That is
   the one and only place in this codebase that performs "registry promotion."**
   `register_v6_adapter.py` records *training evidence*, not a promotion.

Consequence: the gate has two jobs, at two different call sites, and they must not be
confused with each other:

- **Where a regressed adapter could actually reach production:** `m3_promote.py:cmd_promote()`
  — this is where "cannot promote silently" is enforced (AC-2.2).
- **Where the evidence should be visible for the human who decides whether to run
  `m3_promote.py` at all:** `register_v6_adapter.py` and `nightly-retrain.sh`'s log —
  annotation, not enforcement, but this is the file the story's own "Files likely
  touched" list names, so it gets the visibility half of the work.

A third finding, load-bearing for AC-2.1/2.3 (see §3): the `n` field inside
`authorship_gap.py`'s own `twin_seth_vs_adapter` stat block is **always 200** (the
bootstrap-resample count, `a.splits`), regardless of how many real trials or other
senders were available — it is not the signal the DPO `val_loss=None` precedent
implies. The real "did this measurement have enough data" signal is the *presence and
shape* of the JSON at all: `authorship_gap.py` (lines 73-85) already refuses — exit
non-zero, **writes nothing** — before ever reaching the splits loop, whenever there are
zero/too-few usable trials, LUAR fails to load, or fewer than 20 other senders exist.
So "the file exists and parses with a numeric `twin_seth_vs_adapter.mean`" *is* the
correct INCONCLUSIVE-vs-measured signal; treating the literal `n: 200` field as that
signal (as a literal reading of AC-2.3 might suggest) would be checking a field that
can never be missing or small when the file exists at all, and would silently pass
every case the AC intends to catch. This is documented explicitly rather than shipped
silently, per `.claude/rules/verify-before-you-claim.md`.

## 1. Approach

A single pure predicate, two enforcement points, one annotation point, reusing the
measurement pipeline that already runs nightly instead of adding a new one.

### 1.1 The predicate (new, shared)

`scripts/blind_ab/authorship_promotion_gate.py` — a new module in the same directory as
`authorship_gap.py`, `score_candidate_offline.py`, and `casing_probe.py` (which it
mirrors: `compute_casing_gate()` is the precedent for "a pure function that returns a
verdict dict, plus a `_from_file` loader that raises `SystemExit` on missing evidence,
plus a thin CLI").

```python
def decide_promotion(candidate_twin, serving_twin, floor, candidate_twin_ci95,
                      min_gain=0.05) -> dict:
    """Pure. Returns {"verdict": "PASS"|"BLOCK"|"HOLD", "reason": str,
    "candidate_twin", "serving_twin", "floor", "delta", "candidate_twin_ci95",
    "min_gain"}.
    Never returns INCONCLUSIVE — that state is a property of MISSING inputs,
    decided by the caller (load_gate_inputs_from_score_json /
    load_gate_inputs_from_gap_jsons below) before this function is ever called.
    """
    delta = round(candidate_twin - serving_twin, 4)
    ci_lo, ci_hi = candidate_twin_ci95
    if candidate_twin < floor:
        return {"verdict": "BLOCK", "reason": "below_floor", "delta": delta, ...}
    if ci_hi < serving_twin:
        return {"verdict": "BLOCK", "reason": "regression_ci_distinguishable", "delta": delta, ...}
    if ci_lo > serving_twin:
        return {"verdict": "PASS", "reason": "twin_improved_ci_distinguishable", "delta": delta, ...}
    if delta >= min_gain:
        return {"verdict": "PASS", "reason": "twin_improved_min_gain", "delta": delta, ...}
    return {"verdict": "HOLD", "reason": "within_noise", "delta": delta, ...}
```

**UPDATED by the F1 fix (2026-09-05) — see §10.** The shape above is the CURRENT
implementation, not the original one. The story's first cut used a point-mean-only
boundary (`candidate_twin <= serving_twin + min_gain -> BLOCK`, `min_gain` defaulting
to `0.0`); §10 explains why that BLOCKed real, non-regressed cycles as "regressions"
and documents the noise-aware three-way replacement. `min_gain` is still a keyword,
not a magic number buried in the body (so it can be retuned without touching control
flow), but it now defaults to `0.05` and only applies when the candidate's own CI is
too wide to decide the comparison on its own.

Two loaders sit in front of `decide_promotion`, both returning either a fully-populated
input dict or raising `SystemExit("INCONCLUSIVE: ...")` — never a partially-filled dict
with a `None` silently defaulted to something computable:

```python
def load_gate_inputs_from_score_json(path) -> dict:
    """Primary path (AC-2.1): reads a score_candidate_offline.py comparison JSON
    directly — candidate_twin/serving_twin/floor all come from ONE file, measured
    the same night with the same seed/splits/other-senders draw, which removes the
    day-to-day floor-redraw confound a two-separate-files comparison would have.
    Raises SystemExit if the file is missing, fails to parse, or lacks
    comparison.twin_candidate / comparison.twin_serving / candidate.floor_seth_vs_
    other_humans.mean as finite floats — this is the generalized form of AC-2.3's
    'authorship_gap.py refused' / 'either JSON is missing/lacks a measurement'
    (see §0 for why checking the literal `n` field would not catch this)."""

def load_gate_inputs_from_gap_jsons(candidate_path, serving_path) -> dict:
    """Secondary path (stories.md's 'possibly a --prior-twin comparison flag'):
    two separate authorship_gap.py --out files, e.g. authorship_nightly.sh's daily
    ~/.human/logs/authorship-gap-<date>.json for 'serving' when no candidate
    trained tonight. Same missing/malformed -> SystemExit contract."""
```

### 1.2 Enforcement point 1 (measurement, already-nightly): `score_candidate_offline.py`

Add one field to the existing output dict (after `casing_gate`, around
`scripts/blind_ab/score_candidate_offline.py:392-398`):

```python
from authorship_promotion_gate import decide_promotion  # lazy import, real-run only
...
floor = gap_results["candidate"]["floor_seth_vs_other_humans"]["mean"]
try:
    gate = decide_promotion(cand_twin, serv_twin, floor)
except Exception as e:
    gate = {"verdict": "INCONCLUSIVE", "reason": str(e)}
out["promotion_gate"] = gate
```

This runs automatically every night inside `nightly-retrain.sh`'s mlx-tune candidate
stage (`:236-240`, serving-down window, `HU_RETRAIN_MLXTUNE=1` — confirmed **live** in
`~/Library/LaunchAgents/ai.human.nightly-retrain.plist`'s
`EnvironmentVariables.HU_RETRAIN_MLXTUNE = "1"`), which satisfies AC-2.5's "only inside
that serving-down window" literally, without loading any additional model (this script
already loads exactly the two adapters it was going to load; the gate is pure
arithmetic on the LUAR output it already produced). `score_candidate_offline.py`'s own
exit code and its documented contract ("This script only measures... Never promotes
anything") are **unchanged** — `nightly-retrain.sh:240` already treats its rc as
informational (`tee -a "$LOG"`, no branch on exit code), so changing that contract
would be a second, unreviewed behavior change outside this story's scope.

Add one `nightly-retrain.sh` line right after `:240` so a human doesn't have to open the
JSON to see the verdict before deciding to run the manual promote command:

```bash
gate_verdict=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('promotion_gate',{}).get('verdict','UNKNOWN'))" "$score_out" 2>/dev/null || echo "UNKNOWN")
log "mlx-tune candidate stage: promotion_gate=$gate_verdict (see $score_out)"
```

### 1.3 Enforcement point 2 (the actual block): `m3_promote.py:cmd_promote()`

This is the one call site that performs a live swap (`swap_adapter()`,
`m3_promote.py:94`, called at `:226`) and writes `adapter_registry.record_promotion()`
(`:238`) — the literal "registry promotion" AC-2.2 says must be blocked. Insert the
check between the existing scale gate (ends `:204`) and the evidence-string check
(starts `:208`), i.e. as a new precondition in the same style as the scale gate that
already lives there:

```python
# US-2: never promote an adapter whose measured authorship twin regressed against
# what is currently serving, or fell below the measured floor. --skip-authorship-gate
# is the explicit, logged override for genuine emergencies (e.g. promoting a rollback
# target that predates this gate's own JSON).
if not args.skip_authorship_gate:
    gap_json = args.gap_json or _find_latest_score_json(args.adapter)
    try:
        inputs = load_gate_inputs_from_score_json(gap_json)
        verdict = decide_promotion(inputs["candidate_twin"], inputs["serving_twin"], inputs["floor"])
    except SystemExit as e:
        verdict = {"verdict": "INCONCLUSIVE", "reason": str(e)}
    if verdict["verdict"] != "PASS":
        print(f"ERROR: refusing to promote {args.adapter}: authorship promotion gate "
              f"{verdict['verdict']} ({verdict['reason']}). Pass --skip-authorship-gate "
              f"to override (will be recorded as an override, not silently).",
              file=sys.stderr)
        return 5
```

`_find_latest_score_json(adapter_path)` looks for
`~/.human/logs/candidate-authorship-*.json` whose `candidate_adapter` field
(`score_candidate_offline.py`'s output already carries this, see that script's `out`
dict) equals the path being promoted — never a bare "newest file", which would silently
score the wrong adapter. If none matches, `SystemExit` → INCONCLUSIVE → blocked, per
AC-2.3's "missing measurement blocks, does not silently pass."

`return 5` is a new exit code (existing ones: 2 unreachable/bad-input, 3 confirmation
required, 4 scale ceiling — 5 continues that enumeration; document it in the module
docstring's "Exit codes" list, `m3_promote.py:32-35`).

An override is recorded, not merely permitted: when `--skip-authorship-gate` is used,
`cmd_promote()` appends the override to the `evidence` string before it reaches
`record_promotion()` (`"(authorship gate OVERRIDDEN: <verdict/reason>) " + evidence`),
so the registry — not just a terminal log a human may not have seen — carries the fact
that a regressed or unmeasured adapter was promoted anyway.

### 1.4 Annotation point (the AC-named file): `register_v6_adapter.py`

`register_v6_adapter.py` never promotes (§0), so it must not *block* — it should
annotate, matching its own existing pattern for `human_gate` (line 259) and `smoke`
(lines 288-293: `{"status": "NOT_RUN"}` when absent, real dict when present). Add,
right before the `metrics["human_gate"]` line (`:259`):

```python
metrics["authorship_gate"] = _read_authorship_gate_for(adapter)  # {"status": "NOT_RUN"} if no matching JSON
```

`_read_authorship_gate_for()` reuses the same `_find_latest_score_json` /
`load_gate_inputs_from_score_json` pair from `authorship_promotion_gate.py` (imported,
not reimplemented — `.claude/rules/test-references-production-symbol.md`'s "don't
reinvent" principle applies to production code too). This is purely informational: it
does not call `sys.exit` on BLOCK, because recording that a regression happened is
useful staging evidence (the same reason `human_gate: PENDING` and `smoke: NOT_RUN`
already exist as non-blocking annotations here) and this script never sets
`promoted: True` regardless.

## 2. Files touched

| File | Change |
|---|---|
| `scripts/blind_ab/authorship_promotion_gate.py` (new) | `decide_promotion()`, `load_gate_inputs_from_score_json()`, `load_gate_inputs_from_gap_jsons()`, thin CLI (`--score-json` or `--candidate-json`+`--serving-json`, prints verdict, exit 0=PASS/1=BLOCK/2=INCONCLUSIVE) |
| `scripts/blind_ab/score_candidate_offline.py` | add `promotion_gate` field to output JSON (near line 392-398); no exit-code change |
| `scripts/m3_promote.py` | new precondition in `cmd_promote()` between lines 204/208; new `--gap-json` and `--skip-authorship-gate` args in `main()`'s arg parser; exit code 5; override-recorded-in-evidence |
| `scripts/register_v6_adapter.py` | add `metrics["authorship_gate"]` annotation before line 259 |
| `scripts/nightly-retrain.sh` | one `log` line after `:240` surfacing `promotion_gate.verdict` |
| `scripts/blind_ab/test_authorship_promotion_gate.py` (new) | hermetic fixture tests, §5 |
| `scripts/test_m3_promote.py` | extend with the new precondition's PASS/BLOCK/INCONCLUSIVE/override cases (reuses that file's existing fake-MLX HTTP server harness) |
| `scripts/test_register_v6_adapter.py` | extend with an `authorship_gate` annotation-present/absent case |

No `src/` C changes. No new files under `src/`. Ratchets (`file-size-ceiling`,
`clone-ratchet`, `sqlite-includer-ratchet`, `no-new-root-files`, `agent-core-boundary`,
`modeled-person-layering`, `edge-context-isolation`) are all untouched by construction —
verified by scope: every changed/new file is under `scripts/`.

## 3. Where the gate reads its numbers, and how it refuses (AC-2.1, AC-2.3, no-number-without-a-measurement)

**Reads:** `comparison.twin_candidate`, `comparison.twin_serving` (both floats,
`score_candidate_offline.py:377-382`, each one is authorship_gap.py's own
`twin_seth_vs_adapter.mean` from its own JSON — see `score_candidate_offline.py:366-370`
which loads `gap_results[label] = json.load(f)` from a genuine `authorship_gap.py --out`
file), and `candidate.floor_seth_vs_other_humans.mean` (the measured floor from the SAME
run, not the 0.62 constant cited in the story text, which is context/history only —
AC-2.1's explicit requirement). Never a hardcoded 0.625/0.70/0.62.

**Refuses (INCONCLUSIVE, which BLOCKS the swap — never a silent PASS) when:**
- the expected `candidate-authorship-*.json` does not exist for the adapter being
  promoted (nightly run never happened, or matched no candidate) — `SystemExit` from
  `_find_latest_score_json` / `load_gate_inputs_from_score_json`;
- the file exists but fails to parse, or `comparison.twin_candidate` /
  `comparison.twin_serving` / `candidate.floor_seth_vs_other_humans.mean` is missing or
  not a finite float — covers `authorship_gap.py` itself refusing for EITHER side
  (score_candidate_offline.py already turns that into `sys.exit(... nothing written)`
  before ever producing the comparison JSON, so "file exists with a well-formed
  `promotion_gate` field" already implies both underlying LUAR runs succeeded — this is
  the corrected, verified version of AC-2.3's literal "either JSON is missing/lacks
  `n`", per §0's finding that the literal `n` field cannot express this);
- `--gap-json` is passed explicitly but does not exist / does not parse.

**Never refuses silently as a PASS**: `decide_promotion()` itself has exactly three return
values, `PASS`, `BLOCK`, and `HOLD` (§10) — `INCONCLUSIVE` is a property of the *loader*
raising before `decide_promotion` is ever called, so there is no code path where a
missing input defaults to a number that lets the predicate compute a false PASS. As of
the F1 fix (§10), that loader-level refusal also covers a missing/malformed
`candidate.twin_seth_vs_adapter.ci95` — the noise-aware gate's new required input — not
just the three means. This is the direct application of
`.claude/rules/no-number-without-a-measurement.md`'s "refuse loudly, don't fall back" to
this predicate's own contract.

## 4. Operationalizing "toward the 0.70 ceiling" with CIs (n≈36 per run)

Grounded in the live evidence: `~/.human/logs/authorship-gap-2026-09-04.json` —
`trials: 36`, `splits: 200`, `ceiling_seth_vs_seth.mean: 0.701` (ci95
`[0.616, 0.798]`), `twin_seth_vs_adapter.mean: 0.625` (ci95 `[0.506, 0.725]`),
`floor_seth_vs_other_humans` not shown above but reported alongside at ~0.62-0.63 per
the story text. This is the exact regression this story exists to catch.

Important nuance, worth stating explicitly rather than glossing over: `authorship_gap.py`'s
`stat()` (`authorship_gap.py:105-108`) computes its `ci95` as a **percentile bootstrap
over `splits=200` resamples drawn from the SAME ~36-trial pool** (`idx = list(range(len(trials)))`,
reshuffled per split — `authorship_gap.py:92-104`). The 200 draws are *not* 200
independent measurements; their true resolution is bounded by the ~36-trial pool they
resample from (visible in the CI width itself: `0.506-0.725`, a span of 0.22 on a metric
whose whole meaningful range is roughly floor-to-ceiling, `~0.62` to `~0.70`). A gate
that chased noise inside that band would flap.

**SUPERSEDED by the F1 fix (2026-09-05) — see §10.** The paragraph below is the
ORIGINAL design's reasoning, kept for the record; it turned out to be wrong about the
2026-09-02 → 2026-09-04 cycle, which is exactly the case §10 fixes.

~~**AC-2.2's literal boundary is a point-mean comparison** (`new_twin <= prev_twin OR
new_twin < floor`), which is what `decide_promotion()` implements — this story does not
ask for a significance test, and the one scenario it must catch (AC-2.4: 0.70 → 0.625)
is a ~5x-larger move than the CI half-width above, so the literal boundary is not
noise-chasing for the case that motivated the story.~~ But the gate's JSON output carries
both sides' full `ceiling_seth_vs_seth` / `twin_seth_vs_adapter` /
`floor_seth_vs_other_humans` blocks (via `load_gate_inputs_from_score_json` passing the
whole `gap_results` dicts through, not just the three means it needs for the decision),
so a human reviewing a borderline PASS/BLOCK/HOLD can see the CI overlap and judge
whether the move is inside noise — this visibility mechanism survives the F1 fix
unchanged, it's just no longer the ONLY way the CI gets used.
**Toward the ceiling** is read as: report `gap_closed_fraction`
(`authorship_gap.py:116`, `(twin - floor) / (ceiling - floor)`) for both candidate and
serving in the gate's output, so "moving toward 0.70" is legible as a fraction-closed
delta, not just a raw twin delta — useful trend context regardless of which of the
three verdicts the CI-based boundary lands on.

~~Risk flagged, not silently absorbed: with n≈36 and this CI width, a genuine improvement
smaller than ~0.05-0.10 could be a coin flip. If this becomes a practical problem (BLOCK
flapping on marginal candidates), the fix is `min_gain` (already a keyword on
`decide_promotion`, §1.1) or requiring 2 consecutive nights' BLOCK before treating it as
a hard stop — explicitly **not** built into this story (M-sized, and AC-2.4's fixture
doesn't need it); noted here so it isn't rediscovered the hard way.~~ **This is exactly
what happened**: the 2026-09-02 → 2026-09-04 cycle (twin 0.633 → 0.625, delta -0.008)
BLOCKed under the point-mean gate, and would have BLOCKed every subsequent cycle
indefinitely — the "practical problem" flagged here, not a hypothetical. §10 is that
fix, landed the same sprint rather than deferred to a future story.

## 5. Hermetic test plan (non-vacuous assertions — a gate that cannot fail is a bug)

`scripts/blind_ab/test_authorship_promotion_gate.py` (plain-Python, no pytest, matching
`scripts/test_m3_promote.py`'s style — subprocess/importlib, no live model, no network,
no chat.db):

1. **`test_block_known_regression`** — AC-2.4's exact fixture: `candidate_twin=0.625,
   serving_twin=0.70, floor=0.62`. Asserts `verdict == "BLOCK"` AND
   `reason == "regression_vs_prior"` (not just "not PASS" — a test that only checks
   `verdict != "PASS"` would also pass if the code always returned `BLOCK`, which is
   the "gate that cannot fail" bug this rule warns about; asserting the *reason* proves
   the decision path, not just the outcome).
2. **`test_pass_genuine_improvement`** — `candidate_twin=0.71, serving_twin=0.625,
   floor=0.62`. Asserts `verdict == "PASS"`. Proves the predicate can say yes — a
   predicate with no PASS-path test is exactly as unverified as one with no BLOCK-path
   test.
3. **`test_block_below_floor_even_if_improved`** — `candidate_twin=0.60,
   serving_twin=0.55, floor=0.62`. Candidate improved over serving but is still below
   the measured floor. Asserts `verdict == "BLOCK"` AND `reason == "below_floor"` — this
   is the case a naive `candidate > serving` gate would wrongly PASS, and it is
   AC-2.2's second OR-clause, tested independently of the first.
4. **`test_boundary_equal_is_block`** — `candidate_twin == serving_twin` exactly.
   AC-2.2 says `<=`, not `<`; assert `BLOCK` to pin the boundary (an off-by-one here is
   invisible in every other test).
5. **`test_inconclusive_missing_file`** — `load_gate_inputs_from_score_json` on a path
   that does not exist. Asserts `SystemExit` is raised (not a return value — the CLI
   maps that to exit code 2 / verdict INCONCLUSIVE) with "INCONCLUSIVE" in the message.
6. **`test_inconclusive_malformed_json`** — a real temp file containing
   `{"comparison": {"twin_candidate": 0.7}}` (missing `twin_serving` and no `candidate`
   key at all — the shape `authorship_gap.py`'s own refusal-before-write contract
   guarantees can never actually reach disk, but the loader must not `KeyError` on a
   hand-edited or truncated file; it must convert to the same `SystemExit`
   INCONCLUSIVE contract).
7. **`test_inconclusive_non_finite_twin`** — a file where `twin_candidate` is present
   but is `null` or `"NaN"` (JSON has no NaN literal, but a hand-edited file or a future
   producer bug could write a string) — asserts SystemExit, proving the loader checks
   *type*, not just *presence*, closing the exact class of bug
   `.claude/rules/no-number-without-a-measurement.md` catalogs (a null/None value that
   survives as if it were zero).
8. **`test_m3_promote_blocks_on_regressed_gate`** (in `scripts/test_m3_promote.py`,
   extending its existing fake-MLX-server harness): write a `candidate-authorship-*.json`
   fixture matching AC-2.4's regression shape with `candidate_adapter` equal to the
   `--adapter` path under test; run `cmd_promote` against the fake MLX server; assert
   **the fake server never receives the swap POST** (the strongest possible assertion —
   not just "exit code 5", which could pass even if the swap fired and only the exit
   code lied) AND `adapter_registry.record_promotion` was not called (assert the
   registry file's mtime/content is unchanged before/after).
9. **`test_m3_promote_passes_on_improved_gate`** — same harness, PASS-shaped fixture;
   asserts the swap DOES fire and the registry DOES get a `promotion` block — the PASS-path
   companion to #8, required for the same reason as #2 (a promote command that always
   refuses would also make #8 pass).
10. **`test_m3_promote_skip_flag_records_override`** — regression-shaped fixture,
    `--skip-authorship-gate` passed; asserts the swap DOES fire (override works) AND
    the registry's `evidence` string contains "OVERRIDDEN" and the verdict/reason that
    was overridden (proves the override is logged, not silent).
11. **`test_register_v6_adapter_annotates_absent`** (in `test_register_v6_adapter.py`):
    no matching `candidate-authorship-*.json` on disk; asserts
    `metrics["authorship_gate"] == {"status": "NOT_RUN"}` and that registration still
    succeeds (annotation is informational, never blocking, per §1.4).
12. **`test_register_v6_adapter_annotates_block`**: a matching regressed-shaped fixture
    present; asserts `metrics["authorship_gate"]["verdict"] == "BLOCK"` is recorded AND
    registration still succeeds with `promoted: False` unchanged (proves annotation
    never flips this script's own promoted flag, which was already always False —
    guards against a future edit accidentally wiring this into a block).

All twelve are pure/hermetic: no chat.db read, no LUAR/torch import, no model load, no
network except test #8/#9/#10's already-existing loopback fake-HTTP-server pattern from
`test_m3_promote.py`. Per `.claude/rules/reports-success-does-nothing.md`'s "prove a
guard discriminates": tests #1 vs #2, #3, and #4 are deliberately paired
BLOCK/PASS/boundary cases specifically so that a predicate hardcoded to always return
one verdict fails at least one test in every group.

## 6. AC-2.6 — one real gate run's evidence

Not fabricated here (that would itself violate
`.claude/rules/no-number-without-a-measurement.md`). Once `score_candidate_offline.py`'s
`promotion_gate` field ships and the next `HU_RETRAIN_MLXTUNE=1` window runs (nightly,
02:00-05:00 per the plist), the implementer commits that night's
`~/.human/logs/candidate-authorship-<date>.json` (containing `promotion_gate`) to
`sprints/sprint-better-than-human-2026-09-05/evidence/` verbatim — whichever verdict it
actually produces (PASS, BLOCK, or INCONCLUSIVE if e.g. the preference corpus is still
below US-1's floor that night). No message text, handles, or contact names are in this
file (`authorship_gap.py` writes only cosines/stats — verified by reading its `out`
dict construction, `authorship_gap.py:109-119`, which contains no raw text fields at
all — the closest it comes is `reading` and `protocol`, both fixed strings).

## 7. Risks

- **Never restart `:8741` or the service-loop.** Confirmed nothing in this design does:
  `score_candidate_offline.py` is unchanged in that respect (its subprocess generation
  already runs with serving stopped, per its own docstring and
  `never_two_llm_instances`); `m3_promote.py`'s `swap_adapter()` is a hot POST to a
  *running* server, not a restart, and this design adds a precondition *before* that
  call, never touching the call itself. The gate is pure JSON arithmetic — no model
  load anywhere in `authorship_promotion_gate.py`.
- **Model loads only inside the nightly window.** `authorship_promotion_gate.py`
  imports nothing beyond stdlib (`json`, `argparse`, `sys`, `pathlib`) — mirrors
  `casing_probe.py`'s and `score_candidate_offline.py`'s own "import time touches only
  stdlib" discipline (`score_candidate_offline.py:23-28`'s docstring commitment). Safe
  to import from `m3_promote.py` (a manually-run, no-model CLI) with zero risk of
  pulling in torch/mlx_lm.
- **No private text.** §6 above; the gate's own JSON output carries only floats, dates,
  paths, and fixed strings — verified against `authorship_gap.py`'s `out` dict.
- **Ratchets.** No `src/` changes at all (§2) — every ratchet in
  `.claude/rules/*ratchet*.md` and `agent-core-boundary` / `modeled-person-layering` /
  `edge-context-isolation` is scoped to `src/`, so none apply.
- **New risk this design surfaces (not in the skeleton): a second, uncoordinated
  automated promotion path exists.** VERIFIED by grep: `scripts/m3_outcome_driver.py`
  has a `--run-loop` mode (`:568-587`) that trains AND calls `swap_adapter()`
  (`:428`, `:584`) purely on a sample-count threshold, with **no LUAR/authorship gate at
  all**. Checked whether it is actually live: the scheduled launchd job
  (`ai.human.m3-loop.plist` → `scripts/m3_loop_cycle.sh:104`) invokes
  `m3_outcome_driver.py` **without** `--run-loop` (poll-only); `--run-loop` only appears
  in `scripts/live_fire_m3_loop.sh:230`, which is not referenced by any installed
  launchd plist (checked `~/Library/LaunchAgents/` — no match). So this path is
  currently dormant in production, but it is the same class of gap AC-2.2 exists to
  close, and it is NOT touched by this story (out of scope — different file, different
  estimate). Flagged here rather than silently left for a future audit to "discover" as
  if it were new.
- **`m3_promote.py`'s existing scale ceiling (8.0) vs `adapter_is_real.py`'s (4.0).**
  Pre-existing inconsistency (VERIFIED, both files, both cited in §0/§1.3), unrelated to
  this story's predicate but sitting one function away from where this story edits;
  not touched here (different rule, different story) but noted so nobody assumes this
  design silently reconciled it.

## 8. Conflicts with other stories' files

None. Verified against `stories.md`'s "Files likely touched" for every other story in
this sprint:
- US-1: `scripts/build_v6_preference_corpus.py` / `scripts/merge_seth_preference_sources.py` — disjoint.
- US-3: new `scripts/eval_seth_initiation_baseline.py` — disjoint (reads `chat.db` +
  `scripts/blind_ab/score.py`'s `wilson()`, not anything this story touches).
- US-4: `scripts/eval_when_to_speak.py` — disjoint.
- US-5: `src/memory/*`, `src/memory/retrieval/hybrid.c`,
  `scripts/eval_semantic_live_gate.py` — disjoint, and no `src/` overlap at all.
- US-6: `scripts/blind_ab/make_rating_sheet.py` (or sibling) — same directory as this
  story's new/changed `blind_ab/` files but a different file; no shared function names
  checked via `authorship_promotion_gate`/`score_candidate_offline`/`casing_probe`
  grep — zero hits in US-6's text.

Only a logical dependency: US-1 (corpus rebalance) feeds a better candidate for this
gate to score, but per stories.md's own note this story "can be built and
fixture-tested independently" — confirmed true by this design, since every hermetic
test in §5 uses synthetic JSON, never a real corpus or a real training run.

## 9. Estimate

**M**, matching stories.md. Breakdown: `authorship_promotion_gate.py` + its 7 unit
tests (~half a day), `m3_promote.py` precondition + 3 integration tests reusing its
existing fake-MLX harness (~half a day), `score_candidate_offline.py` +
`register_v6_adapter.py` annotations + their tests (~a few hours),
`nightly-retrain.sh` one-line surfacing + AC-2.6 evidence capture on the next real
nightly window (calendar time, not engineering time — the window runs itself at
02:00-05:00; the implementer's job is to be ready to commit its output).

## 10. F1 fix (2026-09-05): noise-aware, three-way verdict (PASS/BLOCK/HOLD)

**Critic finding F1 (MEDIUM):** `decide_promotion()`'s point-mean-only boundary
(`candidate_twin <= serving_twin + min_gain -> BLOCK`, `min_gain=0.0`) BLOCKs on pure
measurement noise. The real 2026-09-02 → 2026-09-04 cycle (serving twin 0.633,
candidate twin 0.625, delta -0.008) is well inside the CI half-width authorship_gap.py
actually measures at n≈36 (§4: `[0.506, 0.725]` on the same twin value, a span of 0.22)
— the point-mean gate would BLOCK this cycle, and every subsequent cycle with a
similarly small delta, forever. §4's own "Risk flagged, not silently absorbed"
paragraph predicted exactly this failure mode and named `min_gain` as the fix; this
section is that fix, landed rather than deferred.

**The fix**, implemented in `authorship_promotion_gate.py`'s `decide_promotion()` (see
§1.1's updated code block) and threaded through both loaders
(`load_gate_inputs_from_score_json` / `load_gate_inputs_from_gap_jsons`, which now also
require `candidate.twin_seth_vs_adapter.ci95` — an ordered `[lo, hi]` pair of finite
floats — and `SystemExit("INCONCLUSIVE: ...")` if it's missing or malformed, per §3):

1. **BLOCK "below_floor"** — `candidate_twin < floor`. Unchanged from the original
   design: a point comparison regardless of CI width, since scoring below raw
   non-Seth-human authorship is disqualifying on its own terms, not a noise question.
2. **BLOCK "regression_ci_distinguishable"** — the candidate's CI upper bound is below
   the serving twin's point estimate. The data can positively rule out "no change or
   better," so this is a real, not noise-attributable, regression.
3. **PASS "twin_improved_ci_distinguishable"** — the candidate's CI lower bound is
   above the serving twin's point estimate. Mirror of (2): the data can positively
   rule out "no change or worse."
4. **PASS "twin_improved_min_gain"** — neither CI bound crosses the serving twin (the
   two are statistically indistinguishable by CI alone), but the point-estimate gain
   is at least `min_gain` (now defaulting to `0.05`, roughly half the ~0.1 CI
   half-width measured in practice) — a deliberately smaller bar than full CI
   separation, so a real, if noisy-looking, improvement isn't stuck behind (3) forever.
5. **HOLD "within_noise"** — none of the above. No promotion (a consumer treats this
   exactly like BLOCK for any swap decision — see below), but also no regression is
   claimed; the correct action is to accumulate another measurement cycle.

**Consumers.** `m3_promote.py:cmd_promote()` already refuses the swap for
`gap_verdict.get("verdict") != "PASS"` (exit 5) — HOLD needed no new branch there, only
the extra `candidate_twin_ci95` argument threaded into its `decide_promotion()` call and
a docstring/comment update naming HOLD explicitly, so a reader doesn't assume exit 5
only ever means "regressed." `register_v6_adapter.py`'s informational annotation
(`_read_authorship_gate_for`) is unaffected in shape — it still never blocks
registration and never flips `promoted` — but now surfaces `HOLD`/`within_noise` as a
third possible `authorship_gate.verdict` value alongside `PASS`/`BLOCK`. The CLI
(`authorship_promotion_gate.py main()`) gained a fourth exit code: `0`=PASS, `1`=BLOCK,
`2`=INCONCLUSIVE (unchanged), `3`=HOLD (new — distinct from BLOCK's `1` so a caller
script can special-case "accumulate another cycle" without string-matching the reason).

**Tests** (`scripts/blind_ab/test_authorship_promotion_gate.py`,
`scripts/test_m3_promote.py`, `scripts/test_register_v6_adapter.py`): the real
0.633/0.625 pair with a realistic CI → HOLD, not BLOCK
(`test_hold_within_noise_real_09_02_vs_09_04_cycle`); a candidate 0.60 with CI upper
bound 0.62 against serving 0.633 → BLOCK
(`test_block_ci_distinguishable_regression_low_candidate`); a candidate 0.70 with CI
lower bound 0.64 → PASS (`test_pass_ci_distinguishable_improvement`); below-floor still
BLOCKs regardless of CI width
(`test_block_below_floor_even_if_improved_and_ci_wide`); a missing/malformed CI field
INCONCLUDEs at the loader (`test_inconclusive_missing_ci95_field`,
`test_inconclusive_malformed_ci95_field`); all four CLI exit codes are asserted
(`test_cli_exit_0_on_pass` / `_1_on_block` / `_2_on_inconclusive` / `_3_on_hold`). The
`m3_promote.py`/`register_v6_adapter.py` integration tests were updated in place: the
original AC-2.4 fixture (0.70 → 0.625 with a realistic ±0.1 CI) now demonstrates HOLD
(`test_m3_promote_holds_on_noisy_regressed_gate`,
`test_register_v6_adapter_annotates_hold`), and a new fixture with a tight,
CI-distinguishable regression demonstrates that the BLOCK path is still reachable
(`test_m3_promote_blocks_on_ci_distinguishable_regression`,
`test_register_v6_adapter_annotates_block`).

---
RESULT_tech-lead=READY — design verified against the actual promotion call graph (m3_promote.py:238 is the one true registry-write site, not register_v6_adapter.py), the real nightly sequence (stages after adapter_is_real.py always stop at "staged, not promoted" today), and the actual meaning of authorship_gap.py's `n` field (always 200, not a measured-sample-size signal) — all corrections and a dormant second promotion path (m3_outcome_driver.py --run-loop) are called out explicitly rather than assumed.
