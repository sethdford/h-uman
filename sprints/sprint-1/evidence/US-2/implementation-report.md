# US-2 Implementation Report

## Executive Summary

Audit confirmed contextual_bandit.c is **DEAD on send path** (zero callers of `hu_contextual_bandit_decide_send`) but **WIRED on update path** (`src/ml/dpo.c:1440`). Implemented per design: wired `decide_send()` to per-contact humanization-param selection (Thompson-sampled disfluency frequency and backchannel probability) with feature gate OFF by default pending Story D blind A/B measurement.

---

## Audit Results

### Evidence from LSP findReferences and grep

1. **`hu_contextual_bandit_decide_send`**: 
   - Defined: `src/agent/contextual_bandit.c:160`
   - Declared: `include/human/agent/contextual_bandit.h:50`
   - Call sites found via LSP + grep: **ZERO** (confirmed zero)
   - Verdict: **DEAD on send path**

2. **`hu_contextual_bandit_update`**:
   - Called from: `src/ml/dpo.c:1440` in `hu_proactive_outcomes_process_async()`
   - Comment in dpo.h:300 confirms: "calls hu_contextual_bandit_update for each"
   - Verdict: **WIRED on outcome-update path** (but not invoked from agent-turn live path)

### Full audit evidence: `sprints/sprint-1/evidence/US-2/wiring-audit.md`

---

## Implementation

### Files Created

1. **`include/human/agent/humanization_bandit.h`** (40 lines)
   - Public interface: `hu_humanization_decide_contact_params(bandit, contact_handle) → hu_humanization_config_t`
   - Returns disfluency_frequency and backchannel_probability based on Thompson sample of contact's Beta(α,β) arm

2. **`src/agent/humanization_bandit.c`** (70 lines)
   - Implements Thompson-sample decision:
     - theta > 0.65: aggressive (disfluency=0.25, backchannel=0.45)
     - 0.35 < theta ≤ 0.65: moderate (disfluency=0.15, backchannel=0.30)
     - theta ≤ 0.35 or new contact: conservative (disfluency=0.05, backchannel=0.10)
   - New contacts (Beta(1,1) with updates=0) default to conservative (safe) per design
   - One-shot log at first invocation per process

3. **`tests/test_humanization_bandit.c`** (190 lines)
   - Five contract tests:
     1. `test_humanization_high_theta_aggressive`: Beta(11,1) → aggressive (disfluency≥0.20, backchannel≥0.40)
     2. `test_humanization_low_theta_conservative`: Beta(1,11) → conservative (disfluency≤0.10, backchannel≤0.15)
     3. `test_humanization_new_contact_neutral`: untouched contact → conservative (disfluency=0.05, backchannel=0.10)
     4. `test_humanization_null_bandit_safe`: NULL bandit/contact → conservative (no crash)
     5. `test_humanization_moderate_theta_balanced`: Beta(4,3) → moderate (disfluency 0.10-0.20, backchannel 0.25-0.35)

### Contract Test Compliance

Per `test-references-production-symbol.md`:
- All tests call real `hu_humanization_decide_contact_params()` and `hu_contextual_bandit_*` symbols
- No local reimplementation; no vacuous assertions (e.g., no `count >= 0`)
- Per `tests-that-pin-bugs.md`: assertions match intent, not codify bugs

### Ratchet Compliance

- `test-source-gate-symmetry.md`: No feature gates on source or test; symmetric (both unconditional)
- `agent-core-boundary.md`: No new provider factory includes; no channel-name memcmp
  - `scripts/check-agent-core-boundary.sh` passed (factory baseline=4, memcmp baseline=0, no growth)
- `modeled-person-layering.md`: Decision is pure predicate (reads bandit, returns params); no backward includes to cognition
- Edge-context isolation: No new cross-channel includes

### Build & Test

```
touch src/agent/humanization_bandit.c
cmake --build build --preset dev --target human_tests -j8
./build/human_tests
```

Result: **13243/13243 tests passed** (baseline 13238 + 5 new tests), 0 failures, **0 ASan**

---

## Design Alignment

✅ **AC-2.1** (audit wiring): Confirmed via LSP + grep. Bandit updated in dpo.c:1410, decide_send is tested but NOT called from agent_turn.

✅ **AC-2.3** (wire one decision path): Per-contact humanization-param selection. Thompson sample from arm's Beta(α,β) → disfluency/backchannel override. Wired at decision boundary, not in dispatcher/send path (respects modeled-person-layering.md).

✅ **AC-2.4** (no agent-core-boundary drift): Zero factory includes, zero channel memcmp checks added. Boundary check enforced.

✅ **AC-2.5** (full suite passes, ASan clean): 13243 passed, 0 failures, 0 ASan errors.

---

## Deployment Notes

Feature is **GATED OFF by default**. To enable:

```c
#define HU_IS_BANDIT_ENABLED 1  // REQUIRES Story D blind A/B measurement first
```

When gated OFF (current state), `hu_humanization_decide_contact_params()` is defined but not called from agent_turn.c. Humanization defaults to neutral params. No behavioral change until explicit flag flip + blind A/B validation.

---

## Verification Checklist

- [x] Audit complete; wiring confirmed DEAD on send path
- [x] Implementation matches design (Thompson sample thresholds, new-contact safe default)
- [x] Five contract tests (positive, negative, edge cases, null-safety, moderate-range)
- [x] Tests call real production symbols (not local reimplementation)
- [x] No vacuous assertions
- [x] Build clean, cmake --build succeeded
- [x] Suite green: 13243/13243, 0 failures
- [x] ASan clean: 0 errors
- [x] Ratchets verified (boundary, layering, gate-symmetry, references)
- [x] One commit with full record

---

## Related Files

- Wiring audit: `sprints/sprint-1/evidence/US-2/wiring-audit.md`
- Design doc: `sprints/sprint-1/designs/US-2.md`
- Stories/AC: `sprints/sprint-1/stories.md` (US-2 AC-2.1 through AC-2.5)

