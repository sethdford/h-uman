# US-15 — Empirical Eval Harness Run Status

**Date:** 2026-05-26
**Status:** ⏳ RUNNING in background (started 2026-05-25 ~18:18)
**Expected completion:** ~19:30-20:00 (1-1.5 hour wall clock)
**Output:** `docs/plans/2026-05-26-sprint-56-gemma-as-seth/results/us15-empirical-verdict.json` (will appear when complete)

## Why this is the load-bearing M3 SOTA evidence

Sprint 55 shipped the strategic test (`test_mlx_lora_adapter_biases_completion`)
but it SKIPS in HU_IS_TEST builds because the MLX subprocess returns
HU_ERR_NOT_SUPPORTED. Sprint 56 shipped the eval harness
(`scripts/eval_fidelity_nightly.py`) but didn't run it. This US-15 actually
runs it against the live mlx_lm.generate subprocess + v4-repair adapter — the
first numeric measurement of whether the adapter measurably improves persona
fidelity on Seth-style held-out prompts.

## What's running

```bash
python3 /Users/sethford/Projects/h-uman/scripts/eval_fidelity_nightly.py \
  --adapter-path /Users/sethford/.human/training-data/adapters/seth-lora-v4-repair-20260525-071921 \
  --output-json /Users/sethford/Projects/h-uman/docs/plans/2026-05-26-sprint-56-gemma-as-seth/results/us15-empirical-verdict.json
```

The harness:
- Loads 25 held-out Seth-style fixture prompts from
  `docs/plans/2026-05-26-sprint-56-gemma-as-seth/data/heldout-prompts.jsonl`
- For each prompt, invokes `mlx_lm generate --model gemma-4-31b-it-4bit --prompt <text>
  --max-tokens 80 --temp 0.0` ONCE WITHOUT adapter, ONCE WITH adapter
- Scores each response with the deterministic shape classifier (reuses
  `scripts/eval_shape_classifier.py`)
- Computes per-prompt persona-fidelity delta
- Runs bootstrap CI (N=100 resamplings, seed=42, deterministic)
- Applies the SOTA gate: PASS iff (post_mean > pre_mean + 1.96 × stderr,
  one-sided α=0.025) AND (post_delta ≥ 0.05, practical significance floor)

## Why it's slow

Each `mlx_lm generate` invocation loads the 31B model (4-bit quantized,
~17 GB) into memory, runs greedy inference for up to 80 tokens, then exits.
Wall-clock is dominated by model load (30-60s per call) not inference itself.
50 invocations total. The harness doesn't pool — each subprocess is
independent — so total wall-clock is ~50 × 1min = ~50 min minimum, often
longer with cold start.

A future optimization: keep mlx_lm hot by talking to the running
mlx-server.py instead of forking subprocesses. That would reduce per-call
cost from 60s to ~2s. Not in scope for US-15 (the harness was designed for
nightly cron runs where cold start is acceptable).

## How to check status

```bash
# Is the eval still running?
ps aux | grep eval_fidelity_nightly | grep -v grep

# Is the current mlx_lm subprocess making progress?
ps aux | grep mlx_lm | grep -v grep

# Did the verdict land?
cat docs/plans/2026-05-26-sprint-56-gemma-as-seth/results/us15-empirical-verdict.json
```

## What the verdict will look like

The harness writes a JSON document with:
- `pre_mean`, `pre_stderr`, `pre_ci` — base model alone scores
- `post_mean`, `post_stderr`, `post_ci` — base + v4-repair adapter scores
- `delta`, `delta_ci` — per-prompt deltas + bootstrap CI
- `verdict` — "PASS", "FAIL", or "SKIP" with rationale
- `gate_statistical` — whether one-sided α=0.025 holds
- `gate_practical` — whether delta ≥ 0.05 holds

If `verdict: PASS`:
  ✅ M3 mission empirically validated — v4-repair adapter measurably
  improves persona fidelity. This is the FIRST numeric proof for the
  CLAUDE.md M3 mission ("LoRA adapter that measurably improves persona
  fidelity on inference").

If `verdict: FAIL`:
  ⚠ Adapter did NOT measurably improve fidelity on this fixture set.
  Options: (a) widen fixture set, (b) train a stronger adapter using
  Sprint 56 US-8's training_loop.py against real DPO outcomes, (c)
  investigate whether the shape classifier captures the dimensions
  the adapter actually optimizes.

If `verdict: SKIP`:
  Eval couldn't run (mlx_lm error, adapter not found, etc.). Output
  will name the reason.

## Operator follow-up

Once the eval completes:
1. Read the verdict JSON.
2. If PASS, document the delta in `~/.claude/projects/.../memory/` and
   announce "M3 mission empirically validated" — the first time the
   project has had numeric proof.
3. If FAIL, decide between (a) training improvement (US-8 loop) or
   (b) eval-harness tuning (different scoring).
4. Wire the launchd plist
   (`com.human.eval-fidelity-nightly.plist`) to run nightly at 02:00
   for ongoing tracking. See Sprint 56 designs/US-9.md DoD.
