---
title: M3 MLX Bridge — Execution Plan
status: active
last-status-refresh: 2026-05-25
owner: ML subsystem
created: 2026-05-17
parent: docs/plans/2026-05-10-m3-frontier-model-bridge.md
related:
  - CLAUDE.md (M3 row in Strategic Missions)
  - src/ml/m3_frontier_adapter.c
  - src/providers/mlx.c
  - src/providers/llamacpp.c
  - docs/plans/2026-05-11-init-04-mlx-qwen3-provider.md
last_audit: 2026-05-25
---

# Why this doc exists

The parent plan (`2026-05-10-m3-frontier-model-bridge.md`) is a great
honest accounting of what M3 means and where each bridge sits. It is
NOT a phased execution plan — it reads as five independent bridge
designs (A / A.0 / A.0.5 / B / C), each with internal phases.

This doc is the **phased, sequenceable, verifier-shaped execution
plan** for Bridge B (MLX) — the path the SOTA roadmap calls out as
"the primary personalization path for the M-series target market."

It opens with the observability slice that just landed because that
slice is a *precondition* for every later phase: the only honest way
to know a tensor-bearing implementation actually wired up is for the
seam to be observable from the test layer, and right now it is.

# Honest status (matches the next CLAUDE.md M3 line)

> **M3** — Bridge A daemon-pattern proven; **Bridge B first slice
> (probe-counter seam) landed 2026-05-17**; Bridge B inference,
> training, streaming, LoRA loading not yet started. Tensor
> personalization signal still gated on B1.

What this PR ships:
- `hu_m3_frontier_adapter_probe_infer` + `hu_m3_frontier_adapter_probe_count`
  (`include/human/ml/m3_frontier_adapter.h`, `src/ml/m3_frontier_adapter.c`).
- All 11 existing `hu_agent_m3_on_provider_success` chat-path call
  sites now mutate observable state — a regression that drops any of
  them is detected by `test_m3_agent_on_provider_success_advances_probe_count`.
- `hu_m3_frontier_adapter_noop_infer` retained as a backwards-compat
  shim — zero call-site churn for this slice.

What this PR explicitly does NOT ship:
- Any tensor computation. The "probe" is a counter; it is not
  personalization. The user's chat output is byte-identical to a
  build with M3 fully disabled. The seam being observable is a
  prerequisite for personalization, not personalization itself.
- Any MLX runtime linkage. The MLX provider remains a NOT_SUPPORTED
  stub. The llama.cpp provider remains a NOT_SUPPORTED stub until
  `HU_ENABLE_LLAMACPP=ON` and libllama is reachable.

# Phases

The phases are ordered by what *blocks what*. Each phase has a
verifier contract — a sentence describing the test that, when green,
constitutes "this phase shipped."

## Phase B-pre — Observability slice (DONE 2026-05-17)

- **Scope:** convert `noop_infer` to `probe_infer` + counter so the
  existing 11 chat-path call sites are testable.
- **Effort:** ~half a day.
- **Dependencies:** none.
- **Risk:** none — counter is allocated as part of the adapter
  struct, nothing else moves.
- **Verifier contract:** with a fixture adapter attached to an
  agent, calling `hu_agent_m3_on_provider_success(agent)` N times
  makes `hu_m3_frontier_adapter_probe_count(agent->m3_adapter)`
  equal to N.
  - Test: `tests/test_ml.c::test_m3_agent_on_provider_success_advances_probe_count`.
  - Plus three unit tests on the adapter directly: zero-start, increment
    semantics, NULL-safety.

## Phase B1 — MLX runtime linkage (SHIPPED via approach #1, verifier-contract test still owed)

**Status refresh 2026-05-25:** Subprocess approach #1 landed on
`src/providers/mlx.c:99-248` as `mlx_run_subprocess`. The vtable
methods `mlx_chat` (mlx.c:289) and `mlx_chat_with_system` (mlx.c:329)
both delegate to it. Gated by `HU_MLX_SUBPROCESS_ACTIVE`, which
requires the conjunction:

```c
HU_ENABLE_MLX_PROVIDER && __APPLE__ && __arm64__ && HU_GATEWAY_POSIX && !HU_IS_TEST
```

When any conjunct is false, vtable methods return
`HU_ERR_NOT_SUPPORTED` cleanly and never mutate `out` — the daemon's
fallback path relies on that invariant (pinned by
`test_provider_all.c::test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat`).

What shipped:
- Fork + pipe + exec of `python3 -m mlx_lm.generate --model … [--adapter-path …] --max-tokens N --prompt -`
- Prompt piped on stdin, output captured from stdout, 180s timeout
- Output trimming, error classification (`HU_ERR_TIMEOUT`,
  `HU_ERR_PROVIDER_RESPONSE`, `HU_ERR_OUT_OF_MEMORY`)
- Owned-copy model/adapter paths on the ctx with explicit-length
  free contract (CodeRabbit 2026-05-17 finding)

What's NOT shipped — the verifier-contract test:
- `tests/test_mlx_provider.c::test_mlx_chat_subprocess_round_trip`
  (gated on `HU_ENABLE_MLX_PROVIDER + __APPLE__ + __arm64__`).
  Without this test, "B1 works" is a code-reading claim, not a
  verifier-proven claim.

What's also NOT yet decided — approaches #2 and #3:
- The HTTP-client approach (#2) IS partially shipped via the
  `hu_mlx_admin_swap_adapter` family in `src/ml/mlx_admin.c`, which
  uses HTTP to manage an external mlx-server's adapter state. The
  chat path itself remains subprocess (#1). Whether to migrate the
  chat path to HTTP (amortizing Python startup) is a future
  decision — not blocking the M3 success criteria.
- Direct `libmlx` link (#3) explicitly punted.

## Phase B2 — Model loading (PARTIAL — ctx owns model path; config-driven wire still owed)

**Status refresh 2026-05-25:** `mlx_ctx_t` (`mlx.c:56-66`) already
owns `model_path_owned` + `adapter_path_owned`. `hu_mlx_provider_create`
(`mlx.c:522`) accepts a config and persists the model identifier.
The subprocess call site passes `--model <model_path_owned>` correctly.

What's NOT shipped:
- Config-parser wire for `personalization.mlx_model_path` (the field
  exists in the config struct but the parser may not surface a clean
  error message when the path is missing or unresolvable — needs
  audit + a fixture-driven test).
- The verifier-contract test `test_mlx_provider_create_resolves_model_path`
  — its absence means model-not-found error classification is a
  code-reading claim.

(rest of section retained from original spec)

## Phase B2 — Model loading (depends on B1) [original spec follows]

- **Scope:** `mlx_provider_create` accepts a model identifier
  (HuggingFace ID or local safetensors directory), validates it
  exists, and primes the subprocess / server with `--model <id>`
  ahead of the first chat call. Adds a config field
  `personalization.mlx_model_path` (parser + serializer + merge).
- **Effort:** ~1 week.
- **Dependencies:** B1.
- **Risk:** path expansion (`~/`), permission, model-not-found
  classification (NOT_SUPPORTED vs IO).
- **Verifier contract:** `hu_mlx_provider_create` with a present
  model returns HU_OK; with a missing path returns a stable
  classification (currently spec'd as `HU_ERR_NOT_SUPPORTED` to match
  the dispatcher safety contract).
  - Test: `test_mlx_provider.c::test_mlx_provider_create_resolves_model_path`.

## Phase B3 — Inference call (SHIPPED minus the verifier-contract test)

**Status refresh 2026-05-25:** The real `mlx_chat` body lives at
`src/providers/mlx.c:289-326` and delegates to `mlx_run_subprocess`.
Output is captured, trimmed, and returned as `hu_chat_response_t.content`
with provenance via `out->model`.

What's NOT shipped:
- The probe-counter outcome-metadata wire — `probe_infer` (B-pre)
  still only increments a counter; recording per-call token_count /
  latency_ms onto the adapter for the eventual learning signal is
  still owed.
- The verifier-contract tests:
  - `test_mlx_provider.c::test_mlx_chat_greedy_completion_matches_fixture`
  - `test_ml.c::test_m3_probe_count_advances_once_per_chat`
  Without these, "B3 works end-to-end" remains a code-reading claim.

## Phase B3 — Inference call (depends on B2) [original spec follows]

- **Scope:** real `mlx_chat` body — invoke the MLX runtime, capture
  generated tokens, parse into `hu_chat_response_t`. The probe
  counter from B-pre stays in place; the body of `probe_infer`
  gets a new wire-in that records inference outcome metadata
  (token count, ms) onto the adapter for the eventual learning
  signal.
- **Effort:** ~2 weeks (including streaming-vs-batch decision).
- **Dependencies:** B2.
- **Risk:** OOM on first big context, timeout handling, error
  propagation surface (Python traceback → C error code).
- **Verifier contract:** end-to-end chat call against a tiny MLX
  model produces a deterministic-greedy completion of a known
  prompt. Probe counter advances exactly once per successful chat.
  - Test: `test_mlx_provider.c::test_mlx_chat_greedy_completion_matches_fixture`.
  - Plus an adapter-side test: `test_ml.c::test_m3_probe_count_advances_once_per_chat`.

## Phase B4 — Streaming (SHIPPED through Sprint 55 Phase 3 — 2026-05-25 PM refresh)

**Status refresh 2026-05-25 (PM):** Status from the earlier 2026-05-25
refresh below is OUTDATED. Reality:

- `mlx_vtable.stream_chat = mlx_stream_chat` IS wired (src/providers/mlx.c:852).
- `supports_streaming` returns true when HU_MLX_SUBPROCESS_ACTIVE.
- Phase 1 shipped in Sprint 54 commit `034fb7c2` (vtable + 14 tests).
- Phase 2 shipped in Sprint 55 commit `e86a08a2` (UTF-8 chunk-safety
  helpers extracted to src/providers/mlx_stream_utf8.c + 16 unit tests
  pinning the partial-codepoint contract).
- Phase 3 shipped in Sprint 55 commits `324d6024` + `3badf6e6`
  (env-gated live mlx_lm subprocess integration script;
  proven locally at 109 tok/s against the 26B MoE model).

The "biggest user-visible gap" claim is also outdated — daemon
production traffic goes through mlx_local (HTTP server on port 8741),
NOT through mlx_run_subprocess. The subprocess streaming path is now
fully wired but exercised primarily for batch / offline use cases.
The HTTP path is what the live daemon hits, and that path is
streaming-capable via mlx_server.py.

What remains for B4: convert the production daemon's mlx_local path
to consume streamed tokens (latency win) rather than waiting for the
full response. That's a different design (mlx_server SSE response
shape) and a separate spec — not "wire streaming for the subprocess."

### Original 2026-05-25 AM status (kept for history)

`mlx_vtable.stream_chat` was reported NULL.
`supports_streaming` was reported false. The "biggest user-visible gap"
framing applied when this section was written before the Sprint 54
US-M3-B4 Phase 1 work landed. See Sprint 54 retro + audit for the
honest narrative.

This phase is the next high-leverage M3 work after B3's verifier-
contract tests land.

## Phase B4 — Streaming (depends on B3) [original spec follows]

- **Scope:** wire `mlx_vtable.stream_chat` to the subprocess'
  token-by-token output, fan it through `hu_chat_response_chunk`
  the same way `llamacpp.c` does. Wire `supports_streaming` to
  return true.
- **Effort:** ~1 week.
- **Dependencies:** B3.
- **Risk:** chunk boundary correctness, cancellation, partial-UTF-8
  at chunk edges.
- **Verifier contract:** streaming a 50-token completion delivers
  N≥2 chunks summing to the same string a batch chat would return,
  and a mid-stream cancel cleanly terminates the subprocess.
  - Test: `test_mlx_provider.c::test_mlx_stream_chat_chunks_equal_batch`.
  - Test: `test_mlx_provider.c::test_mlx_stream_chat_cancellation_terminates_subprocess`.

## Phase B5 — LoRA adapter loading (SHIPPED minus the bias-measurement verifier-contract test)

**Status refresh 2026-05-25:**
- `mlx_load_adapter` lives at `mlx.c:425`. Accepts an `adapter_path`,
  validates `adapters.safetensors` exists in the directory, persists
  to `mlx_ctx->adapter_path_owned`.
- `hu_mlx_provider_active_adapter_path` returns the currently-loaded
  path or NULL (`mlx.c:476`).
- `mlx_unload_adapter` clears the active adapter (`mlx.c:485`) —
  fixes the bugbot finding 2026-05-16 about NULL `unload_adapter`.
- `--adapter-path` is correctly threaded into the subprocess invocation
  when set (`mlx.c:128-131`).
- Three existing tests in `tests/test_mlx_load_adapter.c` cover
  load + active-path + unload paths.
- `tests/test_mlx_admin.c` covers the HTTP-side adapter swap that
  fires when an mlx-server is running (separate code path,
  complementary).

What's NOT shipped:
- The bias-measurement verifier-contract test
  `test_mlx_lora_adapter_biases_completion` — proving the loaded
  adapter MEASURABLY changes greedy output on a held-out prompt.
  Without this test, "B5 works" is a code-reading claim. (This is
  the test that empirically validates the strategic-mission item
  "LoRA adapter measurably improves persona fidelity" — it's the
  evidence link, not a wire.)
- The malformed-adapter rejection test
  `test_mlx_lora_adapter_malformed_safetensors_rejected`.

## Phase B5 — LoRA adapter loading (depends on B3) [original spec follows]

- **Scope:** wire `mlx_load_adapter` and `mlx_active_adapter`.
  Accept an `adapter_path` (safetensors LoRA from `mlx_lm.lora`),
  pass to the subprocess as `--adapter-path`, return active ID via
  the vtable. Bridges `personalization.lora_adapter_path` from the
  daemon's config-driven auto-load into the live MLX chat path.
- **Effort:** ~1 week.
- **Dependencies:** B3.
- **Risk:** safetensors validation, adapter / base shape mismatch,
  rollback path (when `unload_adapter` is called mid-conversation).
- **Verifier contract:** loading a known-good adapter measurably
  changes greedy output on a held-out prompt versus the same model
  without the adapter. Loading a malformed adapter returns
  `HU_ERR_INVALID_ARGUMENT` and leaves the active adapter NULL.
  - Test: `test_mlx_provider.c::test_mlx_lora_adapter_biases_completion`.
  - Test: `test_mlx_provider.c::test_mlx_lora_adapter_malformed_safetensors_rejected`.

# What success looks like

M3 is shipped — in the sense the CLAUDE.md row can drop the
"narrative doesn't match code" honesty caveat — when **all five
items** below are true.

1. Phase B5 verifier contract is green. (Loading a persona-LoRA
   adapter measurably changes the model the user actually chats with
   on Apple Silicon.)
2. `human ml lora-persona --backend mlx` trains a real LoRA against
   a real MLX base from the user's persona bank, end-to-end, with
   the adapter persisted to `~/.human/adapters/<persona-id>/`.
3. The daemon, with `provider: "mlx"` configured, auto-loads the
   trained adapter at startup and applies it to every chat turn.
4. A nightly persona-fidelity eval (`src/eval/persona_fidelity.c`)
   shows the adapter improves fidelity scores by a non-noise margin
   versus the base model on a held-out persona-anchor set.
5. CLAUDE.md M3 row updates from "Bridge A daemon-pattern proven" to
   "Personal LoRA fine-tune on Apple Silicon ships a measurable
   persona-fidelity delta against the frontier base."

Items 1-3 are coding work in this plan's phases. Item 4 plugs into
the existing fidelity harness; the work is choosing a stable anchor
set and writing the cron entry. Item 5 is a one-line CLAUDE.md edit
made when 1-4 are green.

# What's actually next (2026-05-25 status refresh)

The 2026-05-17 plan undersold what would ship in the following 8
days. Reality vs the plan as written:

| Phase | Plan claim (2026-05-17) | Actual state (2026-05-26 refresh) |
|---|---|---|
| B-pre  | ✅ Done                 | ✅ Done |
| B1     | NOT STARTED            | ✅ Subprocess implementation SHIPPED + verifier-contract test SHIPPED (`test_mlx_chat_subprocess_round_trip`, gated apple+arm64) |
| B2     | depends on B1 (~1 week) | ✅ Subprocess: ctx + create + path threading + verifier test (`test_mlx_provider_create_resolves_model_path`) SHIPPED. The plan-doc "config-driven wire owed" claim was stale — production runs through `mlx_local` HTTP provider whose URL is already config-driven via `cfg->providers[i].base_url`; no production wire is missing. |
| B3     | depends on B2 (~2 weeks)| ✅ Inference call + greedy completion test (`test_mlx_chat_greedy_completion_matches_fixture`) + probe-counter advance test (`test_m3_probe_count_advances_once_per_chat_{single,multiple}_call`) SHIPPED. Probe-counter outcome metadata (token_count + latency_ms + hashes + model_id + adapter_id) also SHIPPED via `hu_agent_m3_record_chat_outcome`. |
| B4     | depends on B3 (~1 week) | ✅ Phase 1-3 SHIPPED via Sprint 54 + 55 (subprocess vtable wired + UTF-8 helpers extracted + live script proven at 109 tok/s on 26B MoE). **The only remaining B4 work is the production daemon's `mlx_local` HTTP path consuming SSE chunks instead of waiting for full response** — see `docs/plans/2026-05-26-m3-b4-mlx-local-sse/` for the spec stub. |
| B5     | depends on B3 (~1 week) | ✅ load/active/unload adapter + 3 unit tests + bias-measurement test (`test_mlx_lora_adapter_biases_completion`) + malformed-safetensors negative path test (`test_mlx_lora_adapter_malformed_safetensors_rejected`) SHIPPED. Bias-measurement test skips cleanly when the `seth-lora-v4-repair-*` fixture is absent. |

**The narrative inversion (resolved 2026-05-26):** the M3 plan read as
"B1 → B2 → B3 → B4 → B5" serial blocks. The actual delivery was
"B1 + B3 + B5 in parallel via the subprocess pipe, with the verifier-
contract tests landing across Sprint 54-55 + the 2026-05-25 US-15 +
US-8 + 2026-05-26 Phase B3 work." All six verifier-contract tests from
the original backlog have shipped.

**The "closes the loop" wire (NEW 2026-05-26):** US-8 (commit `416e6c29`)
fired training_loop.py from the DPO pair-count trigger, but stopped at
"adapter written to disk." This was filled in 2026-05-26 by wiring
`hu_mlx_admin_swap_adapter` immediately after the successful subprocess
exit. Observability via the new `hu_training_runner_post_train_swap_attempts()`
public counter — increments on every dispatch that reached the swap
step (under HU_IS_TEST the libcurl call is skipped, counter still
increments so tests pin the wire). Pinned end-to-end by
`test_lora_training_runner_frontier_mlx_path_increments_swap_counter`.

## Original next-PR backlog (FULLY RESOLVED 2026-05-26)

All 6 items from the backlog have shipped — leaving this list intact
as a historical reference:

1. ✅ `test_mlx_chat_subprocess_round_trip` (B1 verifier contract).
2. ✅ `test_mlx_provider_create_resolves_model_path` (B2 verifier contract).
3. ✅ `test_mlx_chat_greedy_completion_matches_fixture` (B3 verifier contract).
4. ✅ `test_m3_probe_count_advances_once_per_chat_*` (B3 adapter wire).
5. ✅ `test_mlx_lora_adapter_biases_completion` (B5 bias measurement).
6. ✅ `test_mlx_lora_adapter_malformed_safetensors_rejected` (B5 negative path).

## What's left for M3 (as of 2026-05-26)

The remaining genuine M3 work, in order of payoff:

1. **B4 mlx_local SSE streaming** — production daemon consumes SSE
   chunks from `mlx_server.py` instead of buffering the full response.
   Latency win. Spec stub at `docs/plans/2026-05-26-m3-b4-mlx-local-sse/`.
   ~1-week implementation arc.
2. **Live empirical re-validation of the post-train swap on Apple
   Silicon** — the existing `test_mlx_chat_greedy_completion_matches_fixture`
   skips cleanly on non-Apple-Silicon runners; first apple-arm64
   developer to pull this should run it end-to-end with a current
   v4-repair adapter present.
3. **`HU_NIGHTLY_LORA_ENABLED` plumbing into config.json** — current
   nightly retrain (daemon.c:4026) is env-gated. Promote to
   `cfg->learning.nightly_lora.enabled` so operators don't need env
   vars. Small follow-up; not blocking M3.

# Out of scope here

- Bridge A (llama.cpp) phases beyond what already shipped — tracked
  in the parent plan.
- Bridge C (any-open-weight unified provider) — explicitly
  punted in the parent plan.
- Continuous-learning / nightly retraining cron — that is SOTA roadmap
  Phase A5 work; the MLX bridge feeds into it, not the other way
  around.
- iOS/iPadOS MLX inference — separate harness and lifecycle.
