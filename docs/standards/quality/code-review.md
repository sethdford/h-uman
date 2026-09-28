---
title: Code Review Standards
---

# Code Review Standards

Standards for the PR review process: author preparation, reviewer priorities, and merge criteria.

**Cross-references:** [governance.md](governance.md), [ceremonies.md](ceremonies.md)

---

## Before Requesting Review

Author checklist (every PR):

- [ ] `cmake --build build && ./build/human_tests` passes (zero failures, zero ASan errors)
- [ ] Changes scoped to a single concern (not a mixed feature + refactor + infra patch)
- [ ] No debug logging, stale `TODO` hacks, or dead code left in
- [ ] New/changed vtable interfaces documented in the relevant `docs/api/*.md`
- [ ] `HU_IS_TEST` guards added for any new side effects (network, filesystem, hardware)
- [ ] Every allocation has a corresponding `free()` call

---

## Review Priorities

Reviewers focus in this order. Higher priorities block merge; lower priorities can be addressed in follow-ups if clearly tracked.

### 1. Correctness

- Does the code do what it claims?
- Are edge cases handled? What happens with NULL input, empty strings, zero-length arrays?
- Are vtable function pointers checked before calling?
- Is the ownership model clear -- who allocates, who frees?

### 2. Security

- No secrets in code (API keys, tokens, credentials)
- Input validated and sanitized before use
- Security policy respected for tool execution and file access
- `HU_IS_TEST` guards prevent real network/process/hardware access in tests
- HTTPS-only for outbound connections
- See `docs/standards/security/threat-model.md` for the full threat surface

### 3. Standards Compliance

- Follows relevant standards from `docs/standards/`
- Naming conventions: `snake_case` for identifiers, `hu_<name>_t` for types, `HU_SCREAMING_SNAKE` for constants
- Architecture boundaries respected (no cross-subsystem coupling)
- Design tokens used for any UI changes (no raw hex, pixel spacing, or pixel radii)

### 4. Quality Governance

- No duplication introduced (code, docs, or config)
- Agent files (`CLAUDE.md`, `AGENTS.md`, `.cursor/rules/`) updated if needed
- Documentation current -- no orphaned or stale docs introduced
- New `.md` files indexed in appropriate README
- See [governance.md](governance.md) for the full checklist

### 5. Maintainability

- Code is clear without comments explaining the obvious
- No unnecessary abstractions (KISS/YAGNI)
- Error paths are explicit and localized (`HU_ERR_*` codes, not silent failures)
- Tests cover behavior and edge cases, not just happy paths
- No dead code, unused includes, or orphaned functions

### 6. Performance

- No unnecessary allocations in hot paths
- String operations use appropriate buffer sizes (no repeated `realloc` in loops)
- Binary size impact considered for new features
- Memory footprint considered: `human --version` peak RSS is 6.9 MB against the 8 MB hard limit in
  [performance.md](../engineering/performance.md); idle RSS (`human mcp`) is 8.6 MB. Both measured
  2026-09-27 on the release-size build, macOS arm64 ([footprint.json](../../perf/footprint.json))

---

## Risk-Adjusted Review Depth

Review depth scales with the risk tier of the changed files:

| Risk Tier | Paths                                                                                           | Review Depth                                        |
| --------- | ----------------------------------------------------------------------------------------------- | --------------------------------------------------- |
| Low       | `docs/`, comments, test additions, formatting                                                   | Quick scan; focus on accuracy                       |
| Medium    | Most `src/` behavior changes                                                                    | Standard review; all 6 priorities                   |
| High      | `src/security/`, `src/gateway/`, `src/tools/`, `src/runtime/`, vtable interfaces, config schema | Deep review; threat modeling; failure mode analysis |

When uncertain about risk tier, classify as higher.

---

## PR Description

Every PR includes:

1. **What** -- one sentence on the change
2. **Why** -- motivation or linked issue
3. **How to test** -- steps to verify the change works
4. **Risk** -- which risk tier and why
5. **Screenshots** -- for any UI change

Format follows conventional commits: `<type>[(<scope>)]: <description>`

---

## Merge Criteria

All must be true:

- Build succeeds with zero warnings
- All tests pass with zero ASan errors
- At least one review approval
- No unresolved review threads
- PR description complete
- Conventional commit format verified

---

## Anti-Patterns

```
WRONG -- "LGTM" with no specific review comments
RIGHT -- Comment on at least one substantive aspect, even if approving

WRONG -- Review only the diff without understanding the surrounding module
RIGHT -- Read the module context; check how the change fits the vtable contract

WRONG -- Approve a security-tier change with a quick scan
RIGHT -- High-risk paths get deep review with failure mode analysis

WRONG -- Block a PR for style preferences not covered by standards
RIGHT -- Only block for standards violations, correctness, or security issues

WRONG -- Let review threads go stale
RIGHT -- Resolve or escalate within 24 hours
```
