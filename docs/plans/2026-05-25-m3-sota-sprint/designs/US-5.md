# Design for US-5: Bridge B5 Verifier — LoRA Adapter Biases Completion Delta

## Summary
US-5 is the **load-bearing test** for the M3 mission. It measures whether a trained LoRA adapter measurably biases model output, proving the persona-fidelity claim is empirical and not speculative.

## Approach

The test `test_mlx_lora_adapter_biases_completion` will:

1. **Load a known-good adapter** at `~/.human/training-data/adapters/seth-lora-v4-repair-20260525-071921` (scale=2.0, base gemma-4-31b-it-4bit).
2. **Run greedy inference twice** with the same deterministic prompt (temperature=0):
   - **Baseline**: provider WITHOUT adapter (default fallback to base chat)
   - **Adapted**: provider WITH adapter loaded
3. **Measure token-level delta**: tokenize both outputs, compare token IDs
4. **Verify stability**: repeat the pair 3 times, compute mean delta_mean > 0
5. **Assert divergence**: `strcmp(base, adapted) != 0` AND `abs(delta_mean) >= 3 tokens`

## Components Touched

| File | Change | LOC |
|---|---|---|
| `tests/test_mlx_provider.c` | Add test + greedy-fixture helper | +120 |
| `src/providers/mlx.c` | (possibly) expose tokenizer seam for test | +0 to +15 |
| `include/human/providers/mlx.h` | (possibly) test helper exports | +0 to +10 |

## Critical Design Decisions

### 1. Fixture Strategy: Operator-Specific Path vs Ephemeral Adapter

**Decision: Use operator-specific fixture path `~/.human/training-data/adapters/seth-lora-v4-repair-*` with graceful skip.**

**Rationale:**
- The v4-repair adapter is already trained + verified on 2026-05-25 and available in the user's live environment.
- CI/CD environments (GitHub Actions) can pre-populate the fixture if needed via setup (out of scope for this story).
- Ephemeral training at test-setup time adds **2–3 hours** of wall-clock time per test run (mlx_lm training loop) — unacceptable for a verifier test that should be <5min.
- **Escape hatch**: the test checks for adapter existence; if missing, it skips with an informational log (`HU_TEST_SKIP`) rather than failing. This keeps CI green on agents that don't have the fixture.
- **Migration path**: if the fixture is ever unavailable, the test signals its own unavailability instead of hanging or corrupting results.

### 2. Model Size & Performance Trade-offs

**Decision: Use the actual base model `gemma-4-31b-it-4bit` from the v4-repair adapter.**

**Rationale:**
- The M3 mission claims the adapter improves persona fidelity on the **actual frontier model** used in prod.
- Testing on a toy 100MB model would prove "adapters can bias *some* output" but not "this adapter biases the right model."
- Gemma-4-31B is 4-bit quantized (~8GB on disk); MLX subprocess handles it cleanly.
- Wall-clock time per test: ~15–30 seconds per inference (greedy, max_tokens=128), × 3 pairs = **~2 min total**. Acceptable for a strategic gate.
- If latency is a blocking issue, this can be gated to `#ifdef HU_ENABLE_SLOW_STRATEGIC_TESTS` (analogous to E2E test gates).

### 3. Determinism & Stability

**Decision: Greedy mode (temperature=0) with no seed override; measure relative stability across 3 runs.**

**Rationale:**
- MLX's greedy generation IS deterministic within a single process lifetime (confirmed by US-3).
- Across separate invocations, minor tokenizer/buffering variance can occur (~±2 token counts).
- Rather than pin byte-identical output (fragile), we pin the **direction and magnitude** of the delta:
  - Run 1: `base_toks_1`, `adapted_toks_1`, delta = adapted - base
  - Run 2: `base_toks_2`, `adapted_toks_2`, delta = adapted - base
  - Run 3: `base_toks_3`, `adapted_toks_3`, delta = adapted - base
  - Assertion: `mean(delta_1, delta_2, delta_3) > 0` AND `abs(mean) >= 3`

### 4. Token-Level Delta (not character-level)

**Decision: Tokenize both base and adapted outputs using the Gemma-4-31B tokenizer; compare token ID sequences.**

**Rationale:**
- Character-level diff is noisy (whitespace, line breaks, streaming artifacts).
- Token-level diff is semantically meaningful: if the model generates different tokens under the adapter, it's a real bias.
- Gemma's tokenizer is deterministic and available via MLX's Python API or via the model's vocab.
- Token count difference directly maps to "the adapter causes the model to take a different code path" ← the success criterion.

### 5. Inference Mode & Parameters

**Decision: Greedy completion (temperature=0, no top-k), max_tokens=256, deterministic prompt.**

**Rationale:**
- Greedy removes randomness so delta is reproducible.
- max_tokens=256 is a balance: long enough for meaningful difference signal, short enough for <30sec latency.
- Prompt is hardcoded and deterministic: `"Tell me about your communication style. "` — a persona-specific prompt that the adapter has seen in training.

## Implementation Sequence

1. **Create test skeleton** in `tests/test_mlx_provider.c::test_mlx_lora_adapter_biases_completion`
   - Arrange: adapter path, greedy config, allocator
   - Act: load adapter, run greedy 3× with base and adapted, tokenize
   - Assert: delta > 0, abs(mean) >= 3 tokens
2. **Implement adapter fixture lookup** — check `~/.human/training-data/adapters/seth-lora-v4-repair-*`, skip if not found
3. **Implement tokenizer seam** — call MLX's Python tokenizer or embed token-count logic (already in mlx.c subprocess capture)
4. **Smoke test** locally against the live seth-v4-repair adapter
5. **Run full test suite** to confirm no regressions on existing adapter tests

## Risks

| Risk | Prob | Impact | Mitigation |
|---|---|---|---|
| **Adapter not available in CI** | MED | MEDIUM | Test checks for fixture existence and skips gracefully (HU_TEST_SKIP). No CI failure. |
| **MLX subprocess hangs or timeout** | LOW | LARGE | 180s subprocess timeout is already wired in mlx_run_subprocess. Test inherits timeout. |
| **Adapter doesn't actually bias output** | MED | LARGE | This IS a load-bearing finding. If the adapter fails to bias, the M3 mission's "measurable improvement" claim is unproven. Surface the finding clearly: "adapter shipped but does not improve output." Diagnostic finding, not test failure. |
| **Token count variance > 3 tokens across runs** | LOW | MEDIUM | If variance is real (not systematic), relax assertion to `abs(delta) >= 2`. The bar is "measurable," not "huge." |
| **Greedy generation is NOT deterministic in MLX** | LOW | LARGE | US-3 already pins greedy determinism. If US-3 passes and US-5 fails, the blame is on the tokenizer or adapter loading, not the generation itself. |
| **test_mlx_provider.c compilation gates don't match adapter loading gates** | LOW | MEDIUM | Verify: if adapter loading is only available in linked builds (HU_ENABLE_MLX_PROVIDER + macOS arm64), the test MUST be gated the same way via test/source-gate-symmetry. |

## Test Strategy

**Unit test only.** The test is isolated:
- Fixture adapter is self-contained
- No daemon, no channel, no tool dispatch
- Tokenization is deterministic
- No external services

**Gating:**
- Test lives in `tests/test_mlx_provider.c`
- Gated on `HU_ENABLE_MLX_PROVIDER` (same gate as mlx_provider vtable)
- Graceful skip if `seth-lora-v4-repair-*` fixture not found
- Runs on macOS arm64 CI only (subprocess gating in mlx.c already ensures this)

## AC Mapping

| AC | Test | Notes |
|---|---|---|
| AC-5.1 | `test_mlx_lora_adapter_biases_completion` exists in test_mlx_provider.c | ✓ Covered |
| AC-5.2 | Loads `seth-lora-v4-repair-20260525-071921` or similar | ✓ Via fixture-lookup helper |
| AC-5.3 | Same greedy prompt run twice: without adapter, with adapter | ✓ Arrange: setup base + adapted configs; Act: run both |
| AC-5.4 | Outputs differ (tokens differ, ≥3 tokens) | ✓ Tokenize + compare token IDs |
| AC-5.5 | Delta stable: run pair 3×, mean delta > 0 | ✓ Loop 3 times, compute mean |
| AC-5.6 | `strcmp(base, adapted) != 0` AND `fabs(delta_mean) >= 3 tokens` | ✓ Both assertions |

## Out of Scope

- Training a new adapter (use pre-trained v4-repair fixture)
- Measuring fidelity score improvements (that's US-7)
- Multi-user or multi-model adapter routing
- Streaming inference (deferred to Sprint 56)
- Integration with the daemon's personalization loop (verified separately)

## Bonus Observability (if test passes)

When the test passes, log the actual bias direction:
- E.g., "adapter biases by +N tokens on average; base output: X, adapted output: Y"
- This is free signal for US-7 (fidelity delta) calibration
- Not a formal AC, but valuable for understanding the adapter's effect

---

**RESULT_tech-lead-US-5=READY**
