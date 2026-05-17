# Test/Source Gate-Symmetry Rule

When a source file `src/.../X.c` is registered in `CMakeLists.txt`
inside an `if(HU_ENABLE_FEATURE)` block, the matching test file
`tests/test_X.c` MUST be either:

1. **Registered inside the same `if(HU_ENABLE_FEATURE)` block** in
   CMakeLists.txt, AND its forward declaration + call site in
   `tests/test_main.c` MUST be wrapped in `#ifdef HU_ENABLE_FEATURE`
   matching the source's gate.

   ```cmake
   if(HU_ENABLE_FOO)
       list(APPEND HU_CORE_SOURCES src/foo.c)
       list(APPEND HU_TEST_SOURCES tests/test_foo.c)
   endif()
   ```

   ```c
   // tests/test_main.c
   #ifdef HU_ENABLE_FOO
   void run_foo_tests(void);
   #endif

   int main(void) {
       // ...
   #ifdef HU_ENABLE_FOO
       run_foo_tests();
   #endif
   }
   ```

   OR

2. **Use the internal-`#ifdef`-wrap-with-stub-runner pattern.** The
   test file's body is wrapped in `#ifdef HU_ENABLE_FEATURE`, and an
   `#else` branch provides an empty stub for the runner so the symbol
   still resolves at link time:

   ```c
   // tests/test_foo.c
   #ifdef HU_ENABLE_FOO

   #include "human/foo.h"
   // ... real test bodies ...

   void run_foo_tests(void) {
       HU_TEST_SUITE("foo");
       HU_RUN_TEST(test_foo_init);
       // ...
   }

   #else

   void run_foo_tests(void) { (void)0; }

   #endif
   ```

   This compiles to an empty stub when the flag is off, so the test
   source can stay in the unconditional `HU_TEST_SOURCES` list
   without breaking variant builds.

## Why this rule exists

This pattern is enforced by `scripts/check-test-source-gate-symmetry.sh`
and runs in the pre-commit hook and the docs CI gate. The script was
added after PR #113 hit the **same systemic failure six times** across
13 fix rounds — every instance was a new test file authored against the
default-everything-on dev preset that referenced symbols only defined
under an `if(HU_ENABLE_X)` block. The minimal/no-sqlite/no-skills/
cross-arm64 feature-flag CI variants caught it at link time:

```
tests/test_X.c: undefined reference to `hu_x_init'
```

Recurring examples this rule would have prevented:

| PR #113 round | Test file | Source gate | Missed in |
|---|---|---|---|
| 2 | `test_reaction_handler_e2e.c` | `HU_ENABLE_SQLITE` | minimal, no-sqlite, cross-arm64 |
| 9 | `test_authentic.c::test_life_thread_scoped_per_contact` | `HU_ENABLE_SQLITE` | same 3 variants |
| 10 (×2) | `test_ml.c`, `test_w13_learner.c` and 8 siblings | `HU_ENABLE_ML` / `HU_ENABLE_LEARNING` | 7+ variants |
| 12 | `test_dpo_miner.c` | `HU_ENABLE_ML` | 5 variants |
| 13 (LRU leak) | symmetric NULL-on-empty pattern | — | ASan |

Each instance produced 10–24 simultaneous CI failures that all collapsed
to a single source-test gate mismatch.

## When you ADD or MOVE a source/test pair

- If you put `src/foo.c` inside `if(HU_ENABLE_FOO)`, also gate
  `tests/test_foo.c` (either via CMake or via internal `#ifdef`).
- If you take `src/foo.c` OUT of a gate, also un-gate the test.
- If you wrap a single function in `if(HU_ENABLE_FOO)` inside an
  otherwise-unconditional source, EITHER the test for that function
  must be in the gate too OR the function body must compile to a stub
  in the disabled path (preferred — keeps the symbol resolvable).

## How to verify

```bash
bash scripts/check-test-source-gate-symmetry.sh
```

Exit code 0 = pass, 1 = mismatches found. The script reports each
mismatch with the test path, the test's CMake gate, the source path,
the source's gate, and the missing flag(s) that would cause a link
failure in a variant where those flags are off.

## Related

- `~/.claude/rules/ci-queue-triage.md` — the systemic-vs-per-PR
  triage pattern. Most gate-mismatch failures hit 5–15 jobs at once;
  fix once at the source-test pair, unblock the whole row.
- `scripts/check-untested.sh` — the orthogonal check for sources
  with no test references.
