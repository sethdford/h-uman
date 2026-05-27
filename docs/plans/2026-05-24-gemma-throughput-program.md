---
title: Gemma Throughput Program — Tier 1 + Tier 2 Execution Plan
status: deferred
owner: ML + inference subsystem
created: 2026-05-24
parent: docs/plans/2026-05-10-sota-roadmap-6mo.md
related:
  - docs/plans/2026-05-10-sota-roadmap-6mo.md            # Track B (Performance SOTA)
  - docs/plans/2026-05-10-m3-frontier-model-bridge.md    # Spec-decode shares draft adapter w/ M3
  - docs/plans/2026-05-17-m3-mlx-bridge-execution-plan.md
  - docs/plans/2026-05-11-init-13-kv-compression.md      # ABANDONED — lessons applied here
  - src/providers/llamacpp.c
  - src/providers/llamacpp_kvcache.c
  - src/providers/mlx.c
  - scripts/mlx-server.py
  - scripts/bench-gemma-perf.py
  - include/human/persona.h
last_audit: 2026-05-25
---

# Why this doc exists

The 6-month SOTA roadmap (`2026-05-10-sota-roadmap-6mo.md`) names the
right north-star metrics for Gemma inference (TTFT ≤ 200 ms cached;
conversational decode ≥ 100 tok/s; analytical decode ≥ 35 tok/s via
spec decode) and the right Phase B program to hit them. It does NOT
sequence the work so a reader can pick up tomorrow and ship.

This is that sequencing — **a tiered, integration-site-grounded,
verifier-shaped execution plan** for the throughput program. It was
written after a four-stream parallel research pass (Apple proprietary
APIs, 2025-2026 academic papers, OSS landscape, practitioner social
signal) on 2026-05-24. The cross-stream triangulation produced a
shorter, more honest priority list than any single stream did.

Three structural caveats from that research drive the priority order:

1. **Memory bandwidth, not FLOPS, is the M-series bottleneck.** M4 Max
   tops out at 546 GB/s. Every optimization is judged on
   bytes-moved-per-token, not FLOPS-per-token. This is why
   self-speculative decoding tops out at ~1.35× on Apple Silicon while
   reaching 2.5×+ on NVIDIA: doubling memory traffic on a
   bandwidth-bound system buys nothing. Cross-model spec decode (a
   small external draft, not a self-draft) is the only spec-decode
   shape that profits on M-series.
2. **You don't fully control scheduling.** CoreML, the ANE scheduler,
   P/E-core dispatch, and the GPU command queue make decisions you
   can't override. Leverage lives in *which framework you call* (MLX
   vs llama.cpp vs Accelerate) far more than in *what assembly you
   write inside it*. This rules out the "rewrite the hot loop in
   ARM64 SVE" instinct; it routes us toward backend selection +
   data-layout work.
3. **ANE direct access is a research moonshot.** Conflicting evidence:
   the Orion project (arXiv 2603.06728) claims 170 tok/s for
   Gemma-class on M4 Max via private `_ANECompiler` APIs; Apple's own
   measurements show 9 tok/s on 8B-class via ANE. Until Apple
   announces a public LLM-shaped ANE API (watch WWDC 2026), it's track,
   not act.

# Honest status of what's currently shipped

Audited 2026-05-24 against actual code, not memory:

| Capability | State today (file:line) | Gap |
|---|---|---|
| System-prompt prefix cache | [llamacpp_kvcache.c:42-49](../../src/providers/llamacpp_kvcache.c#L42) — FNV1a on `system_prompt` only | **Doesn't include persona overlay or example bank.** Hash misses every time the persona renders any per-turn variation upstream of the system prompt. |
| KV cache reuse | [llamacpp.c:237-260](../../src/providers/llamacpp.c#L237) — records hash + `n_past`, BUT still re-decodes on hit (per the comment at :251 "the prefix-skip optimization is Phase 3+") | The hash hit is recorded but not yet *used* to skip decode. Bigger one-line win than it looks. |
| KV quantization | None. KV is FP16 in llama.cpp Metal default. | INT8 via `--cache-type-k q8_0 --cache-type-v q8_0` (llama.cpp) or `QuantizedKVCache` (MLX) is a one-flag change. |
| Flash Attention on Metal | `grep -i flash src/providers/llamacpp*.c CMakeLists.txt` → no hits | Probably already enabled by llama.cpp default but not visibly verified in our build. Confirm + bench. |
| Streaming on MLX provider | [mlx.c:513-514](../../src/providers/mlx.c#L513) — `supports_streaming = NULL, stream_chat = NULL` | The MLX provider is wired as a one-shot subprocess (`python3 -m mlx_lm.generate`). Every call pays cold model load. |
| Speculative decoding | None in llama.cpp wrapper; none in MLX path | llama.cpp has had `--draft-model` for >18 months; we don't pass it. |
| Persona-aware prefix | Persona overlay is applied per-turn in [agent_turn.c:3404,3566](../../src/agent/agent_turn.c#L3404) but the rendered persona prefix isn't surfaced as a stable cache key | Cache miss every turn. |
| MLX backend default | We invoke `mlx_lm.generate` per-call (subprocess) AND we run `mlx-server.py` separately | Two MLX paths with different perf characteristics; no benchmark comparing them to llama.cpp Metal head-to-head on our actual persona prompts |

The init-13 (DeltaKV/SWAN) abandonment doc has the right lesson and
this plan honors it: **prove the integration path is reachable BEFORE
writing the codec.** Every Tier-1 item below either (a) flips a flag
in an upstream that already handles the heavy math, or (b) extends a
hash function we already own.

# North-star metrics — six-week scoreboard

| ID | Metric | Baseline (audited 2026-05-24) | Tier-1 target (week 6) | Tier-2 target (Q3 end) |
|---|---|---|---|---|
| T1 | Gemma-3-4B decode TPS (M4 Max, batch=1, persona prompt) | TBD — gap: `bench-gemma-perf.py` reports tps but `prompt_tokens`/`completion_tokens` are wired to 0 in [mlx-server.py:324](../../scripts/mlx-server.py#L324). FIX THIS FIRST. | +25% over honest baseline | +60% over honest baseline (spec decode with aligned draft) |
| T2 | TTFT, warm persona turn | 670 ms (per SOTA roadmap) | ≤ 250 ms | ≤ 200 ms |
| T3 | KV RSS at 2K context | FP16 baseline TBD | −40% (INT8) | −60% (INT4 once TurboQuant lands) |
| T4 | Persona-prefix cache hit rate | 0% (hash doesn't include persona) | ≥ 80% on multi-turn conversations | ≥ 95% |
| T5 | Spec-decode acceptance rate on Tier-1 channels | n/a | ≥ 25% (unaligned cross-model draft) | ≥ 50% (persona-aligned draft, gated on A3 milestone) |

Bench harness extension: T1/T2 are already measured by
`scripts/bench-gemma-perf.py`. T3 needs a `--measure-rss` flag (one
day's work). T4 needs an instrumented counter on the kvcache hit/miss
paths (already structurally there at
[llamacpp_kvcache.c:54-62](../../src/providers/llamacpp_kvcache.c#L54)).
T5 needs draft-acceptance logging in the spec-decode wrapper.

---

# Phase 0 — Honest measurement (Week 1, 2-3 days)

**Why this is Phase 0, not Phase 1:** every Tier-1 number above is "TBD"
until the bench harness reports real prompt/completion token counts
and RSS. You cannot optimize what you cannot measure. Three of the
four research streams independently flagged this same gap (the
inline-server's `usage: 0,0,0` response masks tok/s metrics).

| # | Slice | File:line | Test |
|---|---|---|---|
| 0.1 | Fix `usage` reporting in inline path | [scripts/mlx-server.py:324](../../scripts/mlx-server.py#L324) — wire prompt_tokens, completion_tokens from tokenizer | New: `tests/test_mlx_server_swap.py::test_chat_completion_reports_real_token_counts` |
| 0.2 | Add `--measure-rss` to bench | `scripts/bench-gemma-perf.py` — sample `/proc/self/status` or `task_info` per request | Manual: re-run baseline, persist `/tmp/bench-baseline-2026-05-24.json` |
| 0.3 | Add KV cache hit-rate counter | [src/providers/llamacpp_kvcache.c](../../src/providers/llamacpp_kvcache.c) — atomic counters; expose via daemon `/health` extension | `tests/test_llamacpp_kvcache.c::kvcache_hit_counter_increments_on_match` |
| 0.4 | Persist baseline | Run `bench-gemma-perf.py --tag baseline-2026-05-24` against M4 Max Gemma-3-4B and Gemma-3-31B-4bit; commit to `tests/fixtures/bench/` | — |

**Phase 0 exit:** `bench-gemma-perf.py --compare baseline new` shows real
deltas across all five metrics. Every subsequent phase's exit is "T_n
moved by ≥ X% from baseline."

---

# Tier 1 — Ship in the next 6 weeks

## Phase 1 — INT8 KV quantization (Week 1-2, ~3 days)

**Why first:** Triangulated across all four research streams. Touches
no application code; flips an upstream flag. ~50% KV RSS drop, ~10-15%
TPS gain at batch=1 (bandwidth-bound). Composable with everything
below — no later phase has to redo it.

| # | Slice | File:line |
|---|---|---|
| 1.1 | Wire `--cache-type-k q8_0 --cache-type-v q8_0` in the llama.cpp launch path | [src/providers/llamacpp.c](../../src/providers/llamacpp.c) construction. Add `kv_quant` enum to provider config (`fp16` / `q8_0` / `q4_0`). |
| 1.2 | Wire MLX `QuantizedKVCache` for the mlx-server.py inline path | [scripts/mlx-server.py:286-305](../../scripts/mlx-server.py#L286). The `mlx_lm.generate` call takes a `kv_bits=8` kwarg in v0.30+. |
| 1.3 | Config plumbing | `include/human/config.h` — add `inference.kv_quant` block (default `fp16` for safety, opt-in to `q8_0`). Parser + validator. |
| 1.4 | Quality gate | Run `scripts/run-gemma-sanity-gate.sh` against [tests/fixtures/gemma_sanity_gate_prompts.json](../../tests/fixtures/gemma_sanity_gate_prompts.json) with FP16 vs Q8 KV; require score delta ≤ 2% per the SOTA roadmap's "±2% at chat lengths" budget. |

**Phase 1 exit:**
- T3 (KV RSS) down ≥ 40%
- T1 (TPS) up ≥ 8%
- Sanity gate delta ≤ 2%
- Q8 path defaults ON for `personalization.kv_quant.enabled = true` users; FP16 retained as escape hatch

**Risk + mitigation:**
- R1: Q8 KV regresses on long-context (>4K) reasoning. *Mitigation:* gate
  Q8 to ≤2K context for first ship; raise after eval.
- R2: llama.cpp build flag differs across our vendored vs. system
  llama.cpp. *Mitigation:* Phase 1.1 hardcodes the flag at provider
  init and asserts the runtime support; fail-fast with a named error.

## Phase 2 — Persona-prefix prompt cache (Week 2-4, ~5 days)

**Why second:** Phase 1's KV-quant win compounds with this — both reduce
bytes-moved per warm turn. This phase delivers B3.1 from the SOTA
roadmap (the existing plan has the design; this phase executes).

**The key insight from the kvcache audit:** the FNV1a hash at
[llamacpp_kvcache.c:42-49](../../src/providers/llamacpp_kvcache.c#L42)
is keyed on `system_prompt` bytes alone. The persona overlay is
applied upstream of the system prompt in
[agent_turn.c:3404](../../src/agent/agent_turn.c#L3404) and varies per
channel and per warmth level — so today's cache misses every multi-turn
persona conversation. Extending the hash key is **the single largest
TTFT win in the entire program** because it doesn't change algorithms,
it makes the algorithm we have actually fire.

| # | Slice | File:line |
|---|---|---|
| 2.1 | Extend the cache key to `(persona_id, channel, overlay_hash, examples_hash, system_prompt_hash)` | New helper `hu_persona_prefix_hash(persona, channel, overlay, banks)` in `src/persona/`. Replace the call at [llamacpp.c:245-247](../../src/providers/llamacpp.c#L245). |
| 2.2 | Surface the new hash through the chat call | `hu_provider_chat_request_t` gains a `persona_prefix_hash` field; provider uses it instead of re-hashing the system prompt. |
| 2.3 | Implement the actual prefix-skip on hit | [llamacpp.c:251](../../src/providers/llamacpp.c#L251) currently no-ops on hit ("re-decodes; the hash-hit signal is the foundation for Phase 3+"). Wire `llama_decode` to start at `cached_n_past` instead of 0. **This is the line that moves T2.** |
| 2.4 | Eviction policy | LRU with 4-entry default (4 personas × 4 channels comfortably fits a fixed-size struct; deferring radix-tree until Tier 2). |
| 2.5 | Persona-aware tests | `tests/test_llamacpp_kvcache.c::cache_hit_when_persona_overlay_unchanged` + `::cache_miss_when_overlay_warmth_changes`. Pin the *contract*, not the implementation. |

**Phase 2 exit:**
- T2 (TTFT warm persona) ≤ 250 ms (from 670 ms baseline)
- T4 (cache hit rate) ≥ 80% on a 50-turn fixture conversation
- Sanity gate unchanged

**Risk + mitigation:**
- R1: Persona overlay rendering is non-deterministic (warmth jitter,
  random example sampling). *Mitigation:* the hash inputs are the
  *resolved* overlay + sampled banks, not the random seed. Already-
  rendered text is what we cache.
- R2: Hash collisions silently serve wrong KV. *Mitigation:* extend
  FNV1a to 128-bit (concatenate two 64-bit hashes with different
  seeds). Per [tests-that-pin-bugs.md], a positive contract test
  must assert non-collision under adversarial inputs.

## Phase 3 — Streaming on the MLX inline path + cross-model spec decode (Week 3-5, ~7 days)

**Why third:** Phase 1 and 2 land regardless of which backend is
primary. Phase 3 makes the MLX path competitive with the llama.cpp
path on perceived latency (streaming) and absolute throughput (spec
decode). Two slices, can run in parallel since they touch different
files.

### 3a — Streaming

| # | Slice | File:line |
|---|---|---|
| 3a.1 | Replace `generate()` with `stream_generate()` in the inline handler | [scripts/mlx-server.py:302-305](../../scripts/mlx-server.py#L302). Emit `data: {...}\n\n` SSE chunks per token. Match the OpenAI streaming schema the rest of the codebase consumes. |
| 3a.2 | Wire `supports_streaming = true` + implement `stream_chat` in the MLX provider | [src/providers/mlx.c:513-514](../../src/providers/mlx.c#L513) currently NULL. Mirror the pattern in [src/providers/compatible.c](../../src/providers/compatible.c) which already does SSE parsing. |
| 3a.3 | Test parity | `tests/test_mlx_server_swap.py::test_streaming_total_matches_nonstreaming_total` |

### 3b — Cross-model spec decode (NOT self-spec — see structural caveat)

| # | Slice | File:line |
|---|---|---|
| 3b.1 | Fetch a draft model alongside the 4B/31B target. **Recommended: `google/gemma-4-E2B-it` (2B, model_type `gemma4`, NOT gated, loads in mlx_lm 0.31.2).** Future-ideal: `google/gemma-4-31B-it-assistant` (470M, 4 layers, purpose-built — blocked on mlx_lm adding `gemma4_assistant`). Fallback: `gemma-3-270m-it` (gated, cross-family draft — same 262k vocab so tokens line up). | Extend `scripts/fetch-gemma.sh` to optionally pull a draft checkpoint. |
| 3b.2 | Wire `--draft-model` + `--draft-min-p` + `--draft-max-tokens` for llama.cpp | [src/providers/llamacpp.c](../../src/providers/llamacpp.c) launch path. Default `draft_min_p = 0.05`, `draft_max_tokens = 5` (practitioner-signal consensus values). |
| 3b.3 | Wire the equivalent in mlx-lm via `mlx_lm.server`'s spec decode flag | [scripts/mlx-server.py](../../scripts/mlx-server.py). |
| 3b.4 | Acceptance-rate telemetry | New counter `hu_inference_spec_decode_acceptance_total` / `_attempted_total`. Surface in `/health`. **This is the metric that tells us whether to invest in A3 aligned-draft training.** |

**Phase 3 exit:**
- 3a: Streaming TTFT (first token) ≤ 30% of total elapsed for a 100-token reply
- 3b: T5 (acceptance rate) ≥ 25% on a 50-turn persona fixture
- T1 (TPS) up ≥ 30% over Phase 2 on long-reply scenarios

**Risk + mitigation:**
- R1: Spec decode regresses on short replies (overhead > savings).
  *Mitigation:* gate `--draft-max-tokens` to 0 for replies < 20 tokens
  expected. The tier router already knows reply-length expectations.
- R2: Draft and target tokenizers diverge. *Mitigation:* assert at
  load time that both models share the same tokenizer hash; fail-fast
  with a named error.

## Phase 4 — Flash Attention verification (Week 5, ~1 day)

The CMake / source grep returned no `FLASH` hits. llama.cpp enables FA
by default on Metal in recent builds (b3500+), but our vendored version
may predate that or we may have a `--no-flash-attn` somewhere. One day
to verify, force-enable, and bench.

| # | Slice |
|---|---|
| 4.1 | Audit our llama.cpp version and flags. Set `--flash-attn on` explicitly. |
| 4.2 | Bench delta. If Flash Attention was already on, T1 won't move; if it was off, expect 15-30%. |
| 4.3 | Pin the flag with a test that fails the build if Flash Attention support regresses. |

---

# Tier 2 — Next quarter (high payoff, more invasive)

## Phase 5 — Head-to-head bench: llama.cpp Metal vs MLX (Week 7, ~2 days)

**Why:** Practitioner signal (Ollama's 1.6-2× claim, late Q1 2026) plus
OSS landscape (mlx-lm matures faster than llama.cpp Metal) means the
default backend choice is now load-bearing. We have both paths; we have
the bench harness. We've never run them head-to-head on our actual
persona prompts.

| # | Slice |
|---|---|
| 5.1 | Bench Gemma-3-4B and 31B-4bit on M4 Max with persona prompts on each backend, including Tier-1 wins from Phase 1-3. |
| 5.2 | Per-tier routing decision: route `REFLEXIVE` and `CONVERSATIONAL` through whichever wins by >20% on TTFT + TPS. |
| 5.3 | If MLX wins: switch the default; keep llama.cpp as a fallback for the `ANALYTICAL` tier (which benefits from llama.cpp's mature multi-tier KV management) until Phase 6 lands. |

**Phase 5 exit:** Written ADR at `docs/plans/adr/2026-XX-XX-default-mlx-backend.md` with the bench JSON checked in. Routing config updated. Sanity gate unchanged.

## Phase 6 — Persona-aligned draft adapter (A3 from SOTA roadmap, Month 3)

**Why this is THE strategic bet:** unaligned cross-model draft hits
~25% acceptance (Phase 3b target). A persona-aligned draft — same
LoRA training pipeline you're building for M3 personalization — pushes
that to ≥50% per the existing A3 plan. The same training run
produces both the personalization adapter (M3 closure) and the
spec-decode draft (Track B 1.5-2× decode TPS).

This phase has a real dependency: it needs the M3 Bridge B training
loop from [2026-05-17-m3-mlx-bridge-execution-plan.md](2026-05-17-m3-mlx-bridge-execution-plan.md)
to be passing real tensors, not the probe-counter seam. Sequence:
- Phase 6.1: M3 Bridge B inference phase lands (tensor signal observable)
- Phase 6.2: Train a 270M Gemma persona-aligned draft via the same `mlx_lm.lora` subprocess pipeline
- Phase 6.3: Wire as `--draft-model` in Phase 3b's path
- Phase 6.4: Acceptance rate target ≥ 50% on Tier-1 channels

**Effort:** 3-4 weeks. Most of it is training-loop work the M3 plan
already sequences; the spec-decode side is one CLI flag once the
adapter exists.

## Phase 7 — Cross-user RadixAttention-style prefix sharing (Month 4, GATED)

**Why later:** This requires continuous batching on the MLX path, which
mlx-lm has not yet shipped (multiple sources flagged this in May 2026).
**Gate:** do not start until either (a) mlx-lm ships continuous
batching, OR (b) we decide to write the scheduler ourselves
(~4 weeks). For a single-user-on-Mac product, the cross-user benefit
is small; for a per-channel persona where the same persona renders the
same prefix across hundreds of users on a server build, it's
transformative. Decision deferred to Phase 5's outcome.

---

# Tier 3 — Research bets (only with profiler evidence)

These are NOT in the six-week or quarter plan. They're in this doc so
the rationale is recorded.

| # | Bet | Trigger to start |
|---|---|---|
| 8 | KV-token eviction (KVzip, SnapKV) | Production trace shows ≥10% of turns hit >4K context |
| 9 | Custom Metal sliding-window KV-pack kernel (B4.1) | Profile shows KV ops ≥ 20% of decode time at long context |
| 10 | ANE direct via Orion-style `_ANECompiler` API | Apple ships a public LLM-shaped ANE API at WWDC 2026 |
| 11 | 3-bit weight quant on 31B (QAT or post-training) | MT-Bench-short quality delta < 1.5% verified on draft Gemma-3-31B-3bit |
| 12 | MLA cross-layer KV retrofit on Gemma | Phase 6 ships AND we still want another 2-4× on memory |

# Anti-recommendations (don't waste cycles)

- **BitNet 1.58-bit:** would require distilling Gemma from scratch. Not
  applicable to our existing checkpoints.
- **QuIP# / AQLM 2-bit weights:** marginal vs INT4 at batch=1
  (our case); high calibration cost.
- **Re-opening init-13 (DeltaKV/SWAN):** abandoned for the right reason.
  The KV-quant story is now better served by Q8_0 (Phase 1) and the
  pending TurboQuant/PolarQuant upstream (Phase 8) which work *with*
  the existing cache rather than against it.
- **PLD (Prompt Lookup Decoding)** for open dialogue. Regresses on
  conversational tokens; works only for code/structured output.
- **Direct AMX programming.** Reverse-engineered by dougallj but no
  shipping LLM uses it because CoreML/Accelerate abstraction blocks
  programmer control. Matmul wins already flow into MLX/llama.cpp via
  Apple's CBLAS routing.
- **Self-speculative decoding on M-series.** Doubles memory traffic on
  a bandwidth-bound system; tops out at ~1.35×, below the 2.5×
  profitability threshold per measured community results.

---

# Sequencing — what's actually parallel

```
Week 1   ┃ Phase 0 (measurement)
Week 2   ┃ Phase 1 (KV quant) ──┐
Week 3   ┃ Phase 2 (persona-prefix) ──┐  Phase 3a (streaming)
Week 4   ┃ Phase 2 cont.              │  Phase 3b (spec decode)
Week 5   ┃ Phase 4 (FA verify)        │  Phase 3b cont.
Week 6   ┃ Tier-1 ship + bench       ←┘
─────────╋─────────────────────────────────────────
Week 7   ┃ Phase 5 (backend bench → routing decision)
Week 8+  ┃ Phase 6 sequencing (gated on M3 Bridge B inference)
Month 4  ┃ Phase 7 decision (gated on mlx-lm continuous batching)
```

Phases 1, 2, 3a are independent and parallelizable across two
implementers in worktree isolation per
`~/.claude/rules/agent-team-os.md`.
Phase 3b depends on Phase 1 (KV quant frees enough RSS to load the
draft model alongside the target).

# Verifier contract per phase

Every phase exits through `/verify` returning `RESULT_verifier=PASS`
against the bench harness output **and** the sanity-gate quality
score, not just the test suite. Per
`~/.claude/rules/quality-gates.md`:
"tests pass" is not "behavior verified." For each phase:

1. Run the relevant suite (e.g. `test_llamacpp_kvcache.c` for Phase 2).
2. Run `bench-gemma-perf.py --tag phase-N --compare baseline`.
3. Run `scripts/run-gemma-sanity-gate.sh`.
4. All three must pass before the next phase opens.

# Open questions for sprint planning

| Q | Owner | When needed |
|---|---|---|
| Is our vendored llama.cpp recent enough to expose `--cache-type-k q8_0` and `--draft-model`? Verify `git log third_party/llama.cpp/`. | ML subsystem | Before Phase 1 starts |
| ~~Which Gemma draft do we pull (Gemma-3-270M vs 1B)?~~ **Resolved 2026-05-25:** Recommend `google/gemma-4-E2B-it` for same-family acceptance with mlx_lm 0.31.2 support. The ideal target — `google/gemma-4-31B-it-assistant` (470M, 4-layer) — is blocked on mlx_lm adding its `gemma4_assistant` model_type. | Inference subsystem | RESOLVED |
| Do we have a 50-turn fixture conversation for evaluating cache hit rate (T4)? If not, fixture creation is a prerequisite. | QA | Before Phase 2 exits |
| What's the M3 Bridge B inference phase ETA? Phase 6 gates on it. | ML subsystem | Before sprint-plus-2 planning |

# Provenance

This plan was synthesized from a four-stream parallel research pass
on 2026-05-24 (Apple proprietary APIs, 2025-2026 inference papers,
OSS landscape, practitioner social signal). Cross-stream triangulation
identified KV quantization, persona-prefix caching, and cross-model
speculative decoding as the three high-confidence Tier-1 levers. Per
`~/.claude/rules/audit-verify-before-allege.md`,
every "today's state" claim in the "Honest status" table was verified
by reading the cited file:line directly, not inferred from research
agent reports. Specific numeric speedup targets are taken from the
existing SOTA roadmap (already vetted) and supplemented by
practitioner-reported reproductions where the original plan didn't
have a target. Speedup numbers reported by research-stream agents
were treated as **directional**, not authoritative — the actual win
is measured by the bench harness in Phase 0, not asserted from a
research report.
