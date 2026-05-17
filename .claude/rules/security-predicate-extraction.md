# Security Predicate Extraction

For any security decision that lives inside a hard-to-test boundary (a
forked child, a signal handler, a kernel sandbox callback, a one-shot
process replacement), **extract the decision into a pure predicate
function** that lives in the same translation unit but is callable from
tests without crossing the boundary.

## Why

Audit 2026-05-16 found that `src/tools/shell.c` was falling through to
bare `/bin/sh` when a configured sandbox failed to apply. The bug was
inside the forked child — directly unit-testing it would have required
spawning a real subprocess with a misconfigured sandbox, observing the
exit code, and parsing wait status. Slow, flaky, platform-dependent.

The fix factored the security decision into:

```c
bool hu_shell_must_deny_unsandboxed(
    const hu_security_policy_t *policy,
    bool apply_applied,
    bool wrap_attempted,
    bool wrap_succeeded);
```

The child still does the fork-and-exec; the predicate is called twice
(once in the child's decision logic, once from each unit test in
`tests/test_shell_sandbox.c`). The truth table is locked by 7 tests
without ever forking.

## When to apply

Use this pattern for any boundary where ALL of these are true:

- The decision is security-relevant (deny / allow / escalate).
- The decision depends on more than one input.
- The code that *acts* on the decision is hard to test
  (`_exit`, `execl`, `kill`, kernel syscalls, hardware writes).

## How

1. **Name the decision.** A predicate name reads like a question or an
   assertion: `must_deny_unsandboxed`, `should_escalate`, `is_allowed`.
2. **Inputs are facts, not pointers to mutable state.** The predicate
   takes booleans / enums / `const` pointers — never a mutable agent or
   policy that the caller's caller might modify. Reproducibility matters.
3. **Output is one of a small set.** `bool`, an enum with ≤ 5 variants,
   or `hu_error_t`. No structures.
4. **Publish the prototype in the module's public header.** Tests reach
   for it the normal way; reviewers see the contract at the boundary.
5. **The non-test code calls the same predicate** — no duplication
   between "what the test checks" and "what the code does."
6. **Test every line of the truth table.** If the predicate has N
   independent boolean inputs, write at least N+2 tests (each input
   isolated + a "nothing configured" case + a "both configured" case).

## Anti-patterns

- ❌ Inline-comment the decision and trust code review: "we deny here if
  wrap fails." Three audits later, someone "cleans up" the branch.
- ❌ Move the decision into the caller. Then every caller must enforce
  the contract — and they won't.
- ❌ Test the decision via end-to-end integration. Slow, flaky, and
  doesn't pin the decision; it pins the *consequence* of the decision.
- ❌ Make the predicate take a `hu_agent_t *` so it can read live state.
  The predicate is no longer reproducible; tests must construct full
  agents to exercise edge cases.

## Related

- `~/.claude/rules/quality-gates.md` — the per-task gate that demands
  edge-case tests.
- `docs/plans/2026-05-16-audit-followups/03-hook-pipeline-invocation.md`
  — uses this pattern to centralize tool dispatch + hook firing.
