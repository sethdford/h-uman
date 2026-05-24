# M3 Frontier-Bridge Full Closure — Design

## Components

- **`mlx-server.py` swap endpoint** — new HTTP handler implementing `POST /v1/adapters/swap` inline in `scripts/mlx-server.py`, with documented JSON shape. Replaces the implicit delegation-to-gemma-realtime contract. Lives in: `scripts/mlx-server.py`.
- **Streaming-path adapter routing** — call to `hu_agent_m3_route_per_turn()` inserted into the streaming chat path. Lives in: `src/agent/agent_stream.c` (one new call site after first successful chunk receipt, mirroring the existing call at `src/agent/agent_turn.c:4224`).
- **Outcome ring populator** — new helper `hu_m3_record_outcome_from_provider_result()` invoked from inside `hu_agent_m3_on_provider_success()` (single function, called from 11 sites). Lives in: extension of `src/ml/m3_frontier_adapter.c`.
- **Outcome ring drainer** (NEW; resolves Q-M3-C) — production consumer of the outcome ring. Today the ring is write-only in production (`hu_m3_outcomes_to_jsonl()` has zero production callers per recon). New daemon-tick consumer `hu_daemon_tick_m3_outcome_drain()` calls `hu_m3_frontier_adapter_snapshot_outcomes()` periodically (default every 256 turns or 5 min, whichever first), persists to new SQLite table `m3_outcomes` (or emits to existing metrics counters), then resets the ring's drain marker. Lives in: `src/daemon.c` + extension of `src/ml/m3_frontier_adapter.c` for the drain marker.
- **Swap-failure observability** — structured-log emit + new metric counter `m3_adapter_swap_failure_total` (label: `reason ∈ {transport, http_4xx, http_5xx, missing_endpoint}`) + `hu_log_info_once()` first-failure landmark. Lives in: `src/ml/mlx_admin.c` (extend `hu_mlx_admin_swap_adapter`) + `include/human/metrics/*` for counter declaration.
- **Fidelity-based A/B gate** — extension of `scripts/m3_eval_adapter.py` and the C-side `metrics.fidelity` gateway method to score candidate adapter outputs on a held-out prompt set using `communication_style_fidelity_score` from `src/ml/fidelity.c`. New C function `hu_m3_ab_run_fidelity_gate(candidate_path, baseline_path, prompt_set_path, threshold)` returns PASS/FAIL with a structured report.
- **Single-command live-fire driver** — new `scripts/m3-live-fire.sh`: cold-starts daemon + MLX server, ingests a fixture DPO set from `tests/fixtures/m3/`, triggers training, swaps adapter, runs A/B gate, exits with structured status.
- **Daemon → real-MLX training bridge** — extension of the existing `hu_training_runner_enqueue_lora_persona()` path to take a target-model parameter (`frontier_mlx` vs `huml_reference`) and dispatch to either the in-process HUML trainer or the `m3_mlx_lora_bridge.py` subprocess. Lives in: `src/agent/lora_training_runner.c` + `src/ml/cli.c`.
- **Fixture DPO set** — new `tests/fixtures/m3/dpo_pairs_50.jsonl` containing ≥50 alpaca-DPO-shaped rows for the live-fire run and a held-out 20-prompt set `tests/fixtures/m3/holdout_prompts.jsonl` for the A/B gate.

## Data flow

```
[User reaction]
   │
   ▼
[reaction_handler → dpo_pairs table]
   │
   ▼
[Spec 2's pair-count trigger fires] ────────────────┐
   │                                                │
   ▼                                                │
[lora_training_runner: enqueue]                     │
   │                                                │
   ▼                                                │
[Branch: target = frontier_mlx]                     │
   │                                                │
   ▼                                                │
[subprocess: m3_mlx_lora_bridge.py against          │
 mlx-community/gemma-4-26b-a4b-it-4bit]             │
   │                                                │
   ▼ (writes ./adapters/<contact>/adapter.safetensors)
[post-training hook]                                │
   │                                                │
   ▼                                                │
[hu_mlx_admin_swap_adapter ──HTTP POST──▶            │
                                /v1/adapters/swap]   │
   │                                                │
   ▼                                                │
[Next user turn arrives, streaming OR non-streaming]│
   │                                                │
   ▼                                                │
[Provider chat / stream_chat]                       │
   │                                                │
   ▼                                                │
[On first success: hu_agent_m3_route_per_turn       │
  (NEW for streaming path)]                         │
   │                                                │
   ▼                                                │
[hu_agent_m3_on_provider_success                     │
  → hu_m3_record_outcome_from_provider_result        │
  → outcome ring buffer ++]                          │
   │                                                │
   ▼                                                │
[Periodic A/B job:                                  │
  hu_m3_ab_run_fidelity_gate                        │
  on held-out prompts                                │
  PASS iff Δfidelity ≥ threshold]                    │
   │                                                │
   ▼                                                │
[Live-fire script reports exit 0 if PASS] ◄─────────┘
```

## Decisions

- **D-M3-1 (AC-M3-1): Implement swap endpoint inline in `mlx-server.py` rather than delegating to gemma-realtime.** Chose inline implementation over delegation because: (a) removes a hidden external dependency, (b) matches the "your hardware, your model" product thesis (gemma-realtime is not packaged), (c) lets us version the swap contract ourselves. Tradeoff: we re-implement the swap mechanics (load LoRA weights into the in-memory model, swap layers, sanity-check). MLX provides primitives for this; the implementation is ~50 lines of Python.
- **D-M3-2 (AC-M3-2): Insert streaming-path swap AFTER first successful chunk, not before stream start.** Chose post-first-chunk over pre-stream because: pre-stream swap adds a synchronous HTTP roundtrip (~50-200 ms) to every turn even when no adapter rotation is needed, hurting perceived latency. Post-first-chunk only swaps when the model is already serving healthily. Tradeoff: the FIRST chunk of a turn after a fresh adapter is generated using the prior adapter; the rest use the new one. For multi-token responses this is acceptable; for single-token "👍" responses it means the rotation effectively starts on turn N+1. The contact-route table already records intended adapter, so missing one turn is recoverable. **Resolution of Q-M3-A:** recon confirms no async-task infrastructure exists in the codebase. The swap is a synchronous HTTP call on the streaming thread; post-first-chunk timing remains the choice over pre-stream. (Pre-stream is a viable alternative if you accept the per-turn latency.)
- **D-M3-3 (AC-M3-3): Swap failures emit structured log + metric + `hu_log_info_once` on first failure.** Chose triple-redundant observability because the 2026-05-18 silent-no-op incident proved a single signal is missable. The `hu_log_info_once` ensures the operator sees the failure even if metrics aren't being scraped.
- **D-M3-4 (AC-M3-4): Outcome ring populated AND drained, with a production consumer.** Chose call-site population over provider-impl population because: (a) the provider vtable boundary stays clean (providers shouldn't know about M3), (b) the existing `hu_agent_m3_on_provider_success()` function is the single canonical "success" point — called from 11 sites but the population call is inside the function, once. **Resolution of Q-M3-C:** recon confirmed the outcome ring has zero production consumers today. Populating without draining is a half-fix: the ring wraps silently after 4096 outcomes and the data is lost. This design adds a production drainer (new Component above) that persists snapshots to `m3_outcomes` SQLite or emits to metrics. AC-M3-4 implicitly expands to "populated AND drained"; tasks.md will sequence populate-then-drain.
- **D-M3-5 (AC-M3-5): Fidelity gate uses ABSOLUTE delta on `communication_style_fidelity_score`, default threshold 0.05.** Chose absolute over relative because the existing fidelity score is on [0, 1] and a relative "20% improvement" is misleading when baseline is near 0. Tradeoff: at very high baselines (>0.95) the absolute-delta threshold may be hard to clear; we accept that as appropriate — at near-ceiling fidelity, further "improvement" is dubious. **Risk: this assumes the fidelity score is calibrated for cross-adapter comparison; if `fidelity.c` produces a per-prompt score not suitable for cross-adapter aggregation, the gate needs prompt-set averaging. Design-time validation required (Risk-1).**
- **D-M3-6 (AC-M3-6): Live-fire is bash, not C.** Chose bash because: operators debug live-fire failures via shell; binary live-fire would obscure failures behind C errors. The script is glue, not logic.
- **D-M3-7 (AC-M3-7): Daemon auto-invocation goes through `m3_mlx_lora_bridge.py` subprocess.** Chose subprocess (already proven in commit 96238913) over in-process MLX bridging because: in-process would require linking the MLX C++ runtime into the daemon, blowing past the 1750-KB binary budget. Subprocess matches `~/.claude/rules/cross-language-via-http.md` — clean boundary, independently deployable.
- **D-M3-8: Tests use a fake `mlx_lm` subprocess shim under `HU_IS_TEST`.** No real GPU work in unit tests; the live-fire script is the only path that exercises real training. Tags AC-M3-6, AC-M3-7.

## Risks

- **Risk-1 (D-M3-5): Fidelity score calibration unknown.** `src/ml/fidelity.c` may produce scores not suitable for cross-adapter aggregation (e.g., per-prompt scores that need bootstrapping for variance). **Mitigation:** design-phase task to read `fidelity.c` and confirm score semantics. If unsuitable, fall back to: rank-by-mean across the 20-prompt set + paired bootstrap CI (5,000 resamples) for the threshold check.
- **Risk-2 (D-M3-1): MLX in-process adapter swap may not be hot-swappable across all LoRA configurations.** If MLX requires a model reload for some adapter shapes, our "swap" becomes "reload + serve" with multi-second latency. **Mitigation:** during design-phase read of `mlx_lm`, confirm `load_weights` supports our rank-16 adapter format. If not, document the constraint as a "swap may take up to N seconds" SLA and add a counter `m3_adapter_swap_latency_ms_histogram`.
- **Risk-3 (D-M3-2): Streaming path post-first-chunk swap loses one turn of personalization on adapter rotation.** **Mitigation:** acceptable per decision rationale; document explicitly in `docs/m3-adapter-routing.md` so future engineers don't re-litigate.
- **Risk-4 (AC-M3-6): Live-fire script becomes the only proof of E2E health; if it bit-rots, no other test catches regressions in the integration surface.** **Mitigation:** AC-M3-7's auto-invocation test (with fake mlx_lm) exercises the same code paths in `human_tests`; live-fire is the system-test layer above it. CI runs live-fire on a schedule (separate workflow from `ci.yml`).
- **Risk-5 (D-M3-7): Subprocess training is slow (~30 min for 50 iterations on M-series).** Tests cannot run real training. **Mitigation:** AC-M3-7's E2E test uses a fake mlx_lm shim that simulates "training" by copying a known-good safetensors fixture; the real run is gated to the live-fire script.
- **Risk-6 (multiple ACs): Six gaps and seven ACs is at the edge of one-spec scope.** **Mitigation:** the tasks.md will sequence implementation such that the first three tasks land observability + streaming-path closure (AC-M3-2, M3-3, M3-4), giving us early evidence of health before the harder daemon-auto-invocation work in AC-M3-7. Each AC has a discrete test gate; if any one slips, the others can still ship.

## Open design questions — RESOLVED

- **Q-M3-A: Streaming-path swap cancellability — RESOLVED.** No async-task infrastructure exists in the codebase (recon grep confirmed). The streaming loop in `agent_stream.c:195-283` runs in the request handler; sync HTTP call on that thread is acceptable. See D-M3-2 update.
- **Q-M3-B: Double-fire training policy — RESOLVED.** Existing learner-pending scheduler (`src/daemon.c:3506-3511` → `hu_w14_scheduler_enqueue_lora`) has **no de-dup or coalesce semantics today**. Both triggers enqueue independently; scheduler processes sequentially. Spec 2's AC-RL-6 ("exactly one enqueue when both fire") would be NEW behavior not matching existing semantics — see scope tension flagged separately.
- **Q-M3-C: Outcome ring drain — RESOLVED.** Ring has zero production consumers; `hu_m3_outcomes_to_jsonl()` is the only caller and has no production sites. D-M3-4 updated to add a production drainer as a required component, not an optional follow-up.
