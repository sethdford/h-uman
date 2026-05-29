# Option A: Sentinel-Boundary Reply-First LoRA — Design

**Date:** 2026-05-29
**Status:** Approved (brainstorming → spec)
**Adapter name:** `seth-lora-v5-replyfirst-<runid>`
**Feeds:** writing-plans → training plan

---

## Problem

Streaming wiring is shipped and deployed (per-tier `stream_strip`, `harmony_filter`,
`streaming_enabled=true` default). The remaining blocker to SOTA *realtime* on-device
streaming is **generation order**, not filtering:

- v4-repair front-loads deliberation and emits the user-facing reply **last**.
- Measured: a casual "hey, you around?" generated **151 tokens for a ~10-token reply**
  → `streaming_beneficial: false`.
- Filtering (harmony_filter / server StreamThoughtFilter) only addresses *cleanliness*.
  Buffering already yields clean output. Neither makes the reply arrive *sooner*.

**First-token latency is an ordering problem.** To win it, the reply tokens must be
generated first. That is what this LoRA teaches.

## Goal

Train a LoRA so that on the **streamed tiers** (REFLEXIVE / CONVERSATIONAL) the model
emits the user-facing reply FIRST, followed by a sentinel boundary and an optional
deliberation block:

```
<reply text><|channel|>thought
<deliberation text>
```

The streamed path emits the reply incrementally (true first-token win) and discards
everything from the `<|channel|>` boundary onward. ANALYTICAL / DEEP tiers are unaffected
(they are buffered via `stream_strip=+1`, keeping think-first cognition where it matters).

## Non-Goals

- Changing ANALYTICAL/DEEP behavior. Those stay think-first + buffered.
- Replacing the harmony filter. The LoRA emits a boundary the **existing** filter already
  recognizes; no filter code change is in scope (a Task-0 spike confirms the exact bytes).
- Stacking adapters. v5 is a fresh standalone LoRA on the base model.
- A new training framework. Reuse the established `training_loop.py` mlx_lm path.

---

## Architecture

A fresh standalone LoRA on `mlx-community/gemma-4-31b-it-4bit` trained on a
**self-distilled, reordered** corpus. v4-repair's voice is preserved by construction:
the reply text in each training target is byte-identical to what v4-repair already
produces — only the *order* (reply-before-deliberation) and the inserted sentinel change.

### Why self-distill + reorder (vs. teacher-generated deliberation)

- The reply text is unchanged from v4-repair → persona voice preserved by construction,
  minimizing regression from the +27pp baseline.
- A teacher model (e.g. Gemini) writing fresh deliberation would inject a second voice and
  risk drift. Rejected.
- Reorder-and-drop-deliberation collapses toward "no deliberation at all," which loses the
  beat-of-reasoning benefit on borderline-casual turns. Rejected.

### Why a new adapter on base (vs. continue-training v4-repair)

The reordered self-distilled corpus already carries v4-repair's voice, so a fresh adapter
at `scale=2.0` re-learns voice + new order together. Cleaner lineage; avoids mlx_lm adapter-
stacking fragility.

---

## Components

Five units, each independently testable.

### 1. `scripts/build_replyfirst_corpus.py` (highest risk)

- **Input:** prompts from `~/.human/training-data/m3-corpus.jsonl` (user→assistant turns).
- **Distillation:** runs v4-repair **offline** via `mlx_lm.generate` (NOT the shared `:8741`
  production server — a concurrent eval is running there). Deterministic (`--temp 0.0`).
- **Parse:** splits each generation into `(deliberation, reply)`. v4-repair deliberates
  *markerless* on many prompts, so the splitter is heuristic:
  1. If the output contains a recognizable channel marker (confirmed by Task-0), split there.
  2. Else fall back to "final coherent paragraph = reply; preceding text = deliberation."
  3. If no deliberation is detectable (pure casual reply), deliberation is empty and the
     target is `reply + <|channel|>thought\n` (boundary present, deliberation minimal).
- **Reorder + sentinel:** emit target `<reply><|channel|>thought\n<deliberation>`.
- **Output:** SFT JSONL `{"text": "<prompt-template>\n<reordered-target>"}` matching
  `training_loop.py`'s expected `{"text": ...}` schema (see `training_loop.py:611-634`).
  Writes a train split and a held-out split (the held-out split is disjoint from the
  fidelity eval's held-out fixtures).

### 2. `scripts/train_replyfirst.py`

- Thin wrapper over the established `python -m mlx_lm lora` path in `training_loop.py`.
- Hyperparameters (matching v4-repair recipe): `rank=8`, **`scale=2.0` explicit**,
  `dropout=0.0`, `learning_rate=1e-5`, `batch_size=1`, `num_layers=8`,
  `max_seq_length=2048`, `iters` tunable (start 500).
- Base: `mlx-community/gemma-4-31b-it-4bit`.
- **Post-train assertion:** reads `adapter_config.json` and FAILS the run if
  `lora_parameters.scale != 2.0` (per `lora-scale-default-or-die.md` — mlx_lm 0.31.2
  default is the catastrophic 20.0).
- Writes adapter `seth-lora-v5-replyfirst-<runid>`; appends `adapter_lineage.jsonl`.

### 3. `scripts/eval_ordering.py` (new gate)

- For each held-out casual prompt, generate with the v5 adapter (offline, deterministic).
- Metrics per prompt: position (token index) of the first reply token; whether the reply
  precedes any deliberation/marker (reply-first boolean).
- Aggregate: `% reply-first`, `median first-reply-token index`.
- Verdict JSON (schema mirrors `eval_fidelity_nightly.py`): `verdict`, `exit_code`,
  `n_prompts`, `adapter_path`, `pct_reply_first`, `median_first_reply_token_idx`,
  `gate.ordering_pass`.

### 4. Reuse `scripts/eval_fidelity_nightly.py` (existing gate)

- Point its adapter path at v5. Existing bootstrap-CI gate (100 resamples, one-sided
  t-test α=0.025, practical floor) measures the persona-fidelity delta.
- For this project the practical floor is **+22pp** (within 5pp of v4-repair's +27pp).

### 5. E2E streaming proof (SOTA artifact)

- Serve v5 (offline or a dedicated non-prod mlx instance), stream a casual prompt through
  the same path production uses (`stream_strip=-1` → incremental).
- Assert: (a) first reply token arrives early (latency win), (b) trailing deliberation is
  stripped by the filter in discard mode (no `<|channel>thought` leak), (c)
  `streaming_beneficial: true`.
- Emits a proof artifact (verdict JSON + sample transcript) under the project's results dir.

---

## Data Flow

```
m3 prompts
  → v4-repair generate (offline mlx_lm.generate, --temp 0.0)
  → split (deliberation, reply)        [build_replyfirst_corpus.py]
  → reorder + <|channel|> sentinel
  → SFT JSONL (train + heldout splits)
  → mlx_lm lora (scale=2.0)            [train_replyfirst.py]
  → seth-lora-v5-replyfirst-<runid>
  → { fidelity eval, ordering eval, e2e streaming proof }
  → verdict JSONs
  → SHIP GATE (both fidelity ≥+22pp AND ordering ≥90% reply-first)
```

## Acceptance Gate (strict — both must pass)

| Dimension | Threshold | Tool |
|-----------|-----------|------|
| Ordering  | ≥ 90% of held-out casual prompts reply-first **AND** first reply token within first 8 generated tokens | `eval_ordering.py` |
| Fidelity  | Δ ≥ +22pp on bootstrap-CI gate (within 5pp of v4-repair's +27pp) | `eval_fidelity_nightly.py` |

Ship the v5 adapter (wire `personalization.lora_adapter_path`) **only if both pass.**

## Error Handling

- **Distillation parse fails** (cannot split a generation): drop that example from the
  corpus, log the prompt + raw output to a `parse_failures.jsonl` for inspection. Do not
  emit a malformed target.
- **Scale != 2.0** in `adapter_config.json`: abort the training run, do not produce an
  adapter (hard fail).
- **Either gate fails:** do not ship. Verdict JSON `verdict: FAIL`, exit non-zero. Iterate
  on iters/corpus size; re-run gates.
- **Shared `:8741` busy / down:** all distillation and eval use offline `mlx_lm.generate`,
  never the production server, so a busy/stale server never corrupts results.

## Testing

### Unit (stdlib only, no live model/judge — importlib pattern, matches `test_eval_multiturn_local.py`)
- `build_replyfirst_corpus`: given a synthetic `(deliberation, reply)` parse, reorder
  produces the correct `<reply><|channel|>thought\n<deliberation>` target.
- `build_replyfirst_corpus`: marker-present split vs. markerless-fallback split vs.
  empty-deliberation case each produce the expected target.
- `eval_ordering`: ordering-metric math (first-reply-token index, % reply-first) over
  fixture generations.
- Verdict assembly: JSON shape for both `eval_ordering` and the e2e proof.

### Live gates (Apple Silicon, NOT in CI — too slow / needs the model)
- Fidelity eval (existing harness).
- Ordering eval.
- E2E streaming proof.

## Risks

| Risk | Severity | Mitigation |
|------|----------|------------|
| Markerless deliberation hard to split into (delib, reply) | High | **Task-0 spike** characterizes v4-repair output on ~20 prompts and pins the exact `<|channel|>` byte sequence the harmony filter strips, BEFORE building the corpus. Robust fallback splitter. Parse-failure drops, not malformed targets. |
| Reorder teaches degenerate empty deliberation | Low | Acceptable — casual turns *want* minimal deliberation. The boundary still trains. |
| Fidelity regresses below +22pp | Medium | Strict floor gate catches it pre-ship; iterate corpus/iters. |
| Disturbing concurrent prod eval on `:8741` | Medium | All distillation/eval offline via `mlx_lm.generate`; never touch the shared server. |
| Scale drift to 20.0 | High | Post-train `adapter_config.json` assertion; hard-fail. |

## Open Questions (resolve in Task-0 spike)

1. Exact byte sequence of the boundary marker the harmony filter strips in **discard**
   mode (`<|channel|>thought` vs `<|channel>thought` vs `<|return|>`). Pin against
   `tests/test_harmony_filter.c`.
2. Does v4-repair emit ANY marker natively, or is it always markerless? (Determines whether
   the splitter's marker branch ever fires, or it's fallback-only.)

## References

- `~/.claude/rules/lora-scale-default-or-die.md` — scale=2.0 enforcement.
- `scripts/training_loop.py:637-941` — established mlx_lm lora path + scale handling.
- `scripts/eval_fidelity_nightly.py` — fidelity gate, verdict schema, bootstrap CI.
- `src/util/harmony_filter.c`, `include/human/util/harmony_filter.h` — marker grammar.
- `/Users/sethford/Documents/gemma-realtime-1/scripts/mlx-server.py` — StreamThoughtFilter
  (server-side, modes strip/discard/off).
- `~/.human/training-data/` — corpus + adapter_lineage.jsonl.
- v4-repair baseline: +27pp fidelity (0.586 → 0.856), commit `9ab9b86e`.
