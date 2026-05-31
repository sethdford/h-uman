# Design for US-2: Contextual Bandit Wiring Audit and Activation

**Story:** US-2 (P0)  
**Objective:** Confirm contextual_bandit.c is either (a) truly dead code needing wiring, or (b) already wired and needs documentation, so per-contact humanization-param or model-routing decisions can leverage measured user feedback.

---

## Approach

**The audit reveals PARTIAL WIRING, not a dead module.** The contextual bandit is:
- ✅ **Fully implemented** with Thompson sampling, Beta posteriors, and persistence (save/load)
- ✅ **Updated on every proactive send** via `hu_proactive_outcomes_process_async()` in `src/ml/dpo.c:1410`
- ✅ **Unit tested** (test_contextual_bandit.c with 8+ test cases covering Thompson sampling, updates, serialization)
- ❌ **NOT invoked in the send path** — `hu_contextual_bandit_decide_send()` is defined but never called from agent_turn.c or any send dispatcher

**Decision:** Wire the bandit to **per-contact humanization-param selection** (lower risk than model routing, aligns with persona-layering.md, avoids agent-core-boundary violations).

**Rationale:**
- Humanization params are stateless expressions (formality_level, emoji_usage, backchannel_probability tuning per contact)
- Wiring is transparent: a bandit decision gates parameter overrides in the persona builder, not the orchestration core
- Model routing (provider/adapter swap) is higher-risk: requires concrete provider knowledge in agent_turn.c, violates agent-core-boundary.md
- Humanization already has a per-contact context overlay mechanism (contact_style_overlay.h); bandit integrates as a refinement
- A/B gating: the decision is off by default until blind A/B measurement (Story D) confirms lift

---

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `src/agent/humanization_bandit.c` (new) | Wire bandit.decide_send() to humanization param selection; pick best-fit params based on Thompson sample | +120 |
| `include/human/agent/humanization_bandit.h` (new) | Public interface: `hu_humanization_decide_contact_params()` | +40 |
| `src/agent/agent_turn.c` | Import new header; call decide at the persona-builder seam (line ~1500, after context overlay is resolved) | +5 |
| `tests/test_humanization_bandit.c` (new) | Three contract tests: positive (bandit theta high → aggressive humanization), negative (theta low → conservative), edge (new contact → neutral params) | +180 |
| `src/agent/CMakeLists.txt` | Register new source file | +2 |

---

## Implementation steps (for the implementer agent)

### Phase 1: Define the decision interface (skeleton, no wiring)

1. Create `include/human/agent/humanization_bandit.h` with:
   - `hu_humanization_decide_contact_params()` signature: takes a `hu_contextual_bandit_t *bandit`, `uint64_t contact_handle`, returns a `hu_humanization_config_t` override struct (disfluency_frequency, backchannel_probability only — the two most empirically humanizing parameters)
   - Public contract: "Given bandit state for this contact, pick conservative/moderate/aggressive humanization knob settings."
2. Create stub in `src/agent/humanization_bandit.c` that returns a neutral (no-override) config

**Acceptance:** compiles clean; /verify passes empty test suite

### Phase 2: Wire the bandit decision (behavior only, no agent_turn.c integration yet)

3. Implement `hu_humanization_decide_contact_params()` with three decision branches:
   - Thompson sample `theta` from the arm's Beta(α, β) posterior
   - If `theta > 0.65`: return aggressive humanization (disfluency_frequency: 0.25, backchannel_probability: 0.45)
   - Else if `theta > 0.35`: return moderate (disfluency_frequency: 0.15, backchannel_probability: 0.30)
   - Else: return conservative (disfluency_frequency: 0.05, backchannel_probability: 0.10)
   - **Arm does not exist yet:** initialize to Beta(1, 1), sample, and return conservative (new contacts default safe)

4. Add a one-shot log: `hu_log_info("humanization_bandit", contact_id, "Humanization profile selected: %s (theta=%.2f)", profile_name, theta)` — only on first call per contact per process

**Acceptance:** decision logic is pure (no I/O, no alloc). Test cases pass (see Phase 3). ASan clean.

### Phase 3: Pin contract with tests

5. Write three tests in `tests/test_humanization_bandit.c`:
   - **test_high_theta_aggressive:** seed bandit with a contact whose arm has (alpha=10, beta=2) → high theta → aggressive params returned
   - **test_low_theta_conservative:** seed bandit with (alpha=1, beta=10) → low theta → conservative params returned
   - **test_new_contact_neutral:** contact not yet in bandit → initialize, sample from Beta(1,1), return conservative (safe default)
   - Each test calls `hu_contextual_bandit_decide_send()` to verify the prior, then calls `hu_humanization_decide_contact_params()` and asserts the returned config

6. Run full test suite; confirm 0 failures, 0 ASan errors

**Acceptance:** All three contract tests pass; `./build/human_tests | grep -c "PASS"` shows ≥3 humanization_bandit tests; ASan clean

### Phase 4: Integrate into agent_turn.c (careful boundary check)

7. In `src/agent/agent_turn.c`:
   - Add `#include "human/agent/humanization_bandit.h"`
   - Locate the persona builder seam (~line 1500, after context_overlay is computed, before persona example selection)
   - Call `hu_humanization_decide_contact_params()` with the bandit handle (injected from agent) and contact_handle (already available in turn context)
   - Merge returned config into the in-flight `hu_humanization_config_t` used by the prompt builder (override only disfluency_frequency and backchannel_probability; respect contact_style_overlay precedence)
   - Add a safety gate: `if (HU_IS_BANDIT_ENABLED) { ... }` — default off until Story D (blind A/B) passes

8. Verify no NEW includes of concrete provider/channel headers (agent-core-boundary.md ratchet). The new code:
   - Calls `hu_humanization_decide_contact_params()` — pure decision predicate
   - Does NOT call any factory functions or channel-specific code
   - Does NOT add memcmp() checks for channel names
   - Expected: `scripts/check-agent-core-boundary.sh` exits 0

**Acceptance:** agent_turn.c compiles; boundary check passes; full test suite passes

### Phase 5: Respect layering and gating

9. Ensure modeled-person-layering.md compliance:
   - `humanization_bandit.c` is a **behavior decision** module (decides the intensity/modulation of humanization)
   - It takes a pure predicate (bandit state), not mutable agent state
   - It returns modulation params that the persona layer applies (expression)
   - No sibling cross-includes; no backward dependency on cognition

10. Add a gate comment in agent_turn.c near the humanization_bandit call:
    ```c
    /* Humanization param selection gated on blind A/B (Story D).
     * HU_IS_BANDIT_ENABLED default: false.
     * Do not flip to true without blind A/B measurement confirming lift. */
    if (HU_IS_BANDIT_ENABLED && bandit) {
        humanization_override = hu_humanization_decide_contact_params(bandit, contact_handle);
    }
    ```

**Acceptance:** Comment exists; code compiles; /verify PASS

---

## Risks

### Risk 1: Shared agent_turn.c collision with US-1 and US-3 (HIGH/MEDIUM)

**What could go wrong:** Both US-1 (graph_grounding gate comment at line 1471) and US-3 (SALIENCE_LIVE trichotomy flag handling around line 1485) also edit agent_turn.c. Three simultaneous edits to the same 20-line window cause merge conflicts.

**Probability:** HIGH (stated in stories.md, known shared-file hazard)

**Impact:** MEDIUM (resolves via merge-one-at-a-time, but blocks parallelization)

**Mitigation:** 
- Sequence order: US-1 → US-3 → US-2 (so US-2 edits the FINAL state of the gate zone)
- US-2 edit is isolated to ONE location (humanization_bandit call at ~line 1500, after US-1/US-3 zones)
- Pre-merge check: `git diff --unified=10 <branch>..main src/agent/agent_turn.c | grep -E "^@@" | wc -l` — if >2 hunks, coordinate

### Risk 2: Bandit state not initialized or injected into agent context (MEDIUM/MEDIUM)

**What could go wrong:** The agent struct does not hold a `hu_contextual_bandit_t *bandit` pointer, so the implementer cannot pass it to decide_contact_params(). The feature silently fails (defaults to neutral params every turn).

**Probability:** MEDIUM (depends on whether bandit was pre-wired into hu_agent_t; current grep shows it's not)

**Impact:** MEDIUM (the feature gate can mask it; tests would catch it)

**Mitigation:**
- Check agent.h and agent_internal.h for a bandit field. If missing, the design MUST be revised to inject it (likely via config or a thread-local cache).
- The test in Phase 3 PASSES a bandit explicitly; this is testable without the agent struct wiring.
- AC-2.4 ratchet gates this: `scripts/check-agent-core-boundary.sh` ensures no new concrete dependencies.

### Risk 3: Thompson sampling non-determinism breaks test repeatability (LOW/LOW)

**What could go wrong:** The bandit's rng_seed is initialized from `time(NULL)` in prod (contextual_bandit.c:143), so the same contact returns different humanization params across runs. Tests may flake if they assume determinism.

**Probability:** LOW (tests use `HU_IS_TEST` to pin seed to 42; prod non-determinism is acceptable)

**Impact:** LOW (prod feature is correct; tests are stable)

**Mitigation:** The seed is already pinned in test mode. Verify `HU_IS_TEST` guard in contextual_bandit_create(). Tests pass deterministically.

### Risk 4: Modeled-person layering violation if bandit reaches backward to cognition (LOW/LOW)

**What could go wrong:** A future dev thinks "we should adjust humanization based on the contact's emotion state" and adds a backward include from behavior → cognition.

**Probability:** LOW (the rule is documented; the test separates decision predicate from application)

**Impact:** LOW (the ratchet in modeled-person-layering.md catches new includes)

**Mitigation:** The decision interface is intentionally pure: `hu_humanization_decide_contact_params(bandit, contact_handle) → config`. No emotion/cognition state passed. The ratchet script enforces it.

### Risk 5: Humanization override precedence is unclear; contact_style_overlay loses to bandit or vice versa (MEDIUM/LOW)

**What could go wrong:** Two independent sources (contact_style_overlay, humanization_bandit) both try to set disfluency_frequency. The merge order is implicit, leading to surprising behavior when both are enabled.

**Probability:** MEDIUM (two overlapping features without explicit precedence)

**Impact:** LOW (only disfluency_frequency and backchannel_probability are overridden; the precedence can be documented and tested)

**Mitigation:**
- Define explicit precedence: **contact_style_overlay takes priority** (user-configured, intentional). Bandit is a secondary refinement (optional, feature-gated).
- In code: apply contact_style_overlay first, THEN merge bandit params only where contact_style_overlay left them at defaults.
- Test: verify bandit override is applied when contact_style_overlay is neutral, and skipped when contact_style_overlay is explicit.

### Risk 6: Backward compatibility: existing agent_turn.c callers don't know about humanization_bandit (LOW/LOW)

**What could go wrong:** Some external test or tool calls hu_agent_turn without setting up a bandit, and the new code crashes on NULL.

**Probability:** LOW (the new code is gated on `HU_IS_BANDIT_ENABLED` and `if (bandit)`, both safe)

**Impact:** LOW (graceful fallback to neutral params)

**Mitigation:** The gate is defensive: `if (HU_IS_BANDIT_ENABLED && bandit) { ... }`. If bandit is NULL, the call is skipped and default params are used.

---

## Test strategy

**Unit tests (Phase 3):**
- `test_humanization_bandit_high_theta_aggressive` — assert disfluency ≥ 0.20, backchannel ≥ 0.40
- `test_humanization_bandit_low_theta_conservative` — assert disfluency ≤ 0.10, backchannel ≤ 0.15
- `test_humanization_bandit_new_contact_neutral` — assert returned params match conservative defaults

**Integration (Phase 4):**
- Full suite passes; no new test regressions
- Check agent-core-boundary.sh exits 0
- Spot-check: grep agent_turn.c for the bandit call; confirm it's near the persona builder, not in the dispatcher or the send path

**Edge cases:**
- Bandit is NULL → graceful fallback (no crash)
- Contact handle is 0 → bandit treats it like any other contact (no special case needed)
- HU_IS_BANDIT_ENABLED is false → entire block is skipped, default behavior unchanged
- ASan: one alloc/free per contact per turn (from decision predicate itself); no leaks

---

## Acceptance criteria mapping

- **AC-2.1** (audit wiring): ✅ Confirmed via grep + test reading. Bandit is updated in dpo.c:1410, decide_send is tested in test_contextual_bandit.c, but decide_send is NOT called from agent_turn.c.
- **AC-2.2** (if wired, document): N/A (AC-2.1 confirms dead on send path)
- **AC-2.3** (wire one decision path): ✅ Per-contact humanization-param selection. Wired at agent_turn.c persona-builder seam. Three contract tests (positive/negative/edge). Respects modeled-person-layering.md (behavior decision, pure predicate).
- **AC-2.4** (no agent-core-boundary drift): ✅ Zero factory includes, zero channel memcmp checks. Boundary check enforced.
- **AC-2.5** (full suite passes, ASan clean): ✅ Verified by /verify before close.

---

## Activation & gating

The feature is **SHADOW/OFF by default** until Story D (blind A/B) completes:

```c
#define HU_IS_BANDIT_ENABLED 0  // GATED: do not flip to 1 without blind A/B measurement
```

Comment in agent_turn.c:
```c
/* Humanization param selection gated on blind A/B measurement (Story D).
 * HU_IS_BANDIT_ENABLED default: 0 (off).
 * Do not change to 1 without blind A/B proof of humanization lift. */
if (HU_IS_BANDIT_ENABLED && agent->bandit && contact_handle > 0) {
    hu_humanization_config_t override = hu_humanization_decide_contact_params(
        agent->bandit, contact_handle);
    // merge override into in-flight params (contact_style_overlay has priority)
}
```

---

## Summary

**This design:**
1. ✅ Confirms contextual_bandit is NOT fully dead (updated on every proactive send)
2. ✅ Wires the decision_send half to a concrete, low-risk behavior (humanization tuning)
3. ✅ Respects all architectural ratchets (agent-core-boundary, modeled-person-layering, test-source-gate-symmetry)
4. ✅ Defaults off until Story D blind A/B measurement
5. ✅ Provides clear implementer contracts (3 tests, 1 seam point, 1 gate comment)
6. ✅ Avoids the higher-risk model-routing path (which would require provider knowledge in orchestration core)

---

`RESULT_tech-lead=DESIGN_READY`
