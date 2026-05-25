# Design for US-4: Bridge B3 adapter wire — probe counter outcome metadata

## Approach

**Goal:** Wire latency_ms + completion_tokens from provider response into the M3 probe counter's outcome ring buffer, enabling training signals grounded in real inference performance.

**Scope:** This story spans TWO subsystems:
1. **Provider response layer** — `hu_chat_response_t` gains `latency_ms` field (PUBLIC API CHANGE)
2. **M3 adapter layer** — `hu_m3_frontier_adapter_probe_infer` captures both fields into the ring buffer via a new getter `hu_m3_frontier_adapter_probe_outcome_at`

**Design choice rationale:**
- The ring buffer struct `hu_m3_inference_outcome_t` already has `latency_ms` (line 139 of the header) — so the storage is READY; the gap is just WIRING it from provider → adapter.
- Adding `latency_ms` to `hu_chat_response_t` is mandatory because all 11 call sites of `hu_m3_frontier_adapter_probe_infer` are in agent code paths that own the provider response object. Without the latency field on the response, the agent code cannot pass it to the adapter.
- The getter `hu_m3_frontier_adapter_probe_outcome_at(adapter, idx)` returns a new struct `hu_m3_probe_outcome_snapshot_t {count, latency_ms}` — a lightweight view of outcome[idx] that tests can assert on without knowing the ring's internal layout.

## Files to modify

| File | Change | LOC |
|---|---|---|
| `include/human/provider.h` | Add `uint64_t latency_ms;` to `hu_chat_response_t` | +2 |
| `include/human/ml/m3_frontier_adapter.h` | Define `hu_m3_probe_outcome_snapshot_t` struct; add `hu_m3_frontier_adapter_probe_outcome_at` getter | +15 |
| `src/ml/m3_frontier_adapter.c` | Implement getter (read from ring[idx]) | +10 |
| `src/providers/<all>.c` | Wire `latency_ms` from start/end timestamps into response before returning (scope: every vtable implementer) | +5 per provider |
| `src/agent/agent_turn.c` | Pass `response.usage.completion_tokens` + `response.latency_ms` to adapter when calling `probe_infer` (11 call sites total) | +3 total |
| `tests/test_m3_frontier_probe.c` | New test `test_m3_probe_outcome_metadata_captured` exercises both fields | +30 |

**Estimated lines:** +80 total (struct def + impl + test), +55 provider wiring (assuming 11 providers need latency_ms tracking)

## Implementation steps

1. **Skeleton (reversible):** Define `hu_m3_probe_outcome_snapshot_t` in the header with placeholders; declare getter.
2. **Response field:** Add `uint64_t latency_ms;` to `hu_chat_response_t` (1 line, breaks ABI, but tests immediately signal any callers that forget to populate it).
3. **Getter impl:** Implement `hu_m3_frontier_adapter_probe_outcome_at` to read outcome[idx].latency_ms and completion_tokens from the ring.
4. **Provider wiring:** For each provider vtable (gemini, openai, mlx, etc.), capture wall-clock at request start/end, compute latency_ms, store in response struct before returning. Test with `test_provider_latency_ms_nonzero`.
5. **Agent wiring:** In the 11 call sites of `hu_m3_frontier_adapter_probe_infer`, pass `{completion_tokens, latency_ms}` to a new adapter function (or extend probe_infer signature).
6. **Test:** `test_m3_probe_outcome_metadata_captured` calls the adapter N times with mocked responses, snapshots outcomes, and asserts completion_tokens + latency_ms are both recorded.
7. **Run:** `/verify` on the task — test passes, full suite passes, no ASan errors.

## Risks

- **Backward compat (MEDIUM/LARGE):** `hu_chat_response_t` gains a field. Every call site that initializes the response must zero-init the struct or explicitly set latency_ms. **Mitigation:** (1) Zero-init the response struct on the stack in every provider vtable before populating fields (already a pattern). (2) Tests will fail loudly if a provider returns without setting latency_ms (because it's uninitialized heap garbage). (3) Add a regression test per provider that calls chat and asserts latency_ms > 0.

- **Latency measurement accuracy (LOW/SMALL):** Wall-clock timing in C is noisy; jitter from scheduling or system load can make latency_ms unrepresentative. **Mitigation:** Ring buffer stores individual measurements; training loop can compute percentiles (p50, p95) instead of relying on single samples. No single measurement is sacred.

- **MLX provider missing (MEDIUM/MEDIUM):** MLX subprocess inference hasn't landed yet (US-1/US-2/US-3 are verifier stories that don't ship executable code). The adapter wire will run against Gemini/OpenAI/test mocks, but the B3 Bridge-specific claim in the spec ("measure that MLX inference improves persona fidelity") depends on US-5 landing MLX probe_outcome_at usage. **Mitigation:** This story does NOT depend on MLX being ready; it's a structural precondition for US-5. If US-5 is delayed, US-4 has shipped and proven the outcome capture layer works.

- **Ring buffer overflow semantics (LOW/MEDIUM):** When the ring hits capacity (4096), new outcomes overwrite old ones. Tests that verify "probe_count=5 and all 5 outcomes in the ring" will fail if the ring is full and was written to 4096+ times. **Mitigation:** The test uses a tiny fixture with ≤10 calls; ring never fills. Production code doesn't assert on ring contents, only on probe_count + periodic snapshots for training. If production ever needs "all outcomes guaranteed to persist", that's a follow-up (drain to SQLite, which AC-M3-4 already has in the header).

- **Concurrency (LOW):** Ring buffer is written by agent turns (concurrent), read by training loop (periodic snapshot). No atomic ops on latency_ms read/write. **Mitigation:** latency_ms is uint64_t (atomic on most architectures); the worst-case race is a partial read of a uint32_t spanning two cache lines. Outcomes are ephemeral training signals, not safety-critical state. If true lock-free is needed, that's a follow-up (wrap snapshot in a mutex).

## Test strategy

- **Unit:** `test_m3_probe_outcome_metadata_captured` — calls adapter with mocked provider responses (pre-set completion_tokens + latency_ms), snapshots the ring, asserts both fields are recorded correctly. (~30 lines)
- **Unit:** `test_provider_X_latency_ms_populated` for each provider (Gemini, OpenAI, MLX stub, test mock) — calls the provider, asserts response.latency_ms > 0. (~10 lines per provider, 5 providers = 50 lines)
- **Integration:** Existing `test_m3_probe_count_advances_once_per_chat` (US-3 verifier story) passes a full provider response through the probe seam; new assertion checks that latency_ms is also in the outcome.
- **Smoke:** Run full test suite; ASan must report 0 errors (no uninitialized-field reads).

## Acceptance criteria mapping

| AC | Implementation | Test |
|---|---|---|
| AC-4.1 | Provider responses populate `latency_ms`; agent code passes `response.usage.completion_tokens` + `response.latency_ms` to adapter | `test_provider_X_latency_ms_populated` (per provider) |
| AC-4.2 | Ring buffer stores outcomes with both fields; `hu_m3_inference_outcome_t` has fields at struct definition | Static struct definition + field reads in getter |
| AC-4.3 | Getter `hu_m3_frontier_adapter_probe_outcome_at(adapter, idx)` returns snapshot with {count, latency_ms} | `test_m3_probe_outcome_metadata_captured` calls getter and asserts both fields |
| AC-4.4 | Test verifies both recorded from same chat call | `test_m3_probe_outcome_metadata_captured` single adapter instance, multiple chat calls, snapshot shows both fields per call |
| AC-4.5 | probe_count=N (monotonic), min_latency≥0, max_latency>0 (at least one call took measurable time) | Snapshot assertion: for each outcome in ring, latency_ms ≥ 0; find max > 0 |

## Out of scope

- **Real MLX wiring:** MLX inference subprocess (US-1/US-2/US-3) is tested separately; this story captures the seam, not the backend.
- **SQLite drain:** Outcome-ring → database persistence (AC-M3-4 in the header) is NOT part of this story; it's orthogonal infrastructure.
- **Training loop consumption:** How the training_loop.py script reads the ring and uses latency_ms for importance weighting is a future story (US-8).
- **Streaming latency:** Streaming responses (US-4 non-goal per backlog) do not measure latency_ms per chunk; that's a Phase B4 follow-up.

## Risk tier: MEDIUM

- Public API addition to `hu_chat_response_t` requires careful rollout.
- All 11+ provider vtable implementers must be wired simultaneously to avoid silent uninitialized-field bugs.
- Latency measurement is platform-dependent (Windows vs macOS vs Linux).

**Mitigation:** (1) Add regression test per provider. (2) Zero-init response struct everywhere. (3) Tests run on all CI matrix variants (Linux, macOS, Apple Silicon) so latency_ms measurement patterns surface early.

---

RESULT_tech-lead-US-4=DESIGN_READY
