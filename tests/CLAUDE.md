# tests/ — Test Suite

9,500+ C tests using a lightweight custom framework (`test_framework.h`).

## Framework

```c
#include "test_framework.h"

static void test_example_does_something(void) {
    HU_ASSERT_EQ(1 + 1, 2);
    HU_ASSERT_STR_EQ("hello", "hello");
    HU_ASSERT_NOT_NULL(ptr);
}

void run_example_tests(void) {
    HU_TEST_SUITE("example");
    HU_RUN_TEST(test_example_does_something);
}
```

## Rules

- Test names: `subject_expected_behavior` (e.g. `test_config_validate_null_returns_error`)
- Use `HU_IS_TEST` guards for side effects — no real network, no process spawning, no hardware I/O
- Free all allocations — ASan catches leaks
- Never assert tautologies like `HU_ASSERT_TRUE(1)` — use meaningful assertions
- Use neutral test data: `"test-key"`, `"example.com"`, `"user_a"` — no real credentials or PII

## Running Tests

```bash
./build/human_tests                          # run all tests
./build/human_tests --suite=JSON             # run suites matching "JSON"
./build/human_tests --filter=config_parse    # run tests matching "config_parse"
./build/human_tests --suite=security --filter=vault  # combine both
./build/human_tests --help                   # show all options
```

Use `scripts/what-to-test.sh <changed-files>` to auto-detect relevant suites for changed files (maps by directory).

For **header** changes, `scripts/predict-tests.sh <changed-files>` is more precise: it traces the reverse `#include` graph and selects the suites of the test files that actually reach the changed header (e.g. a leaf channel header → just the few suites that include it, not the whole `Channel` group). For a hot/central header it prints "RUN THE FULL SUITE" rather than a useless 200-suite filter. Advisory only — CI still runs the full suite. Smoke test: `bash scripts/test-predict-tests.sh`.

## Adding Tests

1. Create `tests/test_<module>.c` with `void run_<module>_tests(void)`
2. Add source file to `CMakeLists.txt` test target
3. Declare and call `run_<module>_tests()` in `tests/test_main.c`
