---
paths: src/**/*.c, src/**/*.h, include/**/*.h, tests/**/*.c, CMakeLists.txt, **/*.cmake
---

# CMake "Built target" Lies: Touch Source Before Rebuilding the Production Binary

When you edit a source file under `src/` that is consumed by BOTH the
production binary (`human`) AND the test binary (`human_tests`) via two
different static-library targets, `cmake --build build -j8` can report
`[100%] Built target human` while the production binary still contains
the OLD object file.

## The hazard

`src/CMakeLists.txt` builds `libhuman_core.a` (for `human`) and
`libhuman_core_test.a` (for `human_tests`) from largely overlapping
source sets. When you edit `src/foo.c`:

1. cmake invalidates `foo.c.o` in BOTH libraries
2. `human_tests` rebuilds `foo.c.o` for `libhuman_core_test.a` immediately
   (because it was just built)
3. `human`'s link stamp doesn't always pick up the change — the executable
   may not relink even though the underlying static lib has a newer object

The result: tests pass against the new code (they linked against
`libhuman_core_test.a` which DID rebuild), but `./build/human` runs the
OLD code. Symptoms include:

- "I wired this feature but the daemon log doesn't show it"
- "The new symbol is in the .o file but `nm build/human` doesn't see it"
- "Tests pass but the binary clearly hasn't been updated"

Real instance (2026-05-24 trusting-lamport session): Sprint A.6
daemon-side `hu_daemon_social_tick` wire appeared in source and was
verified by tests, but the running daemon kept emitting the prior
behavior. `nm build/human | grep daemon_social_tick` showed the symbol
present — but at the wrong offset, pinning the OLD compiled version.
Burned ~30 min diagnosing "loaded but not loaded" before finding the
stale-binary trap.

## Why the obvious fix is wrong

- ❌ `cmake --build build --target human -j8` alone — same trap; cmake
  thinks the target is up to date because the link stamp is fresh
  relative to the LIBRARY stamp, even though the OBJECT inside the
  library is newer
- ❌ `rm build/human && cmake --build build -j8` — works, but wasteful:
  forces a full relink even when only one .o changed
- ❌ `cmake --build build --clean-first` — way too heavy; rebuilds
  every TU in the project

## The right fix

`touch` the source file you just edited BEFORE running cmake. This
updates the source file's mtime, which forces cmake to mark every
downstream target (object + library + executable) as stale and rebuild
the link:

```bash
touch src/daemon.c && cmake --build build --target human -j8
```

For multi-file edits:

```bash
touch src/daemon.c src/daemon_social_tick.c src/agent/reaction_handler.c \
  && cmake --build build --target human -j8
```

This is fast (only the touched TUs + the link step run) and reliable
(the link is forced by mtime, not deduced).

## When this rule applies

Apply this pattern whenever:

- You edit a source file under `src/` AND want to test the change
  against the production binary `./build/human` (NOT just the test
  binary `./build/human_tests`)
- You ran `cmake --build build -j8` and the output says
  `[100%] Built target human` AND
  the `human` executable was either (a) not relinked at all (mtime
  unchanged) or (b) signed but doesn't contain your changes
- You see "Linking C executable human" + "Signing human binary"
  output — if this DOESN'T appear, the executable wasn't relinked
  and your change isn't in the binary

## Detection signal

Always read cmake's output for these two lines together when you
expect a production-binary rebuild:

```
[100%] Linking C executable human
Signing human binary with Human Local Dev certificate
```

If you see only `[100%] Built target human` without those two lines,
the executable was NOT relinked. The .a library may have new objects;
the executable still has the old ones.

## Quick smoke test for "did my change actually land"

After a rebuild, before re-running the daemon:

```bash
# Confirm the new symbol exists at the expected line/offset:
nm build/human | grep <new_symbol_name>

# Or just diff binary mtime against your source edit:
stat -f "%m %N" build/human src/<edited_file>.c
```

If `build/human`'s mtime is OLDER than `src/<edited_file>.c`, the
production binary is stale. Run `touch <source> && cmake --build build
--target human -j8`.

## Related

- `~/.claude/CLAUDE.md` "Verify, don't assert" — "Reading code is
  not verification." This rule is the operational corollary: building
  is not deployment.
- `.claude/rules/test-source-gate-symmetry.md` — the test/source-gate
  rule that prevents the test binary from skipping production paths;
  this rule prevents the production binary from skipping your edits.
- `~/.claude/rules/quality-gates.md` "Per-task gate" → "Behavior
  verification" — verify against the binary actually shipped, not the
  one cmake claims is current.
