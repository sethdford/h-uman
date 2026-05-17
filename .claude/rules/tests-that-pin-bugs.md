# Tests That Pin Bugs Are Worse Than No Tests

A test that asserts incorrect behavior is more dangerous than no test
at all — it provides false confidence, blocks the fix, and makes the
bug look intentional.

## The pattern from audit 2026-05-16

Two adversarial tests were named to suggest they caught a vulnerability:

- `test_permission_unknown_tool_defaults_danger`
- `test_mcp_tool_masquerading_permission_enforced`

But they actually asserted:

```c
HU_ASSERT_EQ(hu_permission_get_tool_level("mcp__evil__file_write"),
             HU_PERM_DANGER_FULL_ACCESS);
HU_ASSERT_TRUE(hu_permission_check(HU_PERM_DANGER_FULL_ACCESS, required));
```

…which **the test named "masquerading enforced"** confirms — that a
`DANGER_FULL_ACCESS` agent CAN invoke an unknown MCP tool. That is the
vulnerability the test name claims to prevent.

When the audit fix introduced `HU_PERM_DENY`, these four tests failed
loudly — and that failure was the only signal that the original tests
had been pinning a bug.

## Signals that a test pins a bug

- Test name describes the *intent* ("permission enforced", "rejects
  unsafe input") but the assertions accept a permissive outcome.
- Test was written immediately after a bug report but no related code
  change landed in the same PR.
- Test exercises a code path with comments like `/* HACK: */` or
  `/* TODO: tighten this */` nearby.
- Test asserts `HU_ASSERT_TRUE(some_check)` where `some_check` returns
  true because the input slipped past intended validation.

## How to avoid

1. **Test names are claims, not labels.** If you write
   `test_X_rejects_Y`, the test MUST fail when `Y` is accepted. Don't
   write a test whose name is aspirational.
2. **Adversarial tests should assert the dangerous case is BLOCKED.**
   Phrase assertions as `HU_ASSERT_FALSE(reaches_dangerous_state)` or
   `HU_ASSERT_EQ(result, HU_ERR_SECURITY_DENIED)`, not
   `HU_ASSERT_EQ(level, HU_PERM_DANGER_FULL_ACCESS)`.
3. **When fixing a bug, look for tests that LOCK the old behavior.**
   `git grep` for the old constant / function / behavior in the test
   tree. If a test asserts the buggy result, it must be updated AND
   you should add a comment explaining what the old assertion meant.
4. **Pair adversarial tests with positive contracts.** "Unknown tool
   returns DENY" + "DENY is strictly above DANGER" + "DANGER agent
   cannot satisfy DENY required" — together those triangulate the
   contract; alone, any one can lie.

## How CI catches this

The audit revealed these by running the FULL suite after the fix; the
targeted suite run was green. The lesson:

- Always run the full suite before claiming a security fix is done.
- Don't trust a green targeted run when the change touches a public
  contract — other suites may be pinning the old version.
- See `~/.claude/rules/quality-gates.md` per-task gate: "tests must
  pass (full suite, not just changed-files)."
