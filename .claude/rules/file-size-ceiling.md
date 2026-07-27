# File-Size Ceiling Ratchet — No `src/*.c` File May Exceed the Current Largest

> **Auto-locked since 2026-07-27.** The "lower the baseline by hand" step below is
> now performed automatically by `ratchet_autolock` (`scripts/lib/ratchet.sh`) the
> next time this gate runs, and a decay target derived from this counter's own
> history is reported weekly. See `.claude/rules/ratchet-decay.md`.

The count of lines in the largest `src/*.c` file is frozen at a baseline and may
only **decrease**. This ratchet prevents the silent erosion of code organization
that let `src/daemon.c` grow ~830 LOC during a phase with no size gate.

## The hazard (T2 erosion)

In a prior iteration, `src/daemon.c` grew from 13,893 LOC to 14,723 LOC (a +830
LOC increase) over the course of a phase with no guardian gate. The growth was
incremental — each commit added ~50–100 LOC — and no single commit triggered
alarm. Without a ratchet, the largest file grows unbounded; refactors that
*shrink* god-files earn no automatic credit (the wall stays high for the next
committer).

Baseline at Phase E0 (2026-05-31): **14,723 LOC** (`src/daemon.c`).

## The rule

New `src/*.c` files must not cause ANY file to exceed the current maximum LOC
count. As each refactor phase **shrinks** the largest file (E2 is the daemon
carve-out phase), lower the `MAX_BASELINE` constant to lock the gain — the
ratchet only tightens.

The aspirational **target ceiling is 800 LOC per file**. This is not enforced
yet (it would fail 65 files today); it documents the long-term goal. Each phase
(E2, E3, E4) carves down the worst offenders and lowers `MAX_BASELINE` in
lockstep, so the floor rises monotonically toward the target.

## Enforcement

`scripts/check-file-size-ceiling.sh`, wired into `.githooks/pre-commit` (fires
when any `src/` C file is staged). The script computes the largest file by LOC,
compares it against `MAX_BASELINE`, and fails if any file exceeds the ceiling.

## When to lower MAX_BASELINE

After a successful refactor that shrinks the largest file:

1. Run `bash scripts/check-file-size-ceiling.sh` and observe the new largest size.
2. If it decreased (e.g., daemon.c went from 14,723 → 14,200), update the constant:
   ```bash
   # In scripts/check-file-size-ceiling.sh
   MAX_BASELINE=14200   # src/daemon.c, measured <date>
   ```
3. Commit the constant update alongside the refactor — the next phase's baseline
   is one commit behind the actual file size, locked at the moment the refactor
   landed.

## Related

- `docs/plans/2026-05-29-ddd-bounded-contexts/phase-E2-daemon-service-lifecycle.md` — the
  refactor phase that drives daemon.c down
- `~/.claude/rules/sqlite-includer-ratchet.md` — the canonical ratchet pattern;
  this file models its shape
- `~/.claude/rules/quality-gates.md` — "never add a new X, only lower the
  ceiling"; this rule is the structural form for file size
