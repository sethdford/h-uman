---
title: "B3 Tasks 4-6 — follow-up after first agent attempt failed build"
created: 2026-05-25
status: open
parent: tasks.md
incident: contamination from background agent (commits 00527490, 7611c0fc, ef823a58)
---

# B3 Tasks 4-6 — Retry plan

## What happened (2026-05-25)

A background agent attempted Tasks 4-6 in one parallel run with US-8 and
US-9. The agent reported "all 12,179 tests pass" but the underlying
human binary fails to compile. Cherry-picking the 3 agent commits
(00527490, 7611c0fc, ef823a58) onto main reproduced the breakage
locally with these errors:

```
src/agent/prompt.c:136: error: no member named 'prompt_budget_config'
                                in 'struct hu_prompt_config'
src/agent/prompt.c:138: error: incompatible pointer types passing
                                'bool *' to 'atomic_bool *'
src/agent/prompt.c:634: error: undeclared function 'HU_PROMPT_SHOULD_SKIP_FIELD'
                       (14 call sites — macro never defined)
```

The agent's `--suite=prompt_budget` test passed because that target
links against `human_core_test` (a separate translation unit that
treats `prompt.c` differently under HU_IS_TEST). The PRODUCTION
binary, which links `prompt.c` directly, failed.

Per CLAUDE.md "Verify, don't assert" and `quality-gates.md` "tests
must pass (full suite, not just changed-files)", the agent's report
was honest about the test binary but did not include the production
binary build. The 3 commits were reverted to keep main shippable.

## Required for the next attempt

### Compile-time contract
1. The macro `HU_PROMPT_SHOULD_SKIP_FIELD(field_id)` MUST be defined
   in the SAME translation unit (or a header included by it) where
   it is used. Its definition needs to access the prompt budget
   state, which means either:
   - A file-static helper function with hidden visibility
   - An `inline` function in `include/human/agent/prompt_budget.h`
   - A macro that expands to a call into a real function
2. The prompt budget state needs to be reachable from
   `hu_prompt_build_system`. The agent tried `config->prompt_budget_config`
   but that field doesn't exist on `hu_prompt_config_t`. Options:
   - Add a `const hu_prompt_budget_config_t *budget_cfg` field to
     `hu_prompt_config_t` (caller threads it through)
   - Use the global singleton (`hu_prompt_budget_t *g_prompt_budget`
     already exists per Task 1b)
3. `atomic_bool` not `bool` for `hu_log_info_once` guards — see
   `include/human/core/log.h` macro signature.

### Test contract (already shipped per Task 1b)
- 18 tests in `prompt_budget` suite already pass against the existing
  TUs. Tasks 4-6 must add 3 more without breaking those 18.

### Build contract (what the agent missed)
- `cmake --build build --target human` MUST succeed after the change.
- `cmake --build build --target human_tests` (the test binary) MUST succeed.
- ASan must report zero leaks.
- The agent's reported "all tests pass" must include the
  `human_tests` AND demonstrate that the `human` binary built.

## Suggested next slice (smaller scope)

Rather than 3 tasks in one agent run, ship just **Task 4 (trim gate)**
in isolation first:
1. Add the missing macro definition as a `static inline bool
   prompt_budget_should_skip_field(field_id)` function at the top of
   `src/agent/prompt.c`.
2. Add the `budget_cfg` pointer to `hu_prompt_config_t` and thread it
   through 1 (one) caller as a smoke test.
3. Wire the gate to just 1 field (memory_context) — proves the
   pattern.
4. Add 1 test: trim gate ON for 1 dead field → byte delta matches.
5. Push + verify build clean + 1 test passing.

Then iterate Tasks 5 + 6 separately.

## Why this matters

The B3 spec's value proposition is *measurement* (per-field byte
accounting + DEAD detection). The TRIM gate adds *action* (skip
dead fields). The doctor check adds *visibility*. The silent-failure
warn adds *operability*. Each can ship independently. Bundling them
into one PR caused the failure mode we just saw — when one piece
breaks the build, the others can't ship either.

Lesson: ship 1 atomic AC at a time, build the binary between each.
