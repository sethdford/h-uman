# Ratchets Must Decay, Not Just Freeze — Auto-Lock Hard, Decay Advisory

A fail-on-growth ratchet holds the level and never lowers it. This repo had
eight of them and they had converged to "never worse, never better." Every
ratchet now (1) **auto-locks** any gain the moment a gate runs, and (2) carries
a **decay target** derived from its own history, reported weekly — never
enforced in pre-commit.

## The hazard (measured 2026-07-27)

Every architecture counter was sitting at or within a rounding error of its
ceiling, and had been for weeks:

| Counter | Value | Ceiling | Last moved |
|---|---:|---:|---|
| `src/daemon.c` LOC | 14,132 | 14,132 | May |
| sqlite3.h includers | 97 | 97 | 8.1 weeks |
| loose `src/*.c` | 4 | 4 | 8.1 weeks |
| `agent/` factory includes | 4 | 4 | no move in 180d |
| clone groups | 11,507 | 11,515 | 8 units of *unlocked* slack |

Two distinct failures produced that table:

1. **Gains leaked back as slack.** Each script printed *"NOTE: count dropped —
   lower BASELINE to lock the gain."* That manual step was inconsistently
   followed, so the clone ratchet carried 8 units of headroom a future commit
   could spend without ever tripping the gate.
2. **Nothing ever asked for a reduction.** Growth was refused; stasis was
   rewarded identically to improvement. `daemon.c` sat unchanged for two months
   with every gate green.

## Why the obvious fix is wrong

❌ **"Make the decayed target fail pre-commit."** These gates block the merge
queue (`ci-required-checks.md`). A calendar-driven target fails commits that
changed nothing relevant — exactly the gate shape `session-worktree-isolation.md`
records as one that "would have blocked the same day's three urgent red-CI
fixes." Debt pressure must not be able to block an unrelated hotfix.

❌ **"Pick a rate — say 25 clone groups a week."** A number nobody measured, and
`no-number-without-a-measurement.md` forbids exactly that. It is also wrong per
counter: 25/week is trivial for clones and impossible for loose root files.

❌ **"Velocity = (first − last) / elapsed."** Tried and discarded. It is
dominated by step changes: `root-files` fell 101 → 4 in essentially one
refactor, which that formula reported as a sustained **3,198 units/week** and
drove every target instantly to its floor.

## The design

**Auto-lock — hard, in pre-commit, no calendar.** `scripts/lib/ratchet.sh`
`ratchet_autolock` rewrites the baseline constant to the measured value whenever
a counter drops, and stages it. It can only tighten, so it can never block a
commit. It refuses to fire when the gate file has unstaged edits (staging it
would sweep in-flight work into someone else's commit), when the measurement is
non-numeric, or under `HU_RATCHET_NO_AUTOLOCK=1`. All twelve of those paths are
pinned by `scripts/test-ratchet-lib.sh`.

**Decay target — advisory, weekly, derived.** `scripts/ratchet-debt-report.sh`
fits each counter's rate from its **own** baseline history:

- **median of per-drop rates**, not total-over-elapsed, so one large carve is a
  single sample rather than the whole trend;
- each inter-drop interval floored at one week, so two same-day commits cannot
  imply an unbounded rate;
- scaled by `RATCHET_AGGRESSION` (default **0.5**) — a target only ever asks for
  half of what this repo has already demonstrated *on that counter*;
- anchored to when the baseline **last moved**, so pressure accrues with
  staleness rather than on a fixed calendar. Improve something and it has no
  target pressure the next day;
- any single ask capped at `RATCHET_MAX_STEP_FRAC` (default **5%**) of baseline,
  so a long-idle counter gets a payable ask instead of an unpayable jump to its
  floor;
- the target rounds **up**, so a fractional ask never becomes a whole unit of
  debt.

Counters with fewer than two baseline moves in the window report *"no proven
velocity yet"* rather than inventing one.

**Exit codes are a contract.** `0` on target · `1` over target (the weekly
workflow opens/updates one issue) · `2` a counter could not be measured. Exit 2
is separate on purpose: an unmeasurable counter must never be rendered as zero
debt.

## Configuration

One row per counter in `scripts/ratchet-config.tsv`: `name`, `script`, `var`,
`floor`, `rate` (`auto` | `off`), `rule`, `pattern`. `floor` uses the target
documented in that counter's own rules file, or `none` where the rules file
states no endpoint — inventing one would be a number without a measurement.
`rate=off` is a legitimate answer (`edge-cross-channel`'s 6 cross-channel
includes are documented as grandfathered, not debt).

**Disagreeing with a target is a supported action.** Set `rate=off` or raise the
`floor`, and say why in the file. Recording the reason is the point; a target
nobody agrees with should be changed, not ignored.

## When this applies / does NOT

- **APPLIES** to any counter-with-a-baseline gate in `scripts/check-*.sh`.
  Adding one means adding its config row.
- **DOES NOT apply** to boolean/structural gates with no counter
  (`check-test-source-gate-symmetry.sh`, `check-test-references.sh`), or to
  counters already at their floor (`MEMCMP_BASELINE=0`, the modeled-person
  layering baselines) — those are freeze-only by design and correctly so.

## Related

- `.claude/rules/clone-ratchet.md`, `file-size-ceiling.md`,
  `sqlite-includer-ratchet.md`, `no-new-root-files.md`,
  `agent-core-boundary.md`, `edge-context-isolation.md` — the individual gates;
  their "lower the baseline by hand" instructions are now handled by auto-lock.
- `.claude/rules/no-number-without-a-measurement.md` — why the rate is fitted
  rather than declared, and why exit 2 is not exit 0.
- `.claude/rules/ci-required-checks.md` — why decay is never a required check.
