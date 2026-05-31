---
title: "ADR — Voice provider tiering: Cartesia Sonic default TTS, local STT, Kokoro privacy mode"
created: 2026-05-31
status: accepted
deciders: product, engineering
---

# ADR: Voice provider tiering — Cartesia default TTS, local STT, Kokoro privacy mode

## Context

The local-first-voice effort (see
[`docs/plans/2026-05-31-local-first-voice/`](../2026-05-31-local-first-voice/requirements.md))
shipped a fully local voice path: Kokoro TTS and whisper.cpp STT over an
OpenAI-compatible HTTP boundary on loopback (slices 1–2), with duplex/barge-in
deferred (slice 3) pending a real-audio harness.

A 2026-05-31 deep-research pass (5 angles + adversarial verification) on SOTA
TTS/STT for Apple Silicon established one decisive constraint:

> **The "voice cloning + sub-300 ms streaming TTFB + expressiveness" triangle
> does not close locally on Apple Silicon today — not even on an M3/M4 Max.**

- Sub-300 ms local TTFB is verified only for **Kokoro (~100 ms on M4)** — which is
  **fixed-voice (cannot clone)** and only mildly expressive (the model author
  states there is "probably no voice cloning on the horizon").
- The cloning + expressive engines (Chatterbox, Qwen3-TTS, Orpheus, F5, Sesame
  CSM) hit their sub-300 ms numbers on **datacenter GPUs**; on Apple Silicon they
  are MPS-broken→CPU, compute-bound below real-time (need an M-Max), or
  architecturally too slow to stream. Realistic local cloned-voice TTFB:
  ~700 ms–1.5 s on base M-series.
- Several otherwise-attractive engines are **non-commercial** (XTTS-v2 CPML,
  F5-TTS weights CC-BY-NC, Fish/OpenAudio) — license-disqualified.

The product goal is **cloned + humanized voice at conversational latency**. Since
that is physically unattainable on-device today, the only way to deliver it is a
cloud engine. **Cartesia Sonic** is the chosen engine, and is **already
integrated** in the codebase (`include/human/tts/cartesia.h`,
`hu_cartesia_tts_synthesize` with cloned `voice_id` + context-derived `emotion` +
`nonverbals`, model `sonic-3-2026-01-12`; STT `ink-whisper` via
`hu_cartesia_stt_transcribe`). Provider routing already exists in `src/voice.c`
(keyed on `voice.tts_provider` / `voice.stt_provider` / `voice.local_*_endpoint`).

### Verified Cartesia facts (2026-05-31; policy quotes via search snippets — confirm directly before contractual reliance)

| Fact | Value | Confidence |
|------|-------|------------|
| Sonic 3.5 TTFB | **~82 ms**, #1 on Artificial Analysis Speech Arena | High (independent) |
| Voice cloning | Instant/zero-shot from ~3 s; Pro cloning fine-tunes on ≥30 min | High (vendor) |
| Expressiveness | emotion, laughter, speed/pacing, pronunciation control | High (vendor) |
| Price | ~$39 / 1M chars; several× cheaper than ElevenLabs | Med (aggregator) |
| **Training default** | **Cartesia *may train on "Customer Content" by default*; opt-out is a form and "will not affect prior uses"** | High (policy snippet) |
| **Zero Data Retention** | **Optional, not default**; enterprise/onboarding-gated | High (DPA snippet) |
| **Voiceprint retention** | **Reference recordings retained even under ZDR** "as reasonably necessary to deliver voice cloning" | High (DPA snippet) |
| Compliance | SOC 2 Type II, HIPAA, GDPR; deletion 90/180 days post-termination | High (vendor) |
| On-device Sonic | Exists (instant clone + emotion) but **private beta**, sales-gated, status unconfirmed; open "Edge" lib is SSM *LLMs*, not Sonic TTS weights | Med |
| API shape | Cartesia's own REST + WebSocket; **no OpenAI-compatible TTS endpoint** | Med-High |

The brand thesis is *"privacy by architecture — your identity never leaves your
hardware, not a settings toggle."* A cloned **voiceprint is identity**, and
Cartesia's default trains on Customer Content and retains voiceprints even under
ZDR. This is a genuine, named tension — accepted here with binding mitigations,
not waved away.

## Decision

1. **Cartesia Sonic is the DEFAULT TTS** (cloned + expressive + ~82 ms) — the path
   that delivers the product goal local hardware cannot.
2. **STT stays LOCAL by default** (whisper.cpp now; WhisperKit/parakeet-mlx later).
   Raw microphone audio never leaves the device; only the assistant's outbound
   **reply text + the chosen voice model** are sent to Cartesia for TTS.
3. **Local Kokoro is the privacy mode AND the graceful fallback.** A `privacy_mode`
   selects fully-on-device TTS (Kokoro) + local STT — nothing leaves the machine.
   Cartesia unreachable / no API key ⇒ fall back to local (privacy-safe by
   construction).

### Mandatory mitigations (binding — required before the cloud default ships)

Choosing "Cartesia default" is conditional on ALL of the following. They are
requirements, not recommendations:

- **M1 — Enable Zero Data Retention (ZDR)** on the Cartesia account/contract before
  any cloud-default build reaches users. Confirm with Cartesia sales whether ZDR is
  self-serve or plan-gated.
- **M2 — File/automate the training opt-out** for Customer Content (the no-train
  form), and record the date (it "does not affect prior uses").
- **M3 — DPA addendum** explicitly addressing the **voice-clone reference-recording
  retention** carve-out and deletion guarantees.
- **M4 — User-facing disclosure** whenever the cloud tier is active: *"Voice is
  generated by Cartesia (cloud); your reply text and voice model are processed by
  Cartesia."* In privacy mode: *"100% on-device."*
- **M5 — Honest marketing.** Do **NOT** claim "your identity never leaves your
  hardware" on the default tier. That claim holds only in **privacy mode** (or on a
  future Sonic On-Device deployment). The unqualified privacy promise moves from
  "always" to "in privacy mode."
- **M6 — Consent gate** for voice cloning (Cartesia requires consent; the product
  must surface and record it before uploading a voiceprint).

## Tiering architecture

```
STT:  always local (whisper.cpp → WhisperKit/parakeet-mlx)   ← mic audio never leaves device
TTS:  default      → Cartesia Sonic (cloned + expressive + ~82 ms)   ← reply text + voiceprint to cloud (ZDR + opt-out required)
      privacy_mode → local Kokoro (fixed voice, on-device)           ← nothing leaves the device
      fallback     → local Kokoro when Cartesia unavailable          ← privacy-safe by construction
```

## Implementation slice (deferred to a follow-up — this ADR ships no code)

The provider integration already exists; the gap is policy/defaults/disclosure:

1. Add `voice.privacy_mode` (bool) — or `voice.tts_tier` (`cloud`|`local`) — config
   key (parse + validate allow-list + docs).
2. Make TTS default to Cartesia when an API key is present and `privacy_mode` is
   off; route to local Kokoro when `privacy_mode` is on. Keep STT local regardless.
3. Graceful local fallback when Cartesia errors / no key (reuse existing fallback
   chain in `src/voice.c`).
4. A disclosure hook (M4) the UI/channels can surface; a consent gate (M6) before
   first voice-clone upload.
5. Tests: precedence (privacy on→local, off+key→Cartesia), fallback, and that STT
   never routes to Cartesia by default. Follow `tests-that-pin-bugs.md`.

## Consequences

**Positive:** delivers cloned + humanized voice at ~82 ms (the goal); reuses an
already-integrated engine (low build cost); cheaper than ElevenLabs; preserves a
genuine on-device privacy mode as a selectable guarantee; keeps mic audio local
even on the default tier.

**Negative / risks:** the unqualified privacy promise no longer holds by default
(M5); a hard dependency on a cloud vendor for the default experience; the voiceprint
retention carve-out persists even under ZDR (M3); brand tension that must be
managed by honest disclosure rather than architecture.

## Alternatives considered

- **Flip to local default + Cartesia explicit opt-in** — most on-brand and cheapest,
  but weaker default UX. **Rejected** in favor of default-cloud-with-mitigations
  (product call, 2026-05-31).
- **Local-only cloning (Qwen3-TTS / Chatterbox via mlx-audio)** — Apache/MIT,
  on-device, but ~700 ms–1.5 s TTFB on base hardware and unverified Mac latency.
  **Rejected** for the default (latency); remains the engine set if privacy mode
  ever needs cloning on-device.
- **Gate on Sonic On-Device (private beta)** — the real best-of-both (cloud quality
  + local privacy). **Parallel track**: pursue beta access; if it lands, revisit this
  ADR — it would let the cloud default also satisfy the on-device thesis.

## Related

- [`docs/plans/2026-05-31-local-first-voice/requirements.md`](../2026-05-31-local-first-voice/requirements.md) — the local-first effort this tiers on top of
- [`docs/plans/2026-05-31-local-first-voice/slice-3-duplex-barge-in.md`](../2026-05-31-local-first-voice/slice-3-duplex-barge-in.md) — deferred streaming/barge-in (STT is the lever there)
