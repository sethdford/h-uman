# M3 Frontier-Bridge Full Closure — Tasks

| # | Task | ACs | Owner | Status |
|---|---|---|---|---|
| 1 | Implement `POST /v1/adapters/swap` endpoint in `scripts/mlx-server.py`. Request: `{adapter_path: string, contact_hash: string}`. Response: `{status: "ok"|"error", adapter_loaded: string, error?: string}`. Loads weights into running MLX model via `model.load_weights`; reverts on exception. | AC-M3-1 | TBD | pending |
| 2 | Add startup check in `scripts/mlx-server.py`: if delegating to upstream (gemma-realtime) and upstream lacks `/v1/adapters/swap`, exit non-zero with named error. | AC-M3-1 | TBD | pending |
| 3 | Insert `hu_agent_m3_route_per_turn()` call into `src/agent/agent_stream.c` streaming loop, AFTER first successful chunk receipt (per D-M3-2). Same arguments as the existing non-streaming call at `agent_turn.c:4224`. | AC-M3-2 | TBD | pending |
| 4 | Regression test pinning both call sites: `tests/test_m3_route_per_turn_call_sites.c` that fails the build if either `agent_turn.c` or `agent_stream.c` omits the route_per_turn invocation. Uses grep + AST-light inspection. | AC-M3-2 | TBD | pending |
| 5 | Extend `hu_mlx_admin_swap_adapter()` in `src/ml/mlx_admin.c` to emit on any error: (a) structured log line at `error` level with adapter path + contact hash + error code, (b) `m3_adapter_swap_failure_total{reason=...}` metric increment, (c) `hu_log_info_once()` on first failure post-startup. | AC-M3-3 | TBD | pending |
| 6 | Test injecting HTTP 500 from a fake MLX server: assert log captured, metric incremented, turn continued on base chat. `tests/test_m3_swap_failure_observability.c`. | AC-M3-3 | TBD | pending |
| 7 | Implement `hu_m3_record_outcome_from_provider_result()` in `src/ml/m3_frontier_adapter.c`. Invoked from inside `hu_agent_m3_on_provider_success()` (one call site inside the function, regardless of how many sites call on_provider_success). Records token_count, latency_ms, contact_hash. | AC-M3-4 | TBD | pending |
| 8 | Implement outcome ring drainer (resolves Q-M3-C): new daemon tick `hu_daemon_tick_m3_outcome_drain()` running every 256 turns OR 5 min, calls `hu_m3_frontier_adapter_snapshot_outcomes()`, persists to new SQLite table `m3_outcomes` via migration. Includes drain-marker advance + ring reset semantics. | AC-M3-4 (expanded) | TBD | pending |
| 9 | Validation task: read `src/ml/fidelity.c`, confirm `communication_style_fidelity_score` semantics suit cross-adapter aggregation. If unsuitable, design fallback: rank-by-mean + paired bootstrap CI on 20-prompt set. Output: a short `docs/m3-fidelity-score-calibration.md`. | Risk-1, AC-M3-5 | TBD | pending |
| 10 | Extend A/B harness (`scripts/m3_eval_adapter.py` + new C-side gateway method `hu_m3_ab_run_fidelity_gate`) to score candidate vs baseline on held-out prompts, emit PASS iff `Δfidelity ≥ threshold` (default 0.05, configurable). | AC-M3-5 | TBD | pending |
| 11 | Build held-out fixture: `tests/fixtures/m3/dpo_pairs_50.jsonl` (≥50 alpaca-DPO rows) + `tests/fixtures/m3/holdout_prompts.jsonl` (≥20 prompts). Both reflect realistic h-uman messaging shapes, no real PII. | AC-M3-5, AC-M3-6 | TBD | pending |
| 12 | Implement `scripts/m3-live-fire.sh`: cold-starts daemon + MLX server, ingests fixture DPO pairs, triggers training, swaps adapter, serves N turns, runs A/B gate, exits 0 iff fidelity-PASS. Bash with `set -euo pipefail`. | AC-M3-6 | TBD | pending |
| 13 | Implement fake `mlx_lm` subprocess shim under `HU_IS_TEST`: copies a known-good adapter fixture in place of actual training. Required so AC-M3-7's E2E test can run without real GPU. Lives in: `tests/fixtures/m3/fake_mlx_lm_train.sh` or equivalent. | AC-M3-7 | TBD | pending |
| 14 | Extend `hu_training_runner_enqueue_lora_persona()` (or current equivalent) with a `target_model` parameter: `huml_reference` (existing behavior) vs `frontier_mlx` (new path via `m3_mlx_lora_bridge.py`). | AC-M3-7 | TBD | pending |
| 15 | Wire daemon auto-invocation: when Spec 2's pair-count threshold fires AND `m3.frontier_auto_training=true`, enqueue with `target_model=frontier_mlx`. Post-training hook calls `hu_mlx_admin_swap_adapter()` automatically. | AC-M3-7 | TBD | pending |
| 16 | E2E test using fake mlx_lm shim: exercises tasks 14+15 deterministically in `human_tests`. Asserts: trigger fires → training enqueued with frontier target → fake training produces fixture safetensors → admin swap called → outcome ring incremented on subsequent turn. | AC-M3-7 | TBD | pending |

## Dependencies

- Task 2 (startup check) builds on Task 1 (endpoint exists).
- Task 3 (streaming wire) requires Task 1 (endpoint exists end-to-end).
- Task 4 (regression test) requires Tasks 3 + existing non-streaming call site.
- Task 6 (failure test) requires Task 5 (observability) and Task 1 (something to call).
- Task 8 (drainer) requires Task 7 (population) — drainer reads what populator writes.
- Task 10 (fidelity gate) requires Task 9 (validation).
- Task 11 (fixtures) can run in parallel with anything.
- Task 12 (live-fire) requires Tasks 1, 3, 5, 7, 8, 10, 11 (it exercises all of them).
- Task 15 (daemon auto-invocation) requires Tasks 13 + 14 + **Spec 2 / AC-RL-1** (the pair-count trigger landing).
- Task 16 (E2E test) requires Tasks 13, 14, 15.

## Sequencing recommendation

**Phase A (observability foundation; lands without external proofs):**
- Tasks 1, 2 (endpoint)
- Tasks 5, 6 (failure observability)
- Tasks 7, 8 (outcome ring + drainer)
- Task 9 (fidelity validation)

**Phase B (route closure):**
- Tasks 3, 4 (streaming wire + regression test)

**Phase C (A/B gate + E2E proofs):**
- Tasks 10, 11 (fidelity gate + fixtures)
- Task 12 (live-fire script)

**Phase D (daemon auto-invocation; gated on Spec 2):**
- Tasks 13, 14 (shim + target_model parameter)
- Task 15 (wire trigger)
- Task 16 (E2E test)

## Cross-spec dependencies

- **Spec 2 AC-RL-1** must land before Task 15. Spec 1 Phases A-C are independent of Spec 2.
- **Spec 3 AC-SM-1** (behavior log) does NOT depend on this spec's tasks; the log hook lives inside `hu_agent_m3_on_provider_success()` which is already called from both streaming and non-streaming paths (recon confirmed 11 sites covering both). Earlier draft dependency removed.
- **Spec 4 AC-TOM-5** (adapter-swap self-change recorder) depends on Task 5 (observable swap success); see Spec 4 tasks.md.

## Verification

After all tasks complete, spawn `spec-verifier`:
```
Agent({
  description: "Verify M3 closure spec satisfaction",
  subagent_type: "spec-verifier",
  prompt: "Spec at specs/2026-05-19-m3-frontier-bridge-closure/. Verify each of AC-M3-1 through AC-M3-7 against the implementation. AC-M3-6's live-fire must run end-to-end; capture exit code + A/B report. Output RESULT_spec-verifier=PASS|FAIL with per-AC evidence."
})
```
