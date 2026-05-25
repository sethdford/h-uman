# Design for US-7: Fidelity delta function + AB comparator integration

## Approach

US-7 requires implementing `hu_communication_style_fidelity_score_delta(baseline, adapted, target_fingerprint)` in `src/ml/fidelity.c` and integrating delta reporting into the CLI's `human ml lora-ab` subcommand.

**Current state:** The `lora-ab` CLI handler exists (src/ml/cli.c:1960-2198) and computes persona-fidelity deltas by calling `hu_communication_style_compare_response_sets()` on two response-JSON files. The text output at line 2180 already prints the delta; AC-7.5 requires JSON mode with `"delta"` as a top-level key.

**Design choice:** AC-7.1 specifies a **leaf function** `hu_communication_style_fidelity_score_delta(baseline, adapted, target_fingerprint)` that scores two individual response strings and returns their fidelity delta. This is a single-pair scorer (not a set aggregator). 

Implementation:
1. **Core function** `hu_communication_style_fidelity_score_delta()` in `src/ml/fidelity.c` — takes two response strings and the target fingerprint, scores each via `hu_communication_style_fidelity_score()`, returns `delta = score(adapted) - score(baseline)` as a float
2. **CLI integration** — add `--json` output flag to `lora-ab` command that emits JSON with `"delta"` key (can call the leaf function directly, or keep using `hu_communication_style_compare_response_sets()` at the set level)
3. **Tests** — AC-7.3 and AC-7.4 test the leaf function on single pairs; AC-7.6 verifies calibration on fixture corpus

**Math contract:** Delta ∈ [-1, 1]. Each score is float ∈ [0, 1] from `hu_communication_style_fidelity_score(target, response_string, response_len)`. Positive delta means `adapted` is closer to the target fingerprint.

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `src/ml/fidelity.c` | Add `hu_communication_style_fidelity_score_delta()` function | +30 |
| `include/human/ml/fidelity.h` | Declare `hu_communication_style_fidelity_score_delta()` | +10 |
| `tests/test_fidelity_delta.c` | New test file with AC-7.3, AC-7.4, AC-7.6 tests | +120 |
| `src/ml/cli.c` | Add `--json` flag to lora-ab, output JSON with delta key | +25 |

## Implementation steps

1. Add function declaration to `include/human/ml/fidelity.h`:
   ```c
   float hu_communication_style_fidelity_score_delta(
       const hu_communication_style_t *target,
       const char *baseline, size_t baseline_len,
       const char *adapted, size_t adapted_len);
   ```

2. Implement in `src/ml/fidelity.c`:
   - Score baseline via `hu_communication_style_fidelity_score(target, baseline, baseline_len)` → `s_base`
   - Score adapted via `hu_communication_style_fidelity_score(target, adapted, adapted_len)` → `s_adapt`
   - Return `s_adapt - s_base`
   - Add defensive checks: NULL target → return 0.0; NULL strings handled gracefully (score as -1, clamp delta)

3. Create `tests/test_fidelity_delta.c`:
   - AC-7.3 test: fixture casual target, casual "adapted" vs verbose "baseline" → assert delta > 0
   - AC-7.4 test: fixture casual target, formal "adapted" vs casual "baseline" → assert delta < 0
   - AC-7.6 test: fixture known-bad and known-good sets, measure |delta| >= 0.05

4. Add `--json` flag to `hu_ml_cli_lora_ab()` in `src/ml/cli.c`:
   - When `--json` is set, output JSON object with keys: `baseline_mean`, `adapted_mean`, `delta`, `scored`, `skipped`
   - Existing text output remains default behavior

5. Run full test suite and verify no regressions

## Risks

- **Scoring magnitude calibration (LOW/SMALL):** If the underlying `hu_communication_style_fidelity_score()` is too coarse, differences may not reach 0.05 magnitude even on "clearly good vs bad" fixtures. Mitigation: AC-7.6 test verifies this; if it fails, we inspect scorer or adjust fixture corpus to use more extreme examples.
- **JSON schema compatibility (LOW/SMALL):** If lora-ab JSON output format changes post-launch, downstream tools break. Mitigation: pin schema via existing `test_check_lora_ab_json_schema.sh` (already in test suite).
- **Input validation (LOW):** NULL strings or fingerprints should be handled gracefully. Mitigation: `hu_communication_style_fidelity_score()` already handles these; our wrapper inherits that safety.
- **Floating-point precision (LOW):** Deltas near 0.05 may be affected by rounding. Mitigation: tests use generous bounds (e.g., assert delta > 0.04 for positive cases).

## Test seam

- **Unit tests:** `tests/test_fidelity_delta.c` (three test cases):
  - `test_fidelity_delta_positive_when_adapted_more_casual` (AC-7.3)
  - `test_fidelity_delta_negative_when_adapted_diverges` (AC-7.4)
  - `test_fidelity_delta_magnitude_ge_005_on_fixture_corpus` (AC-7.6 calibration)

- **Integration:** Existing `test_check_lora_ab_json_schema.sh` validates JSON output shape

- **AC mapping:**
  - AC-7.1 → function declaration + compilation success
  - AC-7.2 → test setup (two strings scored independently, delta = difference)
  - AC-7.3 → `test_fidelity_delta_positive_when_adapted_more_casual`
  - AC-7.4 → `test_fidelity_delta_negative_when_adapted_diverges`
  - AC-7.5 → `human ml lora-ab --json` outputs valid JSON with `"delta"` key
  - AC-7.6 → `test_fidelity_delta_magnitude_ge_005_on_fixture_corpus` asserts |delta| >= 0.05

## Out of scope

- Rewriting `hu_communication_style_fidelity_score()` — the underlying scorer is out of scope; AC-7.6 assumes it's already calibrated to human-readable scales
- Streaming or real-time delta computation
- Multi-persona or context-dependent deltas
- Automatic threshold selection (AC-7.6 is a verification assertion, not a tuning knob)

---

**Status:** READY. Function signature is clear from AC-7.1, test cases are concrete, CLI integration has an existing foundation to build on. No blocking unknowns.

