---
title: Testing Standards
---

# Testing Standards

Standards for writing, organizing, and validating tests across the human runtime.

**Cross-references:** [principles.md](principles.md), [anti-patterns.md](anti-patterns.md), [../quality/governance.md](../quality/governance.md)

---

## Framework

- Custom C test framework in `tests/test_framework.h`
- Run: `./build/human_tests` (full suite) or `./build/human_tests --suite=<name>` (targeted)
- AddressSanitizer enabled in dev builds -- catches leaks, overflows, use-after-free

## What to Test

| Priority | What                                   | Example                                                       |
| -------- | -------------------------------------- | ------------------------------------------------------------- |
| Must     | Vtable wiring and factory registration | Provider creates, channel connects, tool executes             |
| Must     | Error paths and edge cases             | NULL input, empty strings, zero-length arrays, malformed JSON |
| Must     | Security boundary behavior             | Policy enforcement, sandbox limits, HTTPS-only                |
| Must     | Memory correctness                     | Every allocation freed, no dangling pointers                  |
| Should   | Config parsing and validation          | Schema enforcement, defaults, missing fields                  |
| Should   | Utility functions                      | String helpers, JSON parsing, base64, URL encoding            |
| Should   | Integration between subsystems         | Agent loop with mock provider, tool dispatch with mock tools  |
| May      | Performance characteristics            | Startup time, RSS, throughput (via benchmark suite)           |

## Test Structure

```c
// Arrange-Act-Assert pattern
void provider_returns_error_on_null_input(void) {
    // Arrange
    hu_provider_t provider;
    hu_provider_create(&provider, "mock", NULL);

    // Act
    hu_error_t err = hu_provider_chat(&provider, NULL);

    // Assert
    HU_ASSERT_EQ(err, HU_ERR_INVALID_PARAM);

    hu_provider_destroy(&provider);
}
```

## Naming

- Test files: `tests/test_<module>.c`
- Test functions: `subject_expected_behavior` (e.g., `config_parse_rejects_invalid_json`)
- Describe the behavior being tested, not the implementation detail
- Test functions read as sentences when prefixed with "test that": "test that config parse rejects invalid json"

## Test Quality Rules

Passing tests alone is not enough. Tests must:

1. **Test behavior, not implementation** -- assert on observable outcomes, not internal state
2. **Cover edge cases and error paths** -- not just happy paths
3. **Use meaningful assertions** -- never `HU_ASSERT_TRUE(1)` or tautological conditions
4. **Be deterministic** -- no time-dependent logic without mocks, no system-state dependencies
5. **Clean up after themselves** -- every allocation freed, temp files removed
6. **Use neutral fixtures** -- `"test-key"`, `"example.com"`, `"user_a"` -- no personal data

## Mocking and Isolation

- Mock at boundaries: providers, network, filesystem, hardware
- Use `HU_IS_TEST` guards for side effects (network, process spawning, hardware I/O)
- Never mock the unit under test
- Tests must not spawn real network connections, open browsers, or depend on external services
- Tests must be reproducible across macOS and Linux

```c
// WRONG -- test depends on real network
void test_provider_chat(void) {
    hu_provider_t p;
    hu_provider_create(&p, "openai", real_config);  // hits real API
    ...
}

// RIGHT -- test uses mock or HU_IS_TEST guard
void test_provider_chat(void) {
    hu_provider_t p;
    hu_provider_create(&p, "mock", NULL);  // mock provider
    ...
}
```

## Coverage Expectations

The human runtime tracks test coverage through test count and module coverage rather than line-level instrumentation:

| Metric              | Expectation                                              |
| ------------------- | -------------------------------------------------------- |
| Total test count    | Growing; currently 9,500+                                 |
| Module coverage     | Every `src/` module has a corresponding `tests/test_*.c` |
| Error path coverage | Every vtable function has at least one error-path test   |
| ASan clean          | Zero AddressSanitizer errors across the full suite       |

Use `scripts/check-untested.sh` to find modules without test coverage.

## Verification Gate

Before any commit:

```bash
cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./build/human_tests
```

For faster iteration:

```bash
./build/human_tests --suite=<name>        # targeted suite
scripts/agent-preflight.sh                # change-aware validation
scripts/what-to-test.sh <changed-files>   # find relevant suites
```

All tests must pass. Zero ASan errors. No exceptions.

---

## Anti-Patterns

```
WRONG -- Test only the happy path
RIGHT -- Include NULL input, empty strings, boundary values, and error returns

WRONG -- Assert that the function "does not crash" (HU_ASSERT_TRUE(1))
RIGHT -- Assert on specific return values, output content, and state changes

WRONG -- Test implementation details (check internal struct fields)
RIGHT -- Test through the public API (vtable function pointers)

WRONG -- Skip cleanup because "ASan will catch it later"
RIGHT -- Free every allocation in every test; ASan is a safety net, not a substitute

WRONG -- Depend on test execution order
RIGHT -- Each test is independent; setup and teardown in every test function
```

## Normative References

| ID            | Source                       | Version | Relevance                                  |
| ------------- | ---------------------------- | ------- | ------------------------------------------ |
| [TestPyramid] | Martin Fowler — Test Pyramid | 2012    | Unit > integration > E2E ratio guidance    |
| [GoogleTest]  | Google Testing Blog          | Ongoing | Testing best practices for large codebases |
| [CERT-C]      | SEI CERT C Coding Standard   | 2016    | Security-relevant test requirements        |
| [ASan]        | Google AddressSanitizer      | LLVM 18 | Memory error detection methodology         |
