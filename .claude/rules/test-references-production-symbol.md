# Rule: Test Files Must Reference Production Symbols

## Trigger

Fires when a file matching `tests/test_*.c` is created or modified (PostToolUse:Write/Edit).

## Intent

Prevent the "test inlines production code" anti-pattern: a test file that reimplements
the logic it claims to test instead of calling the real production function.  This
anti-pattern silently passes the test suite while leaving the production code path
entirely uncovered.

Recurring failure mode in Sprint 3 (two instances): test files were written with local
helper functions that duplicated validator chain logic without importing or calling
`hu_output_validator_chain_execute`.  The test passed; the production path was not
exercised.

## Rule

Any new or modified file matching `tests/test_*.c` must contain at least one
grep-detectable reference to a `hu_*` function or macro exported from the production
`.c` file implied by the test file name.

Module-name heuristic: `tests/test_daemon_e2e_validator.c` → tries `daemon_e2e_validator`,
then `daemon_e2e`, then `daemon` as the candidate production module.  The first match in
`src/**/<candidate>.c` wins.

## Escape hatch

Add a line containing exactly `// @covers-none` to the test file to suppress this check.
Use this only for genuine standalone tests (testing header-only inline functions, or
integration tests that span multiple production modules with no single implied source
file).

## Enforcement

The check is enforced by two layers:

1. **Pre-commit hook** — `scripts/check-test-references.sh` is wired into
   `.githooks/pre-commit`.  It checks all staged `tests/test_*.c` files on every
   `git commit`.  Bypass requires `git commit --no-verify` (acceptable as deliberate
   override; CI enforces harder).

2. **This rule file** — instructs Claude Code agents: when writing or editing a
   `tests/test_*.c` file, verify the file references at least one symbol from the
   implied production module before marking the task complete.

## Script reference

`scripts/check-test-references.sh` — accepts explicit file paths or auto-detects staged
files.  Exits 0 if all checked files pass, exits 1 with a named missing symbol if any
fail.  Run `scripts/check-test-references.sh --help` for full usage.

## Examples

Bad (triggers the rule):
```c
/* tests/test_validator_chain.c */
static int local_validate(const char *s) { ... }  /* reimplemented production logic */
static void test_validation(void) { local_validate("x"); }
```

Good (satisfies the rule):
```c
/* tests/test_validator_chain.c */
#include "human/output_validator.h"
static void test_chain_rejects_f2_leak(void) {
    hu_output_validator_chain_t *chain = NULL;
    hu_validators_build_default_outbound_chain(&alloc, NULL, 0, &chain);
    /* ... exercises real production code ... */
}
```

Good (standalone test, opted out):
```c
/* tests/test_string_helpers.c */
// @covers-none — tests static inline helpers in include/human/string_utils.h
static void test_hu_strndup_null_safe(void) { ... }
```
