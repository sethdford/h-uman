# Sprint 55 Backlog — M3 SOTA Personalization

## Goal
Ship the M3 frontier-model personalization system from "we prove the plumbing works" to "we ship an adapter that measurably improves persona fidelity on real reactive iMessage turns served by the actual frontier chat model (mlx-community/gemma-4-31b-it-4bit)."

## Summary Table

| # | Title | Size | Dependencies |
|---|-------|------|--------------|
| US-1 | Bridge B1 verifier: MLX subprocess round-trip | S | none |
| US-2 | Bridge B2 verifier: MLX model path resolution | S | US-1 |
| US-3 | Bridge B3 verifier: MLX inference greedy-output determinism | M | US-2 |
| US-4 | Bridge B3 adapter wire: probe counter outcome metadata | S | US-3 |
| US-5 | Bridge B5 verifier: LoRA adapter biases completion delta | M | US-4 |
| US-6 | Bridge B5 negative path: malformed safetensors rejected | S | US-5 |
| US-7 | Fidelity delta function + AB comparator integration | M | US-6 |
| US-8 | Training loop Phase C3: `--source-jsonl` + real training | L | US-7 |
| US-9 | Nightly fidelity eval harness + SOTA gate | M | US-8 |

## User Stories (in priority order)

### US-1 (P0): Bridge B1 verifier — MLX subprocess round-trip
**As a** ML engineer, **I want** to prove the MLX provider's subprocess linkage works end-to-end, **so that** chat calls against mlx-server actually execute and return responses.

**Acceptance criteria:**
- AC-1.1: Test `test_mlx_chat_subprocess_round_trip` exists at `tests/test_mlx_provider.c` and is gated on `HU_ENABLE_MLX_PROVIDER + __APPLE__ + __arm64__`.
- AC-1.2: Test invokes `hu_mlx_provider_create` with a valid model path (fixture or real HF ID), then calls `hu_provider_chat_with_system` with a deterministic prompt.
- AC-1.3: Chat response is non-empty, contains tokens, and `out->model` matches the provider's model identifier.
- AC-1.4: Test skips cleanly on non-Apple platforms; marks as SKIP in CI matrix output.
- AC-1.5: Verifier assertion: exit code 0, response string length >= 10 chars (minimum viable completion).

**Estimate:** S
**Dependencies:** none
**DoD:** test passes on `HU_ENABLE_MLX_PROVIDER + macOS arm64` build; skips on other variants; CI green.

---

### US-2 (P0): Bridge B2 verifier — MLX model path resolution
**As a** ML engineer, **I want** to test that `hu_mlx_provider_create` validates model paths correctly, **so that** missing or unresolvable models fail early.

**Acceptance criteria:**
- AC-2.1: Test `test_mlx_provider_create_resolves_model_path` exists in `tests/test_mlx_provider.c`.
- AC-2.2: When called with a present model (fixture path or real HF cache), `hu_mlx_provider_create` returns `HU_OK`.
- AC-2.3: When called with a missing path, `hu_mlx_provider_create` returns `HU_ERR_NOT_SUPPORTED` (per the dispatcher safety contract).
- AC-2.4: Provider ownership is correct: caller must `hu_provider_free` to avoid leaks.
- AC-2.5: Verifier assertion: two test cases (present, missing), both exit cleanly.

**Estimate:** S
**Dependencies:** US-1
**DoD:** test passes on all CI variants (path resolution is platform-agnostic); dispatcher-safety contract pinned.

---

### US-3 (P0): Bridge B3 verifier — MLX inference greedy-output determinism
**As a** ML engineer, **I want** to prove MLX inference produces deterministic greedy outputs, **so that** LoRA adapter bias measurements are repeatable.

**Acceptance criteria:**
- AC-3.1: Test `test_mlx_chat_greedy_completion_matches_fixture` exists in `tests/test_mlx_provider.c`.
- AC-3.2: Test calls the same deterministic prompt twice (greedy mode, temperature=0) against the same model and compares outputs.
- AC-3.3: Both completions are byte-identical (or token-count identical within ±2 tokens to account for streaming-buffering variance).
- AC-3.4: Probe counter advances exactly once per `hu_provider_chat_with_system` call (verifies B-pre hook wiring).
- AC-3.5: Verifier assertion: `strcmp(response1.content, response2.content) == 0` or token-count diff <= 2.

**Estimate:** M
**Dependencies:** US-2
**DoD:** test passes on MLX-enabled build; confirms streaming/buffering stability with tiny fixture model.

---

### US-4 (P0): Bridge B3 adapter wire — probe counter outcome metadata
**As a** ML engineer, **I want** to record outcome metadata (token count, latency_ms) in the probe counter, **so that** training outcomes carry real signals instead of just a counter increment.

**Acceptance criteria:**
- AC-4.1: `hu_m3_frontier_adapter_probe_infer` now captures `hu_chat_response_t.usage.completion_tokens` and `hu_chat_response_t.latency_ms` from the chat response.
- AC-4.2: Metadata is persisted in the adapter struct (new fields added to `hu_m3_frontier_adapter_t`).
- AC-4.3: New getter `hu_m3_frontier_adapter_probe_outcome_at(adapter, idx)` returns a struct with `{count, latency_ms}`.
- AC-4.4: Test `test_m3_probe_count_advances_once_per_chat` verifies probe count AND latency_ms are both recorded from the same chat call.
- AC-4.5: Verifier assertion: probe count = N, min latency >= 0, max latency > 0 (at least one call took measurable time).

**Estimate:** S
**Dependencies:** US-3
**DoD:** test passes; outcome metadata wired into the probe seam; no regression on existing probe-counter tests.

---

### US-5 (P0): Bridge B5 verifier — LoRA adapter biases completion delta
**As a** ML engineer, **I want** to measure that a loaded LoRA adapter measurably biases model output, **so that** I can prove the persona-fidelity improvement claim is real.

**Acceptance criteria:**
- AC-5.1: Test `test_mlx_lora_adapter_biases_completion` exists in `tests/test_mlx_provider.c`.
- AC-5.2: Test loads a known-good adapter (fixture safetensors LORA from seth-lora-v4-repair or similar).
- AC-5.3: Test runs the same greedy prompt twice: once without the adapter, once with the adapter.
- AC-5.4: Output strings differ (tokens differ, not just whitespace; token count diff >= 3 tokens).
- AC-5.5: Delta is stable: run the pair 3 times and compute mean delta; delta_mean > 0 (adapter has positive/measurable effect).
- AC-5.6: Verifier assertion: `strcmp(base_completion, adapted_completion) != 0` AND `fabs(delta_mean) >= 3 tokens`.

**Estimate:** M
**Dependencies:** US-4
**DoD:** test passes with seth-lora-v4-repair fixture; proves the strategic M3 claim is empirically true.

---

### US-6 (P0): Bridge B5 negative path — malformed safetensors rejected
**As a** ML engineer, **I want** to reject malformed adapters at load time, **so that** bad files don't corrupt the chat path.

**Acceptance criteria:**
- AC-6.1: Test `test_mlx_lora_adapter_malformed_safetensors_rejected` exists in `tests/test_mlx_provider.c`.
- AC-6.2: Test creates or fixtures a malformed safetensors file (truncated, missing required tensors, or corrupted magic).
- AC-6.3: Calling `hu_mlx_provider_load_adapter` with the malformed path returns `HU_ERR_INVALID_ARGUMENT`.
- AC-6.4: Provider state is unchanged: `hu_mlx_provider_active_adapter_path` still returns NULL (no partial load).
- AC-6.5: Verifier assertion: error code is specific (not generic IO), no crash, no silent fallback.

**Estimate:** S
**Dependencies:** US-5
**DoD:** test passes; happy path and negative path both covered.

---

### US-7 (P0): Fidelity delta function + AB comparator integration
**As a** ML engineer, **I want** to compute persona-fidelity deltas between baseline and adapted outputs, **so that** I can measure whether LoRA training improves persona fidelity.

**Acceptance criteria:**
- AC-7.1: Function `hu_communication_style_fidelity_score_delta(baseline, adapted, target_fingerprint)` exists in `src/ml/fidelity.c`.
- AC-7.2: Delta is computed as `(adapted_score - baseline_score)` where both scores are [0, 1] persona-fidelity measures.
- AC-7.3: Test `test_fidelity_delta_positive_when_adapted_more_casual` verifies delta > 0 when adapted is more casual (matches target).
- AC-7.4: Test `test_fidelity_delta_negative_when_adapted_diverges` verifies delta < 0 when adapted diverges from target.
- AC-7.5: CLI integration: `human ml lora-ab --persona seth --before pre.json --after post.json` outputs delta in JSON under `"delta"` key.
- AC-7.6: Verifier assertion: delta sign matches intent; magnitude >= 0.05 on good vs bad persona examples.

**Estimate:** M
**Dependencies:** US-6
**DoD:** function + tests pass; CLI output valid JSON; lora-ab gates on delta >= threshold.

---

### US-8 (P1): Training loop Phase C3 — `--source-jsonl` + real training
**As a** ML engineer, **I want** to wire the training loop to accept outcome JSONL and train a real LoRA, **so that** the continuous-learning loop produces genuine adapters on real conversation data.

**Acceptance criteria:**
- AC-8.1: Script `scripts/training_loop.py` accepts `--source-jsonl <path>` flag.
- AC-8.2: Script resolves prompt hashes to full text via conversation DB (skips unresolvable with a log note).
- AC-8.3: Script runs `mlx_lm.lora` with the resolved SFT batch as input (real rank-8, 500 iters).
- AC-8.4: Output safetensors are written to `--adapter-out <path>` with LoRA structure (A/B tensors present).
- AC-8.5: Test `scripts/test_training_loop_source_jsonl.py` feeds a 4-sample JSONL + stub conversation DB and verifies output safetensors exists and has correct shape.
- AC-8.6: Verifier assertion: exit code 0; safetensors file size >= 100 KB (non-trivial tensors); adapter loads without error.

**Estimate:** L
**Dependencies:** US-7
**DoD:** script passes integration test; real LoRA trains from outcome JSONL; adapter is usable by `hu_mlx_provider_load_adapter`.

---

### US-9 (P1): Nightly fidelity eval harness + SOTA gate
**As a** ML engineer, **I want** to measure whether the LoRA adapter improves persona fidelity on real conversations, **so that** I can claim the M3 sprint shipped a measurable quality improvement.

**Acceptance criteria:**
- AC-9.1: Eval harness loads 20-30 held-out prompts from Seth's real conversation patterns.
- AC-9.2: Harness runs two passes: (pre) base model + no adapter, (post) base model + trained adapter.
- AC-9.3: Shape classifier scores each response on persona-fidelity [0, 1] per `src/eval/persona_fidelity.c::hu_ml_fidelity_score_*`.
- AC-9.4: Bootstrap CI computed over N=100 resamplings; reported as `[lower, upper]` confidence interval.
- AC-9.5: SOTA gate: `post_mean > pre_mean + 1.96 × stderr` (one-sided, α=0.025); verdict is PASS or FAIL.
- AC-9.6: Verifier assertion: gate PASS when `post_delta >= 0.05` (5% absolute improvement in fidelity); gate FAIL or SKIP (informative) when delta < 0.05.

**Estimate:** M
**Dependencies:** US-8
**DoD:** harness runs end-to-end; CI computed correctly; gate verdict logged; cron entry created for nightly runs.

---

## Non-goals
- **Streaming (Phase B4).** `mlx_vtable.stream_chat` remains NULL; all chat is blocking. Deferring to Sprint 56 for user-visible streaming UX.
- **Continuous nightly retraining (Phase C4 full loop).** Harness proves delta; orchestration cron is out of scope for this sprint.
- **Director compression or DPO judge rewrites.** Already fixed.
- **Multi-user adapter routing or HuLa integration.** Persona scoping is per-daemon only.
- **iOS/iPadOS MLX inference.** macOS only for Sprint 55.

## Open questions for stakeholder

None at this time. The plan docs are explicit; the scope is clear.

## Scope Assessment

**Total estimated effort:** S + S + M + S + M + S + M + L + M = **~6 sprint-days** of focused work, assuming:
- MLX subprocess is already working (B1 shipped, needs test)
- seth-lora-v4-repair fixture is available
- Conversation DB and outcome JSONL are accessible for training_loop.py tests

**Critical dependencies:**
1. MLX executable + Python environment accessible at test time (macOS CI runner)
2. seth-lora-v4-repair adapter at `~/.human/training-data/adapters/seth-lora-v4-repair-*`
3. Fixture conversation DB with at least 32 example outcomes

**Risk flags:**
- US-8 (training_loop.py) depends on mlx-lm package availability — may add 1-2 days if conda/pip environment setup is needed.
- US-5 (LoRA bias measurement) requires a stable fixture adapter — if seth-v4 has issues, fallback to training a tiny ephemeral adapter (adds 2-3 hours).
- US-9 (eval harness) depends on replica of prod Gemma-4-31B weights — if unavailable, use smaller test-case model (changes delta magnitude but not the methodological proof).

**Recommendation:** This sprint is FEASIBLE in one week with careful sequencing:
- **Wave 1 (Days 1-2):** US-1, US-2, US-3 (verifier tests — foundation)
- **Wave 2 (Day 3):** US-4, US-5, US-6 (adapter + measurement)
- **Wave 3 (Day 4):** US-7 (delta function, quick)
- **Wave 4 (Days 5-6):** US-8 (training loop, wall-clock for mlx-lm training)
- **Wave 5 (Day 7):** US-9 (eval harness, run + measure)

All nine stories fit in one sprint. **RESULT: READY.**

RESULT_product-owner=READY
