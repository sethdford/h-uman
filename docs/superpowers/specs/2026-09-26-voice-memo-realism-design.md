---
title: Voice Memo Realism — Native Recording, Safety Parity, Speech Rewrite
description: Make h-uman's iMessage voice memos indistinguishable from Seth recording one himself on his Mac.
category: design
---

# Voice Memo Realism — Design

**Date:** 2026-09-26 · **Status:** draft for review · **Owner:** Seth

## Goal and success criterion

A voice memo h-uman sends should be indistinguishable from Seth pressing record in
Messages on his Mac and talking into his Shure MV7.

**Success is measured, not asserted:** in a blind "real or clone?" test (Seth plus at
least two people who know his voice, ≥40 trials each, content-matched real/synthetic
pairs), clone clips are called "real" at a rate whose 95% CI includes 0.5.

## What we learned before designing (evidence)

| Finding | Source |
|---|---|
| Driving Messages' own **Record audio → Stop → Send** buttons (AX labels, macOS 26.6) with Cartesia audio fed through a BlackHole virtual input produces a genuine memo: chat.db row `is_audio_message=1`, Opus 24 kHz mono ~24.5 kbps, written by Messages. | Spike 2026-09-26, row 73060, self-chat |
| Spike defects: 2.9 s leading silence (ffmpeg device-open latency after Record), −4.7 dB level loss, input "restore" restored the wrong device, Messages left on a different conversation. | Same spike |
| Reactive voice memos **skip moderation/crisis (SHIELD-004/005), companion safety (SHIELD-001) and claim hedging** — all live inside `if (!sent_voice …)` after `hu_daemon_voice_reply()` at `src/daemon.c:8686`. Latent today (iMessage `voice_enabled` unset; 67 decisions, 0 sent). | Read of `src/daemon.c:8686-8880`; callers of `hu_outbound_sanitize` are scheduled/proactive paths only |
| The memo speaks the **text-message-shaped** reply: typo injection (`daemon.c:8615`), lowercase/no-punctuation quirks, text fillers ("haha ", "lol "), and raw abbreviations (lol, lmk, tmrw, ur) all reach Cartesia. | Codebase map; typos confirmed by read |
| Sonic 3.6 instant clones learn from up to **60 s**; older models used only the first 10 s. The current clone (2026-04-03) predates 3.6. `[laughter]` is a documented token; speed 0.6–1.5, volume 0.5–2.0, emotion/break/spell tags. | docs.cartesia.ai clone-voices, tts-models, sonic-3/ssml-tags |
| Spontaneous-sounding speech: filled pauses ("um/uh") at clause boundaries or utterance-initial, not mid-clause (Kirkland et al., Interspeech 2022); varied pause lengths (~150 ms / ~500 ms / ~1.5 s clusters, single source). Current prep uses fixed 350 ms / 150 ms breaks and no um/uh. | Research report 2026-09-26 |

## Workstreams (in order; each is independently shippable)

### W1 — Safety parity for voice (blocker, ships first)

A voice memo must pass every gate a text reply passes. Design: a pure predicate
`hu_voice_reply_gates_clear()` in `src/daemon/daemon_voice_reply.c` runs
`hu_moderation_check`, `hu_companion_safety_check` and `hu_memory_has_claim_language`
on the reply. **Any flag → voice declined**, and the reply falls through to the
existing text path, which applies its replacement/crisis/hedging behavior unchanged.
Voice never carries a moderated, replaced or hedge-needing reply.

Tests (`tests/test_voice_reply_gates.c`): flagged-moderation text → declined;
companion-safety text → declined; claim-language text → declined; clean text → allowed;
and a daemon-level test that a flagged reply with voice forced on is delivered as text.

### W2 — Re-clone on Sonic 3.6 from the MV7

Seth records ~90 s (script given in session) via QuickTime. We cut the best 60 s of
conversational speech (drop room tone, keep natural disfluencies), check peak
(−12…−3 dBFS) and noise floor (≤ −55 dB), clone with `human voice clone`, keep the
April id as `previous_voice_id`. No code change unless the clone CLI lacks a needed
field (then a one-line fix, separate commit).

### W3 — Native delivery through Messages (`HU_VOICE_DELIVERY=attachment|messages`, default `attachment`)

New `src/channels/imessage_voice_record.c` (Apple-only, gated like `imessage.c`):

1. **Preconditions** (each a pure, tested predicate): BlackHole device present; the
   configured real mic (`voice.real_input_device`, e.g. `"Shure MV7"`) is not in use
   by another process (CoreAudio `kAudioDevicePropertyDeviceIsRunningSomewhere`); user
   idle ≥ N s (HIDIdleTime, default 20 s); Messages running. Any failure → fall back to
   today's attachment send (never drop the reply).
2. **Record:** remember the currently selected conversation; set input → BlackHole and
   **read it back**; open target chat; open BlackHole output and pre-roll it; press
   `Record audio` and start playback in the same process (AudioQueue/AVAudioEngine) so
   lead-in is a controlled 350–700 ms; apply measured gain (+4.7 dB, recalibrated at
   startup against a reference tone); tail 500–900 ms; press `Stop`, then `Send`.
3. **Restore, always:** set input → configured real mic (not "previous") and read it
   back; reselect the remembered conversation. Runs on every exit path.
4. **Verify the artifact:** poll chat.db for a new `is_from_me=1, is_audio_message=1`
   row in the target chat within 10 s. Missing → log, record failure, do **not**
   re-send as attachment (avoid duplicates); the proactive_decisions row records it.

AX label lookup reuses the `AXUIElementPerformAction` pattern at `imessage.c:4447`.
Labels (`Record audio`, `Stop`, `Send`, `Cancel audio recording`) are constants in one
table so a macOS rename is a one-line fix. Tests cover the predicates, the label
state machine against a fake AX tree, and restore-on-failure; no real AX/audio in tests.

### W4 — Speak the reply, not the text message (`HU_SPEECH_REWRITE=off|shadow|live`, default `off`)

1. **Split before styling:** capture the reply before `hu_daemon_shape_text_inplace`
   and typo injection; the voice path uses that copy. Typos, lowercase/no-punctuation
   quirks and text fillers never reach TTS.
2. **New `src/tts/speech_rewrite.c`** (deterministic, seeded), runs before
   `hu_transcript_prep`:
   - abbreviations → spoken (lmk→"let me know", tmrw→"tomorrow", ur→"your/you're" by
     next-word rule, idk, ngl, tbh, rn, bc…); URLs → dropped or "I'll send you the link";
   - `lol`/`haha`/`lmao` → **Seth-owned rule** (contributed in implementation: drop,
     `[laughter]`, or keep as a spoken "haha" by position/frequency);
   - filled pauses "um"/"uh" at clause boundaries or utterance-initial only, 2–6 per
     100 words, never two in a row, never mid-clause;
   - pause lengths sampled from {150, 500, 1500 ms}-centred distributions instead of
     fixed values (changes the constants in `transcript_prep.c`).
3. SHADOW logs the rewritten transcript beside the original to
   `~/.human/voice/shadow/`; LIVE sends it. Promotion to LIVE gated on the W5 test.

### W5 — "Real or clone?" measurement

Extend `scripts/blind_ab/voice_ab.py` with a `realness` axis: pairs of a real Seth
memo and a clone memo of the same text (Seth reads the text for the real one), both
delivered through Messages (W3), sent one at a time to raters; answer "real"/"clone".
Score: clone-called-real rate with a 95% CI; writes nothing when n = 0 (no verdict
without a measurement). Gates W4 LIVE and the clone swap.

## Explicitly out of scope

Room-tone/iPhone-mic mastering (not needed: Messages records from the Mac mic path
Seth actually uses); private IMCore APIs; any SIP/security-setting change;
pro voice clone (revisit after W5 if the instant clone misses the bar).

## Risks

- **Mic hijack during a call** — mitigated by the in-use predicate; fallback to attachment.
- **Screen interference** — idle gate; AX acts without raising the window where possible.
- **macOS UI changes** — label table + artifact verification makes breakage loud, not silent.
- **Over-humanizing** (too many ums) — rates capped, SHADOW first, measured before LIVE.
