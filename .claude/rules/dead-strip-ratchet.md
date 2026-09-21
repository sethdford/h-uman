# Dead-Strip Ratchet — The Linker Is the Oracle, and Its Two Counters May Only Fall

> **Auto-locked from the start.** Both baselines are rewritten downward by
> `ratchet_autolock` (`scripts/lib/ratchet.sh`) the next time the gate runs from
> the pre-commit hook, and a decay target derived from each counter's own history
> is reported weekly. See `.claude/rules/ratchet-decay.md`.

Two counters, both measured by relinking the real binary, both frozen at a
baseline and allowed only to **decrease**:

| Counter | What it counts | Baseline (2026-09-21) | Floor |
|---|---|---:|---:|
| **A** — never-loaded archive members | objects in `libhuman_core.a` that the linker never saw a single global symbol of, live *or* dead-stripped. A whole translation unit nobody links. | 50 | 0 |
| **B** — unreferenced dead symbols | `_hu_` symbols the linker dead-stripped out of `libhuman_core.a` members that no `human_tests` object references either. Dead in the product **and** unpinned by a test. | 99 | 20 |

Floors come from `docs/plans/2026-09-20-dead-code-plan.md` §6 ("A = 0 after P5,
B < 20"), not from a number invented here.

## The hazard

The September 2026 audit found ~27K LOC of code that compiles, archives, and is
never linked: 125 never-linked non-channel files, 105 functions with zero
callers. None of it was detectable from any single commit — each one looked
fine on its own — and six of those orphan modules had been *edited* by later
sweeps, which is exactly the Eder et al. (ICSE 2012) result reproducing here:
7.6% of maintenance changes touch unused code, and about half of those changes
are wasted. For an agent session the cost is worse: every dead module is a
false localization target.

A one-time purge does not hold. Lehman's first law says complexity grows back
unless work is spent holding it down, so the purge needs a fence.

## Why grep is the wrong oracle

❌ **"Grep for callers of each function."** A caller inside another dead
function is not a caller. Reachability is transitive, and grep cannot see it.

❌ **"Trust `check-untested.sh` / a dead-code linter."** Those answer "is there
a reference somewhere in the tree", which is the question that let the 27K LOC
accumulate in the first place.

The linker already computes exact reachability from `main` in order to emit the
binary — `-dead_strip` *is* the analysis. The gate just asks it for the answer:
relink `human` from `build/CMakeFiles/human.dir/link.txt` with
`-Wl,-dead_strip -Wl,-map`, and read the map.

## Measurement details that are easy to get wrong

Three of these were measured mistakes before the gate was correct:

1. **Attribute by SYMBOL NAME, never by archive-member basename.** The map
   lists members as `libhuman_core.a(dispatch.c.o)` — a basename. This tree has
   ~90 duplicated basenames across layers (two `dispatch.c`, two `client.c`,
   two `context.c`), so basename attribution silently merges unrelated modules.
   The object universe comes from `human_core.dir/link.txt`; liveness is tested
   per object through `nm -g --defined-only`.
2. **A member with no code is not a never-loaded module.** 56 of the 1,024
   members export no global `T` symbol at all — the embedded
   `data_prompts_*_txt.c.o` blobs, and `outbound/{strip,shape,echo,persona,
   moderation}.c.o`, which export only `D`/`S` stage tables. Requiring ≥1 `T`
   symbol excludes them; without that rule A is inflated by 56 phantom hits.
3. **Common symbols (`nm` type `C`) must be excluded from the liveness test.**
   Under ASan every object defines `____asan_globals_registered` as a common
   symbol and the linker coalesces them into one map entry, so a by-name
   membership test marks all 1,024 members live and collapses A to 0.

**B counts symbols, not functions.** At the baseline, B = 40 whole function
symbols + 59 function-local statics (`_hu_fn.DEFAULT_LEN`, `_hu_fn.sql`), which
the compiler emits as separate symbols. Statics move with the function that owns
them, so they are correlated with the thing being measured rather than
independent noise — but do not read B as "99 dead functions". The FAIL output
prints every name.

## The rule

A commit may not increase A or B. Two legitimate responses to a failure:
delete the object / symbol, or give it a real caller on a live path (defining a
function and unit-testing it in isolation is not wiring — see
`.claude/rules/ground-truth-over-proxy-signals.md`). As deletion work lands,
auto-lock captures the gain; no manual baseline edit is needed.

## Enforcement, and the honest limits

`scripts/check-dead-strip-ratchet.sh`, wired into `.githooks/pre-push` (not
pre-commit: it needs a built tree). ~1 s warm — 0.1 s to relink, the rest `nm`
over 1,024 archive members and ~2,000 test objects, with the test-reference set
cached in `$TMPDIR` on the newest test `.o` mtime.

Unlike every other ratchet in `scripts/ratchet-config.tsv`, this one measures a
**build directory**, not source text. Three consequences, all deliberate:

- **The baselines are configuration-specific.** They were measured against the
  dev preset in `build/` (ASan, full feature set). `build-check` — what the
  pre-push hook itself builds — enables a different feature set and therefore
  compiles a different set of translation units, so its counts are *not*
  comparable to these constants. That is why the hook does not point
  `HU_BUILD_DIR` at the tree it just built.
- **The gate skips rather than guesses.** No `build/`, no link map, not macOS,
  `nm` unable to read every member → it prints `RATCHET_SKIP: <reason>` and
  exits 0. A gate that cannot measure must not block a push
  (`.claude/rules/no-number-without-a-measurement.md`), and
  `scripts/ratchet-debt-report.sh` reads that marker so a skip is shown as
  "skipped", not INCONCLUSIVE.
- **A stale `build/` demotes the gate to advisory.** If any `src/` or
  `include/` header is newer than `build/libhuman_core.a`, the counts describe
  an earlier tree, so they are printed and the gate exits 0. **Keep `build/`
  current if you want this gate to bite** —
  `cmake --build build --target human human_tests`.

Linux CI is a documented gap: the map parser is macOS `ld`'s
(`# Object files:` / `# Symbols:` / `# Dead Stripped Symbols:`). GNU `ld -Map`
is a different format; on any non-Darwin host the gate skips.

## Related

- `docs/plans/2026-09-20-dead-code-plan.md` — §6 is this gate's specification;
  Appendix A is the inventory it was built to fence.
- `.claude/rules/clone-ratchet.md` — the sibling ratchet this is modelled on.
- `.claude/rules/ratchet-decay.md` — auto-lock (hard) and decay targets
  (weekly, advisory), the mechanism both baselines participate in.
- `.claude/rules/no-number-without-a-measurement.md` — why the gate skips loudly
  instead of reporting a number it did not measure.
