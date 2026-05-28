# Longitudinal On-Device Personalization — SOTA Flag — Tasks

**Status:** DRAFT — pending approval.
**Builds on:** requirements.md + design.md in this directory.

## Decomposition principles
- Each task is independently revertable in one commit.
- Each task maps to ≥1 AC; each AC is covered by ≥1 task.
- Sized per `~/.claude/rules/agent-task-sizing.md` (≤8 mechanical sites, 30–90
  min). Python harness work mirrors the existing `scripts/eval_*.py` +
  `scripts/test_*.py` pattern.
- Ship order C2/C3 → C1 → C4 → C5 → C6 (gate logic provable in CI first).

## Tasks

| # | Task | Comp | ACs | Depends | Status |
|---|---|---|---|---|---|
| **T1** | Create `scripts/data/base-capability-probes.jsonl`: 10–15 frozen general-ability probes (JSON-extraction, translation, arithmetic/sort, exact-format) each with a `check` spec (type: `json_valid`/`regex`/`exact`/`numeric`). Add `scripts/eval_base_capability.py` with `score_base_capability(responses) -> (per_probe, mean)` using deterministic checkers only (no LLM judge). Record the probe-set sha256. | C2 | AC-3 | — | pending |
| **T2** | `[parallel with T1]` Add `evaluate_trajectory_gate(generations, cfg)` as a PURE function (own module, no I/O) implementing: curve-rising (within-1-stderr-of-running-max), base-capability floor (`gen0 − ε`), final-floor (≥0.80). Returns `{verdict, failing_axis, failing_gen, details}`. SKIP when <2 gens. | C3 | AC-2, AC-3, AC-4 | — | pending |
| **T3** | `scripts/test_eval_base_capability.py`: pin each checker type (valid/invalid JSON, right/wrong translation stub, correct/incorrect numeric), probe-set hash stability, and mean computation. ≥8 assertions, no tautologies. | C2 | AC-3 | T1 | pending |
| **T4** | `scripts/test_trajectory_gate.py`: pin every gate branch on synthetic generation lists — (a) rising curve + base preserved → PASS; (b) a gen dropping >1 stderr below running max → FAIL naming that gen; (c) a **`scale=20`-replay** gen (fidelity up, base-capability below floor) → FAIL on base axis; (d) final-gen below 0.80 → FAIL; (e) single gen → SKIP. This is the CI proof the SOTA gate is honest. | C3 | AC-2, AC-3, AC-4 | T2 | pending |
| **T5** | `scripts/eval_personalization_trajectory.py`: orchestrator. Reuse `run_eval_pass` + `compute_persona_fidelity_scores` (fidelity) and `score_base_capability` (T1). Take an ordered generations manifest (JSON: gen index, label, adapter_path|null, train_pairs, ts). Per-generation **cache** keyed by `(adapter_path, fixture_sha, probeset_sha)`; compute only uncached gens. Emit `trajectory.json` (schema per design C1) + call `evaluate_trajectory_gate`. Exit codes mirror `eval_fidelity_nightly.py` (0 PASS/SKIP, 1 FAIL, 2 DEFERRED). | C1 | AC-1, AC-4 | T1, T2 | pending |
| **T6** | `scripts/test_eval_personalization_trajectory.py`: with `generate`/inference monkeypatched to fixtures (no model), assert trajectory.json schema, caching (uncached gen computed once, cached gen skipped), and that the embedded verdict matches `evaluate_trajectory_gate` on the same data. | C1 | AC-1 | T5 | pending |
| **T7** | Extend `scripts/eval_sota_scorecard.py` with a "Personalization trajectory" section: read `trajectory.json`, render per-gen table (gen, label, fidelity mean±CI, base-capability, train_pairs), combined verdict, and failing-axis note. Works in plain + `--markdown`. Add a scorecard test asserting the section renders for a fixture trajectory.json. | C4 | AC-6 | T5 | pending |
| **T8** | `[parallel with T7]` Extend `scripts/build_heldout_corpus.py` with `--synthetic`: emit `heldout-synthetic.jsonl` — invented contacts/banter, zero real handles/numbers/PII (reuse the existing PHONE/URL/NAME redaction as a post-filter assertion that the synthetic output contains none). Add a test asserting the synthetic fixture is PII-free and loadable by `load_held_out_prompts_from_jsonl`. | C5 | AC-5 | — | pending |
| **T9** | Write `protocol.md` (this dir): the seeded, step-by-step trajectory run recipe; how to reproduce on the synthetic fixture; and the explicit relationship between the internal shape-fidelity metric and LaMP (time-based splits, primary anchor) + PersonaGym (secondary) — with the verified citations from design.md. State plainly that the synthetic run proves methodology, the real claim is always on real held-out data. | C5 | AC-5, AC-8 | T5, T8 | pending |
| **T10** | Wire the loop: extend `scripts/live_fire_m3_full_loop.sh` (or add `scripts/m3_trajectory_advance.sh`) so that after `outcomes_to_dpo → dpo_mlx_train → genK adapter`, it appends genK to the generations manifest and runs `eval_personalization_trajectory.py`. **Gate the hot-swap on the verdict**: only `hu_mlx_admin_swap_adapter` genK when the trajectory verdict is PASS (rising + base preserved); on base-capability FAIL, keep gen K-1 and log the failing axis. No new training logic. | C6 | AC-4, AC-7 | T5 | operator/impl |
| **T11** | `[operator, live macOS]` Run the trajectory end-to-end on real held-out data with the existing adapters as gen0=base, gen1=v4-repair, producing the first real `trajectory.json` + scorecard section. Confirms AC-1/AC-2/AC-4 empirically on the live box (cannot be CI'd — needs mlx-server + 31B). | cross | AC-1,AC-2,AC-4 | T5, T7 | operator |

## Coverage matrix
| AC | Tasks |
|---|---|
| AC-1 (trajectory time-series) | T5, T6, T11 |
| AC-2 (curve rising within CI) | T2, T4, T11 |
| AC-3 (base-capability guard) | T1, T3, T4 |
| AC-4 (combined verdict) | T2, T4, T5, T10, T11 |
| AC-5 (reproducible + synthetic fixture) | T8, T9 |
| AC-6 (scorecard section) | T7 |
| AC-7 (loop wiring + swap gate) | T10 |
| AC-8 (public-bench anchor) | T9 |

## Dependency graph
```
[C2/C3]  T1 ─┬─ T3
             └─ T5 ── T6
         T2 ─┴─ T4
[C1]     T5 ── T7 ─┐
[C5]     T8 ───────┴─ T9
[C6]     T5 ── T10
[live]   T5,T7 ── T11
```

## Parallelism (per agent-task-sizing)
- **Sprint A (gate, CI-only):** T1 ∥ T2 → T3 ∥ T4. Pure-logic + deterministic;
  fully verifiable with no model. **This sprint alone proves the SOTA gate is
  honest, including the `scale=20` regression catch.**
- **Sprint B (harness):** T5 → T6.
- **Sprint C (report + repro):** T7 ∥ T8 → T9.
- **Sprint D (loop + live):** T10 → T11.
None of the parallel batches exceeds the 8-site cap.

## Verification gate (before declaring spec complete)
1. All tasks `completed` in TaskList (T11 is operator/live).
2. `/verify` on the Python suites: `scripts/test_trajectory_gate.py`,
   `test_eval_base_capability.py`, `test_eval_personalization_trajectory.py`
   all green; the `scale=20`-replay test (T4c) FAILS the gate as designed.
3. Full `human_tests` C suite stays 0-fail / 0-ASan (no C regressions from any
   touchpoint).
4. `spec-verifier` on this directory: per-AC PASS/FAIL.
5. **Live (operator, T11):** first real `trajectory.json` shows gen0→gen1 rising
   with base-capability preserved, rendered in the scorecard. AC-1/2/4 confirmed
   on the live box.

## Out-of-band notes
- T11 is the only LIVE-ONLY task (needs mlx-server + the 31B model). Everything
  else — including the entire SOTA-gate honesty proof — is CI-verifiable.
- Activation-steering on the local tier (verified feasible-with-work via
  `mlx_fun`) is a SEPARATE follow-up spike, intentionally excluded here.
- Signal-quality of implicit feedback (reward-hacking, "no reply" ≠ "bad") is an
  upstream follow-up; this spec *catches* its symptoms via the gate but does not
  fix signal design.
