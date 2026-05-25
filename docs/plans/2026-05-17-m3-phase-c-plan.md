---
title: M3 Personalization Loop — Phase C Plan
status: deferred
owner: ML subsystem
created: 2026-05-17
parent: docs/plans/2026-05-17-m3-personalization-loop-closeout.md
related:
  - CLAUDE.md (M3 row)
  - scripts/training_loop.py
  - scripts/m3_outcome_driver.py
  - src/agent/agent.c (hu_agent_m3_record_chat_outcome)
  - src/ml/m3_frontier_adapter.c
last_audit: 2026-05-25
---

# Why this doc exists

Phase B closed the dormant loop — outcomes flow, the driver selects,
the adapter swaps. The simulate-train mode lets us prove the wiring
without spending wall-clock on a GPU pass.

**Phase C swaps simulate for real.** That moves M3 from "we proved the
plumbing" to "we measurably improved Seth's reply quality with on-device
training." This is the slice CLAUDE.md flags as the *real* technical
challenge for M3.

# Honest current state

| Item | Current | Phase C target |
|---|---|---|
| Token counts in outcomes | ✅ C1 (9bbf67f2): provider `usage` block populates outcome.pt/ct | Done |
| model_id / adapter_id | ✅ C2 (a2939365): persisted id-map, model_id=1 in live ring | Done |
| Training trigger | ✅ C3 (4a8fd60c): `--source-jsonl` wire + FNV-1a + DB resolution + dry-run safetensors | Done |
| Adapter artifact | ✅ C4 (2026-05-18): real 131KB LORA-magic binary from `lora-persona` | Done |
| MLX server | Stub (responds in 150ms, no model) | Real gemma-4-31b loaded with the new adapter (separate bridge slice) |
| Validation | ✅ C5 (2026-05-18): MetadataJudge with pass/no-change/regress/fail; live-fire reports PASS | Done (LLM-judge for production gate is sft-prompts judge slot) |
| Chat-path stability | OK on release preset | C6 — DEFERRED (clean repro in m3-c6-asan-repro.log) |

# Phases

Ordered by what blocks what. Each phase has a verifier contract — a
single test that, when green, constitutes "this phase shipped."

## Phase C1 — Token counts from provider `usage` (small)

**Why first:** The bytes/4 estimate is a 20-40% under-counter for
English BPE and worse for structured/non-English text. Real counts
unblock honest per-turn cost reporting AND make `pt/ct` filters
work the way users expect (`completion_tokens > 8` actually means
8 tokens, not 32 bytes).

**Changes:**

- `hu_provider_chat_response_t` already carries
  `usage.prompt_tokens` / `usage.completion_tokens` from providers
  that emit them (OpenAI, MLX, Anthropic).
- `hu_agent_m3_record_chat_outcome` accepts these as new optional
  args; when nonzero, uses them; when zero, falls back to bytes/4.
- All 12 call sites updated to pass the usage block.

**Verifier:**
`tests/test_ml.c::test_m3_record_chat_outcome_prefers_usage_block_when_present`
— records two outcomes from the same prompt: one with `usage.completion_tokens=99`,
one with usage=0. Asserts ring outcome shows `ct=99` for the first and
`ct=prompt_len/4` for the second.

## Phase C2 — model_id / adapter_id mapping (small)

**Why now:** Outcome clustering for the training loop needs to know
which model produced which outcome. Currently both fields are 0
(unknown), so the training loop can't distinguish "Seth said X with
the base model" from "Seth said X with adapter v3."

**Changes:**

- `personalization` config gets a small table: `model_id_map` and
  `adapter_id_map` (string path → uint16). Persisted to disk so the
  ids are stable across daemon restarts.
- `hu_agent_m3_record_chat_outcome` resolves the active provider's
  model + adapter into these ids at record time.

**Verifier:**
`tests/test_ml.c::test_m3_record_outcome_assigns_stable_model_id_per_config`
— records two outcomes with the same active model; asserts both
land with the same nonzero `m` value; reloads config; asserts the
id is still the same.

## Phase C3 — `training_loop.py --source-jsonl` (medium)

**Why now:** The driver's `run_training()` shells out to
`scripts/training_loop.py` with a `--source-jsonl` flag that doesn't
exist yet. The driver works in real mode TODAY only because
`--simulate-train` short-circuits the call.

**Changes:**

- `training_loop.py` gets `--source-jsonl <path>` and
  `--adapter-out <path>` flags. When `--source-jsonl` is provided,
  the script:
  1. Resolves prompts to full text via the conversation DB (the JSONL
     only carries hashes — see B1 privacy design)
  2. Builds an SFT batch (skipping samples whose prompt_hash isn't
     resolvable — meaning the conversation got deleted; expected
     edge case)
  3. Runs LoRA on the existing reference HUML GPT path
  4. Saves the adapter at `--adapter-out` (caller decides the
     filename; lets the driver embed a timestamp)

**Verifier:**
`scripts/test_training_loop_source_jsonl.py` — feeds a 4-sample JSONL
+ a stub conversation DB; runs training_loop.py with both flags;
asserts the output safetensors exists, has the right LoRA shape
(`A`/`B` rank-decomposition tensors present), and trained for at
least one step.

## Phase C4 — Real LoRA train against live outcomes (large)

**Why now:** This is the slice that ACTUALLY personalizes the model.
Up to here we've been proving plumbing.

**Changes:**

- A first-cut warmup run: accumulate 32 outcomes from real Seth
  conversations (via the live-fire script extended with
  `--seed-from-history` that loads from the conversation DB), kick
  off `training_loop.py`, watch the adapter emerge.
- Wall-clock target: ≤60s end-to-end for a 32-sample warmup on M3
  Max. Real training depends on the C3 implementation's LoRA rank
  and dataset preparation cost.

**Verifier:**
The same `scripts/live_fire_m3_loop.sh` script, BUT with
`--simulate-train` removed AND a new step 11 that snapshots the
adapter's tensor count via `/v1/adapters/current?include_tensor_names=1`.
Pass iff `tensors_loaded ≥ rank × num_lora_layers` (i.e. a real
non-trivial LoRA, not just a swap of an empty file).

## Phase C5 — A/B eval: outcome-trained vs baseline (medium)

**Why now:** Without an eval gate, we can't tell whether the loop is
*helping* or *hurting* the model's quality. We've been measuring
plumbing, not quality.

**Changes:**

- New eval suite under `eval_suites/m3-personalization/`:
  - 20-30 held-out prompts from Seth's real conversation patterns
  - Reference completions = Seth's actual replies (held out from
    training)
  - Judge: blinded GPT-5 grading on persona-fidelity (tone, vocab,
    response shape)
- `make m3-eval` runs the suite against:
  - `pre`  — base model + no adapter
  - `post` — base model + the outcome-trained adapter
- Surfaces the delta with bootstrap CIs.

**Verifier:**
`scripts/eval_m3_personalization.py` — asserts
`post_score > pre_score + 1.96 × stderr` (one-sided, α=0.025).
Skips with informative output (not failure) when the model isn't
loaded.

## Phase C6 — Chat-path ASan fix (medium, sidecar — DEFERRED)

**Status (2026-05-18):** Reproduced cleanly with `ASAN_OPTIONS=halt_on_error=1`.
Full trace in `docs/research/m3-c6-asan-repro.log`. **Deferred** to a
focused investigation slice — see "Repro recipe" below.

**Why deferred:** The bug fires in the thread pool worker that handles
chat completions (T3 → `_platform_memset+0x6c`, 208-byte memset to a
stack address). It's gated to ASan dev builds; the release binary
(used in production and live-fire) is unaffected. Phase C4 + C5
(the load-bearing slices that personalize the model) ship first.

**Repro recipe:**
```
cmake --build --preset dev --target human
ASAN_OPTIONS=halt_on_error=1:abort_on_error=0 ./build/human service-loop --with-gateway &
python3 scripts/stub_mlx_server.py --port 8741 &
curl -X POST http://127.0.0.1:3006/v1/chat/completions \
  -d '{"model":"mlx_local","messages":[{"role":"user","content":"hi"}]}'
# Daemon aborts with: BUS on unknown address in _platform_memset+0x6c
# Stack: thread pool worker T3, created by hu_gateway_run thread pool
```

**Investigation hints:**
- Crash size is `0xd0` (208 bytes) — search for memsets of structs in
  that size range
- Caller's `lr=0x0000000104a4ad50` symbolize with `atos -o ./build/human
  -l <slid_base> 0x0000000104a4ad50` (slid base derivable from
  pthread_create address in the trace)
- The chat handler path likely involves: `hu_openai_compat_handle_chat_completions`
  → `hu_agent_turn` → some deeply nested error-handling branch
- Try the "fix obvious cases first" pass: any place that does
  `memset(&local_struct, 0, sizeof(local_struct))` where `local_struct`
  was declared in a tighter scope than where the memset runs

**Why now (original):** Discovered during live-fire setup. ASan caught
a stack-use-after-scope inside the chat path under load. Not on the
loop's critical path (the release binary doesn't have ASan), but
worth fixing for the dev preset to be usable.

**Changes:**

- Reproduce with `bash scripts/live_fire_m3_loop.sh` using a dev
  binary (currently crashes on chat #1)
- Use ASan's backtrace to locate the offending stack lifetime
- Fix without changing the calling pattern

**Verifier:**
`scripts/live_fire_m3_loop.sh` with `BIN=build/human` (the dev
binary) completes all 10 steps green.

# Sizing

| Phase | LOC est | Wall-clock est |
|---|---|---|
| C1 (token counts) | ~80 LOC C + 1 test | 1-2 hours |
| C2 (model/adapter id map) | ~150 LOC C + 1 test | 2-4 hours |
| C3 (training_loop.py --source-jsonl) | ~200 LOC Python + 1 test | 3-6 hours |
| C4 (real warmup train) | ~50 LOC integration + 1 verifier | 4-8 hours (mostly wall-clock for training itself) |
| C5 (A/B eval) | ~200 LOC Python + suite | 1-2 days |
| C6 (ASan fix) | ~5-20 LOC C + verifier | 2-6 hours |

Total: 1-2 weeks of focused work. C1+C2+C3 are mechanical; C4 is the
load-bearing one for actually proving personalization works; C5 is
the gate; C6 is hygiene.

# Decision: Phase C scope

Phases C1–C4 are the **minimum viable Phase C** — that's the
self-consistent definition of "we measurably personalize the model."
C5+C6 are explicit follow-ups but should land in the same sprint to
avoid the "loop runs but nobody knows if it helps" trap.

# Open questions for Phase C kickoff

1. **LoRA rank**: 16 (compact, fast) or 32 (more capacity, slower)
   for the C3 default? The reference HUML GPT path uses rank=16;
   keeping parity reduces variance.
2. **Sample threshold for first real warmup**: currently driver
   default is 32 outcomes. Real LoRA training is more stable around
   100-200 samples. Bump the default for C4 or run the warmup with
   `--threshold 32` and accept higher variance?
3. **Adapter retention policy**: every train produces an artifact in
   `~/.human/training-data/adapters/m3-driver-{ts}.safetensors`. Do
   we keep all of them (size grows linearly) or rotate (last N)?
4. **What does success on the A/B eval look like?** A 5% bump on
   persona-fidelity might be in the noise band of a 30-sample eval
   set. Probably need to scale the eval set or the effect threshold.

These are explicit decision points for the C4/C5 design slice.
