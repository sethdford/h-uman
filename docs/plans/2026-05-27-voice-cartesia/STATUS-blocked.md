# Voice via Cartesia — Execution Status: BLOCKED on external prerequisites

**Date:** 2026-05-28
**Decision:** Execution deferred per the plan's own STOP instruction. This
session cannot satisfy the external prerequisites; proceeding would only
produce a half-wired path that fails its acceptance criteria (AC-1..AC-5 all
require a real inbound phone call).

## Why blocked

The plan (`docs/plans/2026-05-27-voice-cartesia/tasks.md`) opens with a
PREREQUISITES gate and instructs: *"If any prerequisite fails, STOP and
report. Do not start execution."* The following prerequisites are external
to the codebase and must be provisioned by the operator (Seth) — an agent
cannot create accounts, purchase numbers, stand up public endpoints, or
place a phone call:

| Prereq | Status | Who must provision |
|---|---|---|
| Twilio `account_sid` + `auth_token` | ❓ not in repo (correct — secrets never committed) | Seth |
| Twilio phone number purchased + webhook URL configurable | ❓ external | Seth |
| Cartesia API key valid for TTS **and** STT | ❓ external | Seth |
| Seth's reference audio recorded in Cartesia UI; `voice_id` UUID copied | ❓ external | Seth |
| Public-reachable HTTPS endpoint (ngrok / Cloudflare Tunnel / Tailscale Funnel) reachable from Twilio's cloud | ❓ external | Seth |
| A real phone to dial the Twilio number for AC-1..AC-5 acceptance | ❓ external | Seth |

## What is NOT a blocker (verified in-repo, 2026-05-28)

- **The "biggest sleeper risk" — a voice/reflexive tier passing
  `thinkingConfig.thinkingBudget=0` — is structurally satisfied.**
  `include/human/agent/model_router.h:25` carries `int thinking_budget`
  per tier and `reflexive_model` (line 32) exists; `thinkingBudget` flows
  to the provider via `src/providers/gemini.c` + `src/agent/model_router.c`.
  The operator only needs the voice/reflexive tier's config value set to
  `thinking_budget = 0` (a config edit, not a code gap). Per the CLAUDE.md
  Gemini-3.x gotcha, this MUST be set before Task 6 to avoid empty replies.
- **Existing infra confirmed present:** `src/tts/cartesia.c`,
  `src/tts/cartesia_stream.c`, `src/channels/twilio.c`,
  `src/channels/voice_channel.c`, `src/voice/voice.c` (Whisper STT + optional
  Cartesia STT).
- **`CARTESIA` is confirmed NOT yet a `hu_voice_mode` value** (current
  modes: `SONATA`, `REALTIME`, `WEBRTC` — `voice_channel.h:32-34`),
  consistent with "spec only / unstarted." Task 8 is genuinely greenfield.

## To unblock

1. Seth provisions the six external prereqs above.
2. Web-verify the current Cartesia model_id at sprint start (spec assumes
   `sonic-3-2026-01-12`; confirm it's still active).
3. Re-dispatch the plan starting at Wave 1 (μ-law codec + VAD — these two
   are pure units that COULD be built now with mock providers, but were
   deferred here to keep the sprint atomic and avoid a dangling half-sprint).

No code was written for this item in this session.
