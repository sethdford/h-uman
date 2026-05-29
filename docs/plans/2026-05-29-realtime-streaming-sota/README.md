# Realtime Streaming → SOTA: Root Cause & Decision Plan

**Date:** 2026-05-29
**Status:** DECISION NEEDED (server-side change crosses a separate-repo boundary)
**Owner signal:** `scripts/eval_streaming_smoke.py` (the tripwire — exits 0 the day this is fixed)

---

## TL;DR

The realtime-feel blocker is **server-side buffering** in the gemma-realtime
server (`~/Documents/gemma-realtime-1/scripts/mlx-server.py`, a SEPARATE repo).
It is **not** a bug and **not** in the h-uman C code. The in-repo streaming work
(SSE consume in `compatible.c` + `harmony_filter.c` marker stripping, 36 tests)
is genuinely complete.

The buffering is a **deliberate correctness tradeoff**: the `seth-lora-v4-repair`
model deliberates in bare markdown bullets with **no channel markers**, so a
token-by-token filter cannot separate deliberation from the answer until the
full generation exists. **Incremental delivery and clean output are in tension
for this model.** Flipping `cfg.mlx_local.streaming_enabled=true` today delivers
zero latency benefit (and would leak deliberation if the server's buffer flag
were off).

---

## Evidence (read-only investigation, 2026-05-29)

Live server on `:8741`:
```
mlx-server.py --model mlx-community/gemma-4-31b-it-4bit --port 8741 --realtime
  --kv-bits 4 --adapter-path .../seth-lora-v4-repair-20260525-071921
  env: GEMMA_DISABLE_THINKING=1
```

Server already implements BOTH paths:
- `_handle_stream` (line 1670) — **true incremental**: yields one SSE `data:`
  chunk per token at line 1729, with a `StreamThoughtFilter` (line 1067) that
  strips `<|channel>thought` markers incrementally.
- `_handle_stream_buffered` (line 1783) — accumulates the full generation, runs
  the same `finalize_generation` as the non-stream path, emits ONE clean chunk.

Dispatch (line 1690): `if _stream_should_buffer(): self._handle_stream_buffered(...)`.

`_stream_should_buffer()` (line 1039–1060): returns True whenever
`GEMMA_DISABLE_THINKING` is set (it is, on the live process). Override env:
`HU_STREAM_BUFFER_STRIP=0` forces incremental, `=1` forces buffered. **The
decision is global/env-driven, not per-request.**

Live tripwire probe (casual prompt, `eval_streaming_smoke.py`):
```
chunk_count=1, ttft_ratio=1.000, harmony_leaks=[] → buffered, exit 1 (NOT READY)
```
Clean output, single chunk, TTFT==total — exactly the buffered signature.

---

## Why the obvious "fixes" are wrong

- ❌ **Flip `streaming_enabled=true`.** Zero benefit: the server still emits one
  chunk. The C client would just receive the same buffered reply over SSE.
- ❌ **Set `HU_STREAM_BUFFER_STRIP=0` on the server.** Re-enables incremental,
  but the v4-repair model leaks markerless markdown deliberation to the client
  (confirmed in the server's own 2026-05-28 comment at line 1044–1049). You'd
  trade "slow but clean" for "fast but garbage."
- ❌ **Rely on the harmony filter.** It strips `<|channel|>` markers; this model
  doesn't emit them. Wrong tool for markerless deliberation.

---

## Options (honest tradeoffs)

### Option A — Train a parseable answer boundary into the model (the real fix)
Extend the LoRA so the model emits a deterministic sentinel (e.g. a real
`<|channel|>final<|message|>` or a literal `ANSWER:`) between deliberation and
the visible reply. Then the server buffers ONLY up to the sentinel, and streams
every token after it in real time. **Removes the tension** — both clean output
AND incremental delivery.
- **Cost:** one LoRA training run + eval. We own the pipeline (`finetune-gemma.py`)
  and the eval harness (`eval_fidelity_nightly.py` + `eval_multiturn_local.py`).
  Must re-verify persona fidelity didn't regress (+27pp baseline) and watch the
  `scale=2.0` rule (`.claude/rules/lora-scale-default-or-die.md`).
- **Risk:** training-data work (label the boundary in 1963 examples); a few days.
- **Payoff:** the SOTA endgame. Streaming becomes correct, not a hack.

### Option B — Per-turn-type routing (smallest near-term win, partial)
Casual turns skip deliberation (stream incrementally via the marker filter);
analytical/structured turns buffer (stay clean). h-uman's `model_router` already
carries a per-tier `thinking` budget (reflexive/analytical/deep) — that signal
can drive the choice.
- **Cost:** (1) a small SERVER change: make the dispatch honor a per-request body
  flag (e.g. `req.get("stream_strip")`) instead of only the global env; (2) an
  h-uman change to send it per turn-type. **Crosses the separate-repo boundary.**
- **Open empirical question (de-risk FIRST):** do casual turns actually leak
  deliberation under incremental mode, or is buffering overkill for short turns?
  Test by running the existing server code with `HU_STREAM_BUFFER_STRIP=0` on a
  scratch port and probing casual prompts. (Loads a 2nd 31B model — coordinate
  with the nightly-eval serial lock; do not double-load during a run.)
- **Payoff:** realtime feel for the 80% case (casual chat) without retraining.

### Option C — Stream-the-thinking UX (frontier-chat pattern)
Emit deliberation tokens as a distinct SSE role/channel the client renders as a
"thinking…" shimmer, then stream the answer. Realtime feel without hiding
anything, no retrain.
- **Cost:** server + client cooperation; the most UI work. Out of scope for the
  C daemon's text channels; relevant for the native apps.

### ❌ Option D — Speculative decoding / prompt-cache. Both measured as DEAD ENDS
on this hardware (spec-decode 0.69–0.79x; Gemma's RotatingKVCache defeats
trim-based cache reuse). See memory notes. Skip.

---

## Recommended sequence

1. **De-risk Option B's premise** (zero server edit): run the existing server
   code with `HU_STREAM_BUFFER_STRIP=0` on a scratch port, probe 5 casual + 5
   analytical prompts with the tripwire, and measure: do casual turns stream
   clean? If yes → Option B is a small, high-value change.
2. **Ship Option B** (per-request `stream_strip` flag) if (1) confirms casual
   turns are clean incrementally. Smallest path to realtime feel for chat.
3. **Pursue Option A** (sentinel training) as the durable SOTA fix that makes
   ALL turn types streamable-and-clean — folds into the M3 training cadence.

## Acceptance gate (already shipped)

`scripts/eval_streaming_smoke.py` is the standing signal. It exits **0** the day
incremental delivery lands clean (`chunk_count≥2`, `ttft_ratio<0.5`, no leaks),
**1** while buffered/leaking, **2** if the server is down. Wire it into
`nightly_eval.sh` so the regime flip is logged automatically. Extend it to probe
BOTH a casual and an analytical prompt + report absolute TTFT ms when Option B
work begins (so it measures the two regimes the routing fix targets).

---

## What is SOTA realtime conversational AI (the bar)

| Dimension | Target | Status |
|---|---|---|
| Time-to-first-token | < ~300 ms (perceived instant) | ❌ buffered: TTFT==total (tens of seconds at depth) |
| Token-by-token delivery | incremental SSE | ❌ server buffers (this plan) |
| Persona/voice fidelity | held under streaming | ✅ +27pp proven (v4-repair); must re-verify post-Option-A |
| Multi-turn retention | no drift across 20–30 turns | ✅ harness shipped (`eval_multiturn_local.py`) |
| Interruptibility / barge-in | for voice | ⏳ future (native apps + STS path) |

The streaming work in this plan closes rows 1–2. Rows 3–4 are proven. Row 5 is a
later voice-stack concern.

---

## Decision needed

1. **Do we pursue the server-side fix at all now**, or keep buffering (correct,
   just not realtime) until the M3 sentinel-training cadence?
2. **If yes: Option B (route) or Option A (train) first?**
3. **Authorization:** Options A & B require editing the SEPARATE gemma-realtime
   repo (`~/Documents/gemma-realtime-1`), which the current working constraint
   says not to touch. Explicit go-ahead needed before any edit there.
