# tests/ — Test Suite

3,127+ C tests using a lightweight custom framework (`test_framework.h`).

## Framework

```c
#include "test_framework.h"

static void test_example_does_something(void) {
    SC_ASSERT_EQ(1 + 1, 2);
    SC_ASSERT_STR_EQ("hello", "hello");
    SC_ASSERT_NOT_NULL(ptr);
}

void run_example_tests(void) {
    SC_TEST_SUITE("example");
    SC_RUN_TEST(test_example_does_something);
}
```

## Rules

- Test names: `subject_expected_behavior` (e.g. `test_config_validate_null_returns_error`)
- Use `SC_IS_TEST` guards for side effects — no real network, no process spawning, no hardware I/O
- Free all allocations — ASan catches leaks
- Never assert tautologies like `SC_ASSERT_TRUE(1)` — use meaningful assertions
- Use neutral test data: `"test-key"`, `"example.com"`, `"user_a"` — no real credentials or PII

## Adding Tests

1. Create `tests/test_<module>.c` with `void run_<module>_tests(void)`
2. Add source file to `CMakeLists.txt` test target
3. Declare and call `run_<module>_tests()` in `tests/test_main.c`
