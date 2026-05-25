# Design for US-9: Nightly fidelity eval harness + SOTA gate

## Approach

US-9 gates the M3 sprint's fidelity claim via a statistical proof: the trained LoRA adapter produces persona-fidelity responses with measurable improvement over the base model. The harness runs nightly, loads 20-30 held-out iMessage prompts from Seth's real conversation history (stratified by date to avoid training contamination), executes two passes (base-only, base+adapter), scores each response via the deterministic shape classifier in `src/eval/persona_fidelity.c`, and computes a one-sided bootstrap confidence interval. SOTA gate triggers when both (a) statistical significance holds at α=0.025 and (b) practical significance (post_delta ≥ 5%) is met.

This design reuses infrastructure from the 2026-05-18 eval audit: `eval_sota_scorecard.py` provides the bootstrap-CI formula, `eval_shape_classifier.py` provides scoring, and the existing conversation-DB schema supports held-out prompt stratification. The harness orchestrates two-pass inference via subprocess calls to the C daemon's eval-runner, toggles adapter on/off via the MLX provider's runtime adapter-load mechanism, and logs gate verdicts for production observability.

**Why this design, not alternatives:**
- **Pure-Python harness vs C-side eval runner**: Python harness allows flexible prompt sourcing (SQLite queries, JSONL fixtures), simpler A/B orchestration, and easier stats-library integration. C-side would require new eval-runner variants per gate; Python is cheaper.
- **Held-out prompt source**: Real iMessage prompts from `~/.human/memory.db` (date-stratified, excluding recent 30d to prevent training leakage). Fallback: curated JSONL fixture if DB unavailable. This ensures fidelity measures "Seth's real style," not a synthetic test set.
- **Bootstrap resampling N=100 vs other CI methods**: matches the existing 2026-05-18 methodology already validated on fidelity scores; consistent bar with prior SOTA gate.
- **SOTA gate semantics (both AC-9.5 AND AC-9.6 must hold)**: one-sided t-test at α=0.025 catches noise; 5% absolute floor catches effects too small to be meaningful (practical significance). Conjunction prevents false claims.
- **Cron entry**: launchd plist on macOS (where MLX runs), scheduled nightly at 2am when load is low. Script exits nonzero on FAIL; launchd sends digest email if configured.

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `scripts/eval_fidelity_nightly.py` | New harness: load held-out prompts, run two-pass evals, compute bootstrap CI, gate verdict | +280 |
| `scripts/test_eval_fidelity_nightly.py` | Unit tests: mock DB, fixture JSONL, verify gate logic | +120 |
| `scripts/eval_fidelity_helpers.py` | Extract shared bootstrap + shape-score logic; reuse from sota_scorecard | +40 |
| `.launchd/com.human.eval-nightly.plist` | macOS launchd agent for nightly scheduling | +25 |
| `docs/plans/2026-05-25-m3-sota-sprint/designs/US-9-implementation-log.md` | Decision log: held-out-prompt stratification, gate threshold tuning, cron platform choice | +50 |

## Implementation steps (for the implementer agent)

1. **Create eval_fidelity_helpers.py**: extract `bootstrap_ci()` and `persona_fidelity_score()` from existing scripts into a shared module. Signature: `(values, n_resamples=100, confidence=0.975) -> (mean, lower_ci, upper_ci)`.

2. **Implement eval_fidelity_nightly.py skeleton**:
   - Parse CLI args: `--adapter-path`, `--model-id`, `--output-json`, `--held-out-db-path`, `--held-out-fixture-jsonl`
   - Load held-out prompts: query `~/.human/memory.db` (exclude last 30d, stratify by channel), fall back to fixture JSONL
   - Return early with informative skip if <20 prompts available

3. **Wire two-pass orchestration**:
   - Pass 1 (base): invoke `human eval run --model <model-id> --prompts <held-out>` with adapter unloaded
   - Pass 2 (adapter): invoke same with `--adapter-path <path>` 
   - Capture stdout JSON; extract `mean_fidelity_score` from each pass
   - Fail if either pass returns zero scores (no responses)

4. **Implement gate logic**:
   - Compute delta = post_mean - pre_mean
   - Compute bootstrap CI on per-prompt deltas (100 resamplings)
   - Verdict A (statistical): one-sided test `post_mean > pre_mean + 1.96 * stderr` 
   - Verdict B (practical): `delta >= 0.05`
   - Final verdict: PASS iff (A AND B), else FAIL or SKIP (informative)
   - Log gate verdict with all inputs to stdout and to `~/.human/logs/eval-nightly.log`

5. **Add unit tests** (`test_eval_fidelity_nightly.py`):
   - Mock DB query: return 25 stratified fixtures
   - Test case: pre_mean=0.10, post_mean=0.20, delta=0.10 → PASS
   - Test case: pre_mean=0.10, post_mean=0.12, delta=0.02 → SKIP (below 5%)
   - Test case: pre_mean=0.10, post_mean=0.11 (CI overlaps) → FAIL (not significant)
   - Test edge case: 0 prompts returned → SKIP with reason "held-out set empty"

6. **Create launchd plist**:
   - Service: `com.human.eval-nightly`
   - Schedule: daily at 02:00 (low load)
   - Invocation: `python3 /Users/sethford/Projects/h-uman/scripts/eval_fidelity_nightly.py --adapter-path ~/.human/training-data/adapters/seth-lora-v4-repair --model-id mlx-community/gemma-4-31b-it-4bit --output-json ~/.human/eval-nightly-results.json`
   - Stdout/stderr: redirect to `~/.human/logs/eval-nightly.log`

7. **Run full suite of tests** (inherited from US-8 training_loop.py): verify that eval_fidelity_nightly.py can be imported, gates execute without crashes, logs contain gate verdict.

## Risks

- **Held-out contamination (MEDIUM/MEDIUM)**: if training data includes recent iMessage turns, held-out prompts may overlap training set. **Mitigation**: stratify by date (exclude last 30d); audit training-data cutoff commit and verify held-out window is disjoint. Add pre-flight check: `SELECT COUNT(DISTINCT prompt_hash) FROM conversations WHERE ts >= (NOW - 30d)` must be ≤ 3% of training-set prompts.

- **Fidelity score correlates poorly with naturalness (MEDIUM/MEDIUM)**: shape classifier is deterministic but may not capture Seth's actual voice preferences. **Mitigation**: shape classifier was already validated on 2026-05-18 audit (shape-score moved from 0.053 → 0.350 post-fix, matching observed qualitative voice improvement). Supplement gate report with 3-sample human spot-check ("does adapted output sound like Seth?") — not a fail criterion, but a red flag if classifier and human diverge.

- **Bootstrap CI too narrow on N=20 prompts (MEDIUM/SMALL)**: with only 20 samples, CI may be wide; gate could fail even when true effect exists. **Mitigation**: accept wider CI; increase N to 25-30 if feasible. Running gate on subsets (per-channel stratification) can also surface if effect is channel-specific.

- **Gate too strict or too lenient (LOW/MEDIUM)**: α=0.025 + 5% practical floor are principled but untested on LoRA-adapter deltas. **Mitigation**: run gate on historical eval data from US-8 training; if existing adapter produces delta_mean=0.10 ± 0.05, gate thresholds are reasonable. If delta is actually 0.02 post-fix, loosen practical floor to 0.02 or accept α=0.05.

- **Prompt source unavailable at eval time (LOW/SMALL)**: DB locked by daemon, fixture JSONL missing. **Mitigation**: gate exits with SKIP reason "held-out prompts unavailable" (not FAIL). Operator sees that eval didn't run; no false-negative gate block.

- **MLX model path or adapter safetensors invalid at eval time (MEDIUM/SMALL)**: adapter or model download incomplete. **Mitigation**: eval_fidelity_nightly.py returns SKIP with the specific error ("adapter path does not exist", "model download timeout"). Operator can retry or disable nightly run.

## Test strategy

| Test | Scope | Expected outcome |
|---|---|---|
| `test_eval_fidelity_nightly_gate_pass` | Mock: pre=0.10, post=0.20, delta=0.10 | Verdict PASS; CI non-overlapping |
| `test_eval_fidelity_nightly_gate_fail_practical` | Mock: pre=0.10, post=0.12, delta=0.02 | Verdict SKIP (delta < 0.05) |
| `test_eval_fidelity_nightly_gate_fail_statistical` | Mock: pre=0.15, post=0.18, CI overlaps | Verdict FAIL (not significant) |
| `test_eval_fidelity_nightly_empty_prompts` | Mock: 0 prompts in held-out set | Verdict SKIP with reason |
| `test_eval_fidelity_nightly_bootstrap_ci` | Unit test of `bootstrap_ci()` helper | CI matches scipy.stats.bootstrap |
| `scripts/test_eval_fidelity_nightly.py` | Integration: real eval subprocess, mock model | Produces valid JSON output with gate verdict |

## Acceptance criteria mapping

- **AC-9.1** → `eval_fidelity_nightly.py`: load 20-30 held-out prompts from `~/.human/memory.db` (date-stratified, excluding recent 30d)
- **AC-9.2** → two-pass orchestration: invoke eval runner with/without adapter, capture responses
- **AC-9.3** → reuse `eval_shape_classifier.py` scoring on each response, mean per pass
- **AC-9.4** → `bootstrap_ci()` function: 100 resamplings, returns (mean, lower, upper)
- **AC-9.5** → gate logic: one-sided test `post_mean > pre_mean + 1.96 * stderr` at α=0.025
- **AC-9.6** → gate logic: PASS iff `post_delta >= 0.05` (5% floor) AND statistical significance; else FAIL or SKIP

## Out of scope

- Continuous nightly retraining (Phase C4). Gate only runs; humans decide whether to retrain.
- Streaming inference (Phase B4). Eval runs synchronous/blocking.
- Director compression or DPO judge integration. Uses shape classifier only.
- Multi-user adapter routing or HuLa integration.
- iOS/Android eval harness (macOS only).
- Automated gate-triggered retraining or model deployment.

---

**Status**: Skeleton design complete. Ready for codebase investigation and refinement.
