# Slice 3 — Duplex / Barge-in over the Local Path (Findings & Scope)

> **Status:** SCOPED / DEFERRED. Authored 2026-05-31 from a verify-first audit of
> `src/voice/duplex.c`, `src/voice/semantic_eot.c`, and their sole consumer
> `src/gateway/cp_voice_stream.c`. Conclusion: slice 3 has **no clean,
> unit-testable code deliverable** of the kind slices 1–2 had; the real work
> needs a real-audio test harness (and likely a streaming protocol change).
> This doc captures what was verified and the precisely-scoped future tasks so
> the work is de-risked, and so nobody "fixes" a non-bug. See
> [requirements.md](requirements.md) and [design.md](design.md).

## What was verified (not assumed)

- **`duplex.c` (the micro-turn FSM) is mature and pure.** `IDLE → USER_TURN →
  AGENT_TURN → OVERLAP/BACKCHANNEL` transitions, barge-in (`HU_TURN_SIGNAL_INTERRUPT`
  → `OVERLAP` → `HU_TURN_ACTION_CANCEL_GENERATION`), a fallback silence-timeout
  mode, and first-byte-latency metrics are all present, in-memory, and already
  covered by `tests/test_voice_duplex.c`. No concrete defect found.
- **`semantic_eot.c` is mature and pure.** Three layers — a heuristic cascade
  (`hu_semantic_eot_analyze`), text+acoustic fusion (`_analyze_with_audio`), and a
  learned logistic model (`_classify` over a 10-D feature vector) — with a config
  (`pause_threshold_ms` default 400, `confidence_threshold` 0.6). Covered by the
  `test_sota_*` suites. No concrete defect found.
- **There is exactly one consumer** of the EOT classifier:
  `src/gateway/cp_voice_stream.c` (the gateway voice-streaming flush). It calls
  `hu_semantic_eot_classify(..., silence_ms = 0, ...)` — i.e. the silence
  dimension (the `pause_threshold_ms` boost and the `silence_norm` feature) is
  **inert at the only place EOT runs.**

## Why `silence_ms = 0` is defensible (NOT a bug to "fix")

The streaming flush is a **discrete-chunk** protocol: the client uploads a
complete audio chunk, the server records it to a temp file, runs STT
(`hu_voice_stt_file` — the slice-1 local path when configured), extracts audio
features, and runs EOT on the whole transcript. Verified facts that make `0`
reasonable:

- The stream slot (`vs_slot_t`) tracks **no inter-chunk timing** (no
  `last_chunk_ms` / `last_speech_ms`).
- The voice protocol carries **no client-provided silence or timestamp**.

So there is no reliable server-side "silence since last speech" to pass. Feeding
a naive `now − previous_flush` would be **wrong**, especially on the local path
(see next section). `0` correctly says "no trustworthy silence signal" and lets
EOT decide on text + acoustics alone.

> ⚠️ **Anti-pattern, do not do this:** set `silence_ms` to wall-clock elapsed time
> between chunk flushes. On the local (non-streaming) STT path that elapsed time
> is dominated by the STT **round-trip latency**, not by the user pausing — so
> the EOT silence boost would fire on the server's own processing delay and cause
> **premature end-of-turn** (the agent talks over a user who never paused). This
> is the core "local server slower than cloud" hazard.

## The real phenomenon (and the shape of the eventual fix)

On a streaming **cloud** STT, partial transcripts arrive continuously and
"silence since last speech" is measurable, so the 400 ms `pause_threshold_ms`
behaves as designed. On the **local** path (whisper.cpp `/inference`,
non-streaming), the transcript only exists after a synchronous round-trip, so:

1. there is no incremental signal to drive mid-utterance turn-taking, and
2. any elapsed-time silence measure is inflated by STT latency.

The eventual fix is a **latency-corrected silence**, computed by a pure helper so
it stays unit-testable:

```
corrected_silence_ms = max(0, measured_silence_ms − stt_round_trip_ms)
```

…but it only has meaning once a real silence source exists (a streaming protocol
with continuous VAD, or client-supplied speech timestamps). The STT round-trip
*is* measurable today (a `vs_now_ms()` delta around `hu_voice_stt_file`); the
**user-pause** half is not, in the current protocol.

## Scoped future tasks (with verification requirements)

| # | Task | Blocked on | How to verify |
|---|------|-----------|---------------|
| 3a | Add client speech timestamps (or a streaming/VAD frame protocol) to the voice stream so a real `measured_silence_ms` exists server-side. | protocol/UI change | gateway tests for the new field; UI demo-gateway mirror |
| 3b | Pure helper `hu_semantic_eot_correct_silence(measured_ms, stt_round_trip_ms)` + wire it at `cp_voice_stream.c` (measure the STT round-trip around `hu_voice_stt_file`). | 3a (for a real `measured_ms`) | unit tests for the helper (truth table); gateway threat note |
| 3c | Tune `pause_threshold_ms` / classifier threshold for the local path against recorded conversations. | **real-audio harness** | offline eval on a labelled turn-taking corpus — cannot be a unit test |
| 3d | Confirm barge-in latency: user-interrupt → `CANCEL_GENERATION` → TTS stop, end-to-end, when the local server is slower than cloud. | real-audio harness | timed end-to-end probe, not a unit test |

## Decision

Defer slice-3 **code** until a real-audio test harness exists; shipping
threshold/turn-taking changes without one would be guessing, and the project
rules forbid speculative abstractions (no caller) and unverifiable behavior
changes. **Slices 1–2 (local STT + TTS turnkey) are the shippable wins.** This
doc converts slice 3 from "polish" into the precise, verifiable tasks above.
