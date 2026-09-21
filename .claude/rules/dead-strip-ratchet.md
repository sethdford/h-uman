# Dead-Strip Ratchet — The Linker Is the Oracle, and Its Two Counters May Only Fall

> **Auto-locked from the start — precisely: a gain locks on your next commit
> that stages a `src/` change with `build/` freshly built.** `ratchet_autolock`
> (`scripts/lib/ratchet.sh`) only rewrites a baseline when it runs from
> `.githooks/pre-commit` (`HU_RATCHET_FROM_HOOK=1`), which is why this gate is
> wired into *both* hooks: pre-commit for the lock, pre-push for the
> enforcement. A decay target derived from each counter's own history is
> reported weekly. See `.claude/rules/ratchet-decay.md`.

Two counters, both measured by relinking the real binary, both frozen at a
baseline and allowed only to **decrease**:

| Counter | What it counts | Baseline (2026-09-21) | Floor |
|---|---|---:|---:|
| **A** — never-loaded archive members | objects in `libhuman_core.a` that the linker never saw a single global symbol of, live *or* dead-stripped. A whole translation unit nobody links. | 50 | 0 |
| **B** — unreferenced dead symbols | `_hu_` symbols the linker dead-stripped out of `libhuman_core.a` members that no `human_tests` object references either. Dead in the product **and** unpinned by a test. | 99 | 20 |

Floors come from `docs/plans/2026-09-20-dead-code-plan.md` §6 ("A = 0 after P5,
B < 20"), not from a number invented here.

Both counters read `libhuman_core.a`, which since Task 12 holds **daemon code
only**: the test/eval/SDK modules the daemon never links moved to the
`human_devlib` archive (see the `human_devlib` block in `CMakeLists.txt`), so a
never-loaded member of `human_core` now means dead code rather than "library
code with no daemon caller".

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

## Enforcement: two hooks, two jobs

`scripts/check-dead-strip-ratchet.sh` is wired into **both** hooks, because the
two things a ratchet must do live in different places:

| Hook | Fires when | What it does |
|---|---|---|
| `.githooks/pre-commit` | a `src/` `.c`/`.h` is staged (`ACMD`, like the clone ratchet) | runs the gate plain, so **`ratchet_autolock` can rewrite and stage a lowered baseline**. This is the *only* place that can happen: `scripts/lib/ratchet.sh` refuses to rewrite unless `HU_RATCHET_FROM_HOOK=1`, which only pre-commit exports — pre-push cannot, since its `git add` would stage a file into no commit. Skips in one line when `build/` is absent or stale. |
| `.githooks/pre-push` | every push, after the suite passes | **rebuilds `build/` incrementally** (`cmake --build build --target human human_tests`), then runs the gate with `HU_DEAD_STRIP_STRICT=1`. This is the enforcement point. |

Without the pre-commit half the baselines could only ever freeze, never ratchet
down — the exact pathology `.claude/rules/ratchet-decay.md` exists to prevent.
Without the pre-push rebuild the enforcement half would wave through most real
pushes, because pre-push builds `build-check` while the gate measures `build/`,
which would therefore usually be stale and self-demote (see below).

Cost: ~1 s warm — 0.1 s to relink, the rest `nm` over 1,024 archive members and
~2,000 test objects, with the test-reference set cached in `$TMPDIR` on the
newest test `.o` mtime. Plus the pre-push incremental rebuild, which is seconds
on a `build/` the developer was already using.

## The honest limits

Unlike every other ratchet in `scripts/ratchet-config.tsv`, this one measures a
**build directory**, not source text. Three consequences, all deliberate:

- **The baselines are configuration-specific.** They were measured against the
  dev preset in `build/` (ASan, full feature set). `build-check` — what the
  pre-push hook itself builds for the test suite — enables a different feature
  set and therefore compiles a different set of translation units, so its counts
  are *not* comparable to these constants. That is why neither hook points
  `HU_BUILD_DIR` at `build-check`.
- **The gate skips rather than guesses.** No `build/`, no link map, not macOS,
  `nm` unable to read every member, `nm -u` failing over the test objects → it
  prints `RATCHET_SKIP: <reason>` and exits 0. A gate that cannot measure must
  not block a commit or a push
  (`.claude/rules/no-number-without-a-measurement.md`), and
  `scripts/ratchet-debt-report.sh` reads that marker so a skip is shown as
  "skipped", not INCONCLUSIVE.
- **A stale `build/` demotes the gate to advisory — except under
  `HU_DEAD_STRIP_STRICT=1`.** If any `src/` or `include/` file is newer than
  `build/human` or `build/human_tests`, the counts describe an earlier tree, so
  they are printed and the gate exits 0. This is the right answer for an ad-hoc
  run and for pre-commit (which must not block a commit on a tree nobody built).
  It is the *wrong* answer for pre-push, which is why pre-push rebuilds first
  and then sets `HU_DEAD_STRIP_STRICT=1` to disable the demotion. For a manual
  run that you want to bite: `cmake --build build --target human human_tests`
  first, or pass `HU_DEAD_STRIP_STRICT=1` yourself.

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
