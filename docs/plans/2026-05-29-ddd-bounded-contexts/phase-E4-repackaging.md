# Phase E4 — Internal Repackaging: Config Facade + `agent/` Sub-Packages

> **Program v2 note (2026-05-31):** carried forward from v1 Phase 4. Lowest-severity,
> runs last/ongoing. **Updated baseline:** `agent/` is now **157 flat files** (was
> 154); `config_*.c` are relocated to `src/config/` by E1, so Part A's facade is
> built *over* the co-located config module. Retires the `FACTORY_BASELINE` ratchet
> (4 → 0) on provider injection. See [README.md](README.md).

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Lower coupling without changing behavior. (A) Break the `hu_config_t` 73-includer chain by giving agent core a narrow application-config facade. (B) Give `src/agent/` (154 flat files) real sub-package directories matching its 8 implicit sub-domains. Lowest-severity phase — do it last, after T1-T3 are addressed.

**Architecture:** (A) define `hu_agent_app_config_t` (~12 fields agent core actually uses) + a builder from `hu_config_t`; migrate agent entry points to accept the narrow type. This also lets the 5 provider-`factory.h` includes in `agent/` be replaced by an injected provider vtable, dropping the Phase-0 `FACTORY_BASELINE` ratchet to 0. (B) `git mv` files into sub-package dirs; script the include-path churn (`~/.claude/rules/agent-task-sizing.md`: N≥20 mechanical → script).

**Tech Stack:** C11, CMake, `git mv` + `sed`, `HU_TEST_*`.

---

## Part A — Config facade (higher value; do first)

### File Structure
- Create: `include/human/agent/app_config.h` — narrow `hu_agent_app_config_t` + builder decl
- Create: `src/agent/app_config.c` — `hu_agent_app_config_from(const hu_config_t*)`
- Create: `tests/test_agent_app_config.c`
- Modify: `src/CMakeLists.txt`, `tests/test_main.c`

### Task A1: Define the facade + builder (TDD)

- [ ] **Step 1: Identify the fields agent core actually reads from `hu_config_t`**

Run: `grep -rno 'cfg->[a-z_.]*' src/agent | sed 's/.*cfg->//' | sort | uniq -c | sort -rn | head -20`
Expected: a short list (default_provider, default_model, agent.*, security.*, …). These ~12 fields are the facade surface. Record them.

- [ ] **Step 2: Write the facade header** (fill the fields from Step 1; example shape)

```c
/* include/human/agent/app_config.h */
#ifndef HU_AGENT_APP_CONFIG_H
#define HU_AGENT_APP_CONFIG_H
#include "human/config.h"   /* source of truth; facade is a read-only projection */
#include <stdbool.h>
#include <stddef.h>

/* Narrow projection of hu_config_t for the agent turn loop. Agent code accepts
 * this instead of the 65-substruct god-DTO, so config changes elsewhere don't
 * ripple into agent core. Read-only view — borrows strings from the parent cfg
 * (which must outlive it). */
typedef struct hu_agent_app_config {
    const char *default_provider; size_t default_provider_len;
    const char *default_model;    size_t default_model_len;
    bool hula_enabled;
    /* ... the remaining fields identified in Step 1 ... */
} hu_agent_app_config_t;

/* Project a full config into the agent view. Pure, no allocation. */
hu_agent_app_config_t hu_agent_app_config_from(const hu_config_t *cfg);

#endif
```

- [ ] **Step 3: Write the failing test**

```c
/* tests/test_agent_app_config.c */
#include "human/agent/app_config.h"
#include "test_harness.h"
#include <string.h>

static void app_config_projects_core_fields(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    /* set the same fields the builder reads, e.g.: */
    cfg.default_provider = "openai"; /* adjust to real field names from Step 1 */
    hu_agent_app_config_t app = hu_agent_app_config_from(&cfg);
    HU_ASSERT_TRUE(app.default_provider != NULL);
    HU_ASSERT_TRUE(strncmp(app.default_provider, "openai", 6) == 0);
}

void run_agent_app_config_tests(void) {
    HU_TEST_SUITE("agent_app_config");
    HU_RUN_TEST(app_config_projects_core_fields);
}
```

- [ ] **Step 4: Implement the builder** (`src/agent/app_config.c`) — a field-by-field copy from `cfg` into the projection. Register source+test in CMake; declare+call runner in `tests/test_main.c`.

Run: `cmake --build build --target human_tests -j8 && ./build/human_tests --filter=app_config` → PASS.

- [ ] **Step 5: Migrate ONE agent entry point** to accept `const hu_agent_app_config_t *` instead of `const hu_config_t *` (pick the narrowest — e.g. a helper in `prompt.c`). Build prod + full suite. Commit: `refactor(agent): narrow app-config facade over hu_config_t`.

> The remaining agent entry points migrate incrementally (follow-on chip). Each
> migration that removes a `cfg->` deep-reach lowers coupling; when the last
> `providers/factory.h` include is replaced by injected provider access, set the
> Phase-0 `FACTORY_BASELINE` to 0.

---

## Part B — `agent/` sub-packages (mechanical; script-driven; optional)

The 8 sub-domains from the audit. This is cosmetic relative to T1-T3 — schedule
only when the churn won't collide with in-flight Phase 2/3 work.

| Sub-dir | Representative files (count) |
|---|---|
| `agent/planning/` | dag.c, planner.c, tool_router.c, mcts_planner.c, llm_compiler.c (~21) |
| `agent/guards/` | output_validator.c, approval_gate.c, arbitrator.c, input_guard.c (~21) |
| `agent/context/` | context.c, prompt.c, compaction.c, memory_loader.c (~14) |
| `agent/social/` | theory_of_mind.c, channel_trust.c, anticipatory.c (~10) |
| `agent/autonomy/` | goals.c, orchestrator.c, scheduler.c, team.c (~10) |
| `agent/inference/` | model_router.c, speculative.c, hula_analytics.c (~9) |
| `agent/outbound/` | (already exists) proactive.c, burst_egress.c, outbound_sanitize.c (~8) |
| `agent/simulation/` | autodream.c, counterfactual.c (~4) |

### Task B1: Script the move for ONE sub-domain (prove the pattern)

- [ ] **Step 1: Move `simulation/` first (smallest, 4 files)**

```bash
mkdir -p src/agent/simulation
git mv src/agent/simulation/autodream.c src/agent/simulation/counterfactual.c src/agent/simulation/
# update CMake source paths + any #include "human/agent/..." unaffected (headers stay)
grep -rl 'src/agent/simulation/autodream.c\|src/agent/simulation/counterfactual.c' src/CMakeLists.txt \
  | xargs sed -i '' 's#src/agent/simulation/autodream.c#src/agent/simulation/autodream.c#; s#src/agent/simulation/counterfactual.c#src/agent/simulation/counterfactual.c#'
touch src/agent/simulation/*.c && cmake --build build --target human -j8 && \
  cmake --build build --target human_tests -j8 && ./build/human_tests
```
Expected: 0 failures. Headers in `include/human/agent/` do NOT move (public contract paths stay stable) — only `src/` files relocate, so `#include` lines are unaffected; just CMake paths change.

- [ ] **Step 2: Commit** — `refactor(agent): sub-package simulation/`. Repeat per sub-domain as follow-on chips (one chip per sub-dir).

---

## Self-Review

- **Spec coverage:** config facade (Part A) + agent sub-packaging (Part B). Both lower-severity; explicitly sequenced last. ✓
- **No placeholders:** Part A has real header/test/builder shape with a Step-1 command to discover the exact field set (not a TODO — a concrete discovery step). Part B has a complete worked move for the smallest sub-domain + a per-dir repeat. ✓
- **Behavior preservation:** every task ends in prod build (touch-first) + full suite. Moves only; no logic edits. ✓
- **Ties back:** Part A retires the Phase-0 `FACTORY_BASELINE` ratchet to 0 once provider injection lands. ✓
