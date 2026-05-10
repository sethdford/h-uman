---
title: Quality Governance
---

# Quality Governance

Every artifact in this repository -- code, documentation, configuration, tests -- must meet five non-negotiable standards.

**Cross-references:** [ceremonies.md](ceremonies.md), [code-review.md](code-review.md), [../ai/evaluation.md](../ai/evaluation.md)

---

## Principles

1. **Clean Code** -- readable, minimal, no dead code, no clever tricks. C11 with `-Wall -Wextra -Wpedantic -Werror`.
2. **Clean Architecture** -- vtable boundaries respected, single responsibility per module, dependency direction inward to contracts.
3. **Clean Documentation** -- every doc has a purpose, stays current, never duplicates another doc.
4. **Clean Context** -- agent files (`CLAUDE.md`, `AGENTS.md`, `.cursor/rules/`) stay synchronized with reality.
5. **No Duplication** -- DRY across code, docs, config, and agent instructions.

---

## The Gatekeeping Mindset

Every change goes through a mental audit before completion:

| Gate     | Question                                                                 |
| -------- | ------------------------------------------------------------------------ |
| Refine   | Is this the simplest version that solves the problem?                    |
| Refactor | Does this introduce duplication, dead code, or unnecessary complexity?   |
| Rework   | Does this drift from established patterns, standards, or architecture?   |
| Audit    | Does this pass all verification gates (build, tests, ASan, binary size)? |

If any answer is "no" or "not sure," the work is not done.

---

## Standards Alignment

### Drift Prevention

Standards drift is the gradual divergence between documented standards and actual practice. Prevent it:

- **Before writing code**: read the applicable standard in `docs/standards/`
- **After writing code**: verify the code matches the standard, not the other way around
- **When standards need updating**: update `docs/standards/` first, then propagate to agent files
- **Never ad-hoc**: if a pattern appears 3+ times without a standard, write the standard

### Propagation Order

When a standard changes, update in this order:

```
docs/standards/*.md          <- Source of truth (update here first)
    |
.cursor/rules/*.mdc          <- Cursor agent rules (reference, never duplicate)
    |
CLAUDE.md                    <- Quick reference for all agents
    |
AGENTS.md                    <- Full engineering protocol
```

Never update downstream files without updating the source standard first.

### No Orphaned Documentation

Every `.md` file in `docs/` must be:

- Indexed in the appropriate README or parent document
- Referenced by at least one agent file or standard
- Current with the actual state of the codebase

Orphaned documentation is worse than no documentation -- it creates false confidence.

---

## Code Quality Gates

### Build and Test Gate

Every piece of work must pass before claiming done:

```bash
cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./build/human_tests
```

Requirements:

| Gate        | Threshold                                                       |
| ----------- | --------------------------------------------------------------- |
| Compilation | Zero warnings (`-Wall -Wextra -Wpedantic -Werror`)              |
| Test suite  | All 9,500+ tests pass                                            |
| ASan        | Zero AddressSanitizer errors (leaks, overflows, use-after-free) |
| Memory      | Every allocation freed via `free()` or cleanup function         |

### Release Build Gate

For release-targeted changes, additionally verify:

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=MinSizeRel -DHU_ENABLE_LTO=ON
cmake --build build-release -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
```

| Gate          | Threshold                                      |
| ------------- | ---------------------------------------------- |
| Binary size   | No unexplained growth beyond ~1696 KB baseline |
| Clean compile | Zero warnings under release flags              |

### Test Quality

Passing tests alone is not enough. Tests must:

- Test behavior, not implementation details
- Cover edge cases and error paths, not just happy paths
- Use meaningful assertions (not just "does not crash")
- Be deterministic -- no flaky tests, no time-dependent logic without mocks
- Use `HU_IS_TEST` guards for side effects (network, filesystem, hardware I/O)
- Use neutral placeholders in fixtures (`"test-key"`, `"example.com"`, `"user_a"`)

---

## Documentation Standards

### Clean Documentation Rules

1. **One source of truth** -- every concept documented in exactly one place
2. **Actionable** -- every doc contains concrete rules with right/wrong examples
3. **Current** -- docs updated in the same PR as the code they describe
4. **Indexed** -- every standards doc referenced in `docs/standards/README.md`
5. **No essays** -- concise, scannable, table-driven where possible

### Agent File Maintenance

Agent files (`AGENTS.md`, `CLAUDE.md`, `.cursor/rules/*.mdc`) must be updated when:

- A new standard is added to `docs/standards/`
- Module architecture or build commands change
- A new vtable interface is added
- Verification commands change

Agent files reference standards -- they never duplicate full standard content.

---

## Design System Alignment

All UI work (web dashboard, website, native apps) must use design tokens:

- **Colors**: `--hu-*` CSS custom properties. Never raw hex values.
- **Typography**: `var(--hu-font)` (Avenir). Never set `font-family` directly.
- **Icons**: Phosphor Regular from `ui/src/icons.ts`. Never emoji characters.
- **Spacing**: Token scale. Never arbitrary pixel values.

See `docs/standards/design/design-strategy.md` and the `design-tokens/` directory for the full token reference.

---

## Compliance Checklist

Use this checklist before marking any work complete:

- [ ] Build succeeds with zero warnings
- [ ] All tests pass with zero ASan errors
- [ ] No duplication introduced (code, docs, or config)
- [ ] Standards alignment verified (read the applicable standard, confirmed match)
- [ ] Agent files updated if commands, architecture, or standards changed
- [ ] Documentation current -- no orphaned or stale docs
- [ ] Design tokens used -- no raw hex colors, pixel spacing, or pixel radii
- [ ] No dead code, debug artifacts, or stale `TODO` hacks in committed code
- [ ] Every allocation freed (ASan catches leaks)
- [ ] `HU_IS_TEST` guards present for any new side effects
- [ ] PR description follows conventional commit format
