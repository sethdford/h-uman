# Clone-Detection Ratchet — Duplication Windows May Only Decrease

The count of **duplicated code blocks** (6-line sliding windows appearing 2+ times in the src/ tree) is frozen at a baseline and may only **decrease**. This ratchet prevents the silent accumulation of copy-paste code that erodes the ability to refactor and maintain the codebase.

## The algorithm

The ratchet uses a **line-content sliding window** scanner, not a sophisticated token-based tool:

1. For each `src/**/*.c` file (excluding `third_party/`, `vendor/`, and `*_generated.c`):
2. Normalize lines: strip leading/trailing whitespace, collapse internal whitespace, drop blank lines and comment-only lines
3. Build overlapping windows of 6 consecutive normalized lines
4. Count which windows appear 2+ times (duplicated blocks)
5. Report the count of distinct duplicated windows

**Why 6-line windows:** Window size trades off between catching meaningful duplication and avoiding false positives on trivial common patterns (`if (!x) return;`, loop structure, etc.). 6 lines catches real function/logic duplication; 3-line windows produce too much noise.

**Speed:** Full scan of ~1,071 source files completes in ~5 seconds.

## The hazard (cumulative creep)

Copy-paste code starts small: one module reuses logic from another without a shared helper. The copies drift over time (different bug fixes applied to one but not others), and refactors become *locally* safe ("this file's version works") but miss the other copies. The code smell is undetectable from commits (each looks fine alone). Without a ratchet, the duplication grows unbounded.

Baseline at Phase E0 (2026-05-31): **11,766 duplicated windows** across the src tree.

## The rule

New commits must not increase the count of duplicated windows. As refactoring phases extract shared code, lower the `CLONE_BASELINE` constant to lock the gain — the ratchet only tightens.

Genuine dedup work (moving common patterns into shared helpers) is the path to lowering the baseline. E3 and later phases drive this.

## Enforcement

`scripts/check-clone-ratchet.sh`, wired into `.githooks/pre-commit` (fires when any `src/` C file is staged). Exits 0 if the current duplication count ≤ baseline; exits 1 if count increased.

## Note: Phase relocations may transiently increase duplication

E1–E4 refactor phases move files between directories and may temporarily change which lines are adjacent or spread across files. A file move that splits a function into separate files can increase the window-duplication count (the function body now appears in two places: the original + a partial copy during the refactor). These transient increases are expected and non-blocking *during* the chip. The ratchet is checked **post-chip**, after refactors settle. Lower the baseline once the refactor merges and the new code structure is stable.

## Related

- `~/.claude/rules/sqlite-includer-ratchet.md` — the canonical ratchet pattern this models
- `docs/plans/2026-05-29-ddd-bounded-contexts/` — the refactor phases that drive dedup over time
- `~/.claude/rules/quality-gates.md` — "No silent failures"; this is its structural form for code duplication
