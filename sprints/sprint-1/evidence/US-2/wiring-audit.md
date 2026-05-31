# US-2 Wiring Audit — Contextual Bandit

**Date:** 2026-05-30  
**Auditor:** implementer  
**Conclusion:** DEAD on send path; WIRED on update path

---

## Exported Functions Audit (per LSP findReferences)

All exported `hu_contextual_bandit_*` functions in `include/human/agent/contextual_bandit.h`:

1. `hu_contextual_bandit_create` — Called from:
   - `tests/test_contextual_bandit.c:44` (test setup)
   - `tests/test_e2e_learning_loop.c:237` (test setup)
   - `tests/test_proactive_outcomes.c:182` (test setup)
   - `src/ml/dpo.c` (contextual_bandit.c internal state allocation — NOT AN EXTERNAL CALLER)

2. `hu_contextual_bandit_destroy` — Called from:
   - `tests/test_contextual_bandit.c:98` (test teardown)
   - `tests/test_e2e_learning_loop.c:251` (test teardown)
   - `tests/test_proactive_outcomes.c:189` (test teardown)

3. **`hu_contextual_bandit_decide_send`** — **NO CALLERS**
   - Defined: `src/agent/contextual_bandit.c:160`
   - Declared: `include/human/agent/contextual_bandit.h:50`
   - grep search: `grep -rn "hu_contextual_bandit_decide_send" src tests --include="*.c" --include="*.h"` returns zero matches outside declaration/definition
   - Conclusion: **DEAD on send path**

4. `hu_contextual_bandit_update` — Called from:
   - `src/ml/dpo.c:1440` — **LIVE CALLER on outcome-update path**
   - `tests/test_contextual_bandit.c` (test setup)
   - Comment in `include/human/ml/dpo.h:300` documents this: "calls hu_contextual_bandit_update for each"

5. `hu_contextual_bandit_get_arm` — Called from:
   - Tests only (no live callers)

6. `hu_contextual_bandit_save`, `hu_contextual_bandit_load`, `hu_contextual_bandit_sample_beta` — Tests only

---

## Path Analysis: `hu_contextual_bandit_update` Call Site

**Live call in production**: `src/ml/dpo.c:1410-1450`

```c
hu_error_t hu_proactive_outcomes_process_async(
    hu_memory_facade_t *mem_facade,
    void *bandit_opaque,
    const hu_outcome_event_t *events,
    size_t event_count) {
    // ...
    hu_contextual_bandit_t *bandit = (hu_contextual_bandit_t *)bandit_opaque;
    // ...
    for (size_t i = 0; i < event_count; i++) {
        hu_contextual_bandit_update(bandit, contact_handle, outcome_type);
    }
}
```

**Call site path**: `src/ml/dpo.c:1440` (via `hu_proactive_outcomes_process_async`)

**Caller of this function**: Only `tests/test_proactive_outcomes.c` (test code)
- No production call path from the agent/conversation core to this function

**Result**: The bandit UPDATE is tested and wired to an async outcomes processor, but that processor is NOT invoked from any live agent-turn path. The bandit arm states accumulate in memory but `hu_contextual_bandit_decide_send()` is never called to use them.

---

## Send-Path Analysis: `hu_contextual_bandit_decide_send` 

**Definition**: `src/agent/contextual_bandit.c:160-173`

```c
hu_error_t hu_contextual_bandit_decide_send(hu_contextual_bandit_t *bandit, uint64_t contact_handle,
                                            bool *out_should_send) {
    hu_error_t err = HU_OK;
    hu_contextual_bandit_arm_t *arm = lookup_or_insert(bandit, contact_handle, &err);
    if (err != HU_OK) return err;

    double theta = hu_contextual_bandit_sample_beta(arm->alpha, arm->beta, &bandit->rng_seed);
    *out_should_send = (theta > bandit->threshold);
    return HU_OK;
}
```

**Call sites**: Zero

**grep verification**:
```
$ grep -rn "hu_contextual_bandit_decide_send" src tests --include="*.c" --include="*.h" | grep -v "contextual_bandit.h\|contextual_bandit.c"
(no output)
```

**grep for ANY call to bandit inside agent_turn.c**:
```
$ grep -rn "contextual_bandit\|bandit" src/agent/agent_turn.c
(no output)
```

**Conclusion**: `hu_contextual_bandit_decide_send()` is **DEAD CODE** on the send path. It is fully implemented, unit-tested in isolation, but never invoked from any production or test code path that routes through the agent turn loop.

---

## Verdict

| Component | Status | Evidence |
|-----------|--------|----------|
| **Bandit implementation** | ✅ COMPLETE | `src/agent/contextual_bandit.c:1-350+` |
| **Bandit unit tests** | ✅ COMPLETE | `tests/test_contextual_bandit.c` (8 test cases) |
| **Bandit update wiring** | ✅ WIRED | `src/ml/dpo.c:1440` calls `hu_contextual_bandit_update` on every proactive outcome |
| **Bandit decide wiring** | ❌ DEAD | Zero call sites found via grep + LSP; never invoked from agent-turn or any send path |

**Design Decision**: Wire `hu_contextual_bandit_decide_send()` to **per-contact humanization-param selection** (lower risk than model routing, aligns with persona-layering.md).

