---
title: Directed voice memos — the model performs the line (F2-voice)
date: 2026-09-27
status: draft — awaiting Seth's review
---

# Directed voice memos — the model performs the line

## Why

The first real "is it me?" test failed. Seth's sister got one memo in his
cloned voice and answered, by voice (transcribed locally with Whisper):

> "Okay, I've lost my freaking shiz. That is not you. That is annoying. I only
> take calls from my brother, not from weirdy, weirdy sauce."

Seth's goal is not to back off; it is to make the memo indistinguishable from
him. Four causes, all verified 2026-09-27:

1. **Nothing performs the line.** The language model writes a *text message*.
   C code guesses one emotion from keywords and inserts pauses by rule
   (`src/tts/emotion_map.c`, `src/tts/transcript_prep.c`). The rewrite that
   would make it spoken (`HU_SPEECH_REWRITE`) is shadow-only and forbidden from
   using tags. voiceai does the opposite: its model is told every word is
   spoken aloud and directs its own delivery
   (`voiceai/src/personas/bundles/ferni/identity/voice-guidance.md`).
2. **A 5-of-57 emotional range.** Cartesia Sonic supports 57 emotions (primary:
   neutral, calm, angry, content, sad, scared); h-uman only ever sends content,
   excited, calm, contemplative, sympathetic. Cartesia: emotion tags "only work
   when the emotion is consistent with the transcript" — a guessed emotion on
   words written for a screen is pulled back toward neutral.
3. **The clone.** An April instant clone recorded before the MV7 arrived. No
   direction can exceed the clone's ceiling (W2 re-clone pending).
4. **Deaf to the answer.** Inbound voice memos reach the reply as `[Audio]`:
   the inbound path only knows cloud STT routed by the reply provider's name,
   and the live reply provider is local. The daemon answered Mindy's memo with
   "Please let me know what information you need" and a Queen song.

## Goal

A voice memo a close family member accepts as Seth. Measured, not asserted
(see Measurement).

## Non-goals

- Live calls (backchanneling, barge-in, endpointing) — separate F2-calls work.
- Deciding *when* to send a memo (the voice decision; 0 of 70 chosen since
  09-21). Tuned separately once memos are good enough to send.

## Design

### Stage map (memo path only)

```
reply text (unshaped) ──► S2 cleanup ──► D1 PERFORM (LLM) ──► D2 VALIDATE (C)
                                              │ any failure          │
                                              ▼                      ▼
                                   today's path (S2 + keyword   S3 drift guard on words
                                   emotion + rule pauses)       S4 safety gates on words
                                                                     │
                                                              D3 NORMALIZE (C) ──► Cartesia sonic-3.6
```

`HU_SPEECH_DIRECTION=off|shadow|live` (default off). It supersedes
`HU_SPEECH_REWRITE`: the rewrite prompt becomes part of D1. OFF is today's
behavior exactly.

### D1 — Perform (one LLM call, only once a memo is chosen)

The model is cast, not instructed. The prompt is a scene, in voiceai's
structure (constraints → voice DNA → palette → examples → golden rule), with
Seth's character instead of Ferni's:

- **Scene:** who Seth is talking to (name, relationship, closeness from the
  persona contact), where and when (local time, day), what they last said
  (including a transcribed inbound memo), and what the reply means to convey
  (the reply text — the *intent*, not a script).
- **Casting line:** "You are Seth, recording a quick voice memo on your phone
  to your sister Mindy. This is a line in a play: say it the way Seth would
  actually say it out loud to her." The model knows the line is heard, not
  read.
- **Voice DNA** (new persona `voice.dna` block, calibrated in §Calibration):
  base pace, default emotion, the emotions Seth uses often / sometimes /
  rarely, signature reactions ("ha", "oh man", "dude" — from his real speech,
  not Ferni's "Hmm."), and how he opens and closes memos.
- **Palette:** Cartesia's tags only: `<emotion value>` (the 57), `<speed
  ratio>`, `<volume ratio>`, `<break time>`, and a laugh token (see Laughter).
- **Examples:** 4–6 Seth-specific performances (warm, teasing, excited,
  consoling), written from the calibration takes.
- **Golden rule (voiceai's):** tags are for emphasis, not every sentence;
  no stage directions, asterisks, or narrated actions.
- **Output:** the spoken line with inline tags. Nothing else.

Model choice is an open decision (below): the local reply model may not
produce valid tags reliably; D2 makes a bad output safe, not good.

### D2 — Validate (pure C, fully unit-tested)

A pure predicate over the model's output (security-predicate-extraction
pattern). Rejects → today's path; repairs are limited to clamping.

- Tags outside the palette, unbalanced markup, SSML Cartesia ignores
  (`<prosody>`), stage directions → reject.
- Emotion value not in the 57 → reject. Speed clamped to the DNA range
  (≈0.85–1.10 inside Cartesia's 0.6–1.5), volume to ≈0.85–1.15 (0.5–2.0),
  breaks ≤ 800 ms.
- Budgets: ≤ 1 emotion change per two sentences and ≤ 3 per memo, ≤ 1 laugh,
  ≤ 1 speed and ≤ 1 volume change (F1's restraint, now enforced on the
  model's choices instead of ours).
- Length within F1's [0.5, 1.6] of the reply.

### S3/S4 on the words

The drift guard (new numbers, dates, names, negation flips, questions) and the
outbound safety gates run on the tag-stripped words, exactly as for F1. The
performance may change delivery, never content.

### D3 — Normalize

Keep transcript_prep's text normalization (numbers, dates, times, phone,
currency, abbreviations, consonant smoothing, sentence segmentation). Skip its
heuristic tagging: in direction mode the tags are the model's.

### Laughter

voiceai writes laughs as text because Cartesia's `[laughter]` "uses stock audio
that doesn't match persona voice". Render both on the new clone and let Seth
pick by ear; the winner becomes the only laugh token in the palette.

### Hearing the answer (prerequisite)

Inbound voice memos are transcribed locally: a Whisper server under launchd
(`mlx_whisper` / whisper.cpp HTTP; `whisper-large-v3-turbo` is already cached)
→ `voice.local_stt_endpoint` → the inbound attachment path tries local STT
first, cloud only as a fallback. Opus `.caf` → 16 kHz WAV first. First check
whether iOS's own memo transcript is already in chat.db. The transcript reaches
the reply as `[Audio transcription: …]` and feeds D1's scene.

## Calibration — Seth's real voice

Seth has sent no self-recorded memos (chat.db: 0 before 09-26), so the DNA
comes from one MV7 session that also produces the W2 re-clone:

1. The W2 clone script (≈60 s clean read) → re-clone on sonic-3.6.
2. Six natural memos, unscripted, to people he'd really send them to (warm,
   teasing, excited, consoling, logistics, a story). Whisper word timestamps
   give his pace (words/min), pause lengths between and within sentences, and
   his real openers, reactions and closers.
3. Those six become the D1 examples and the DNA ranges. Nothing in the DNA is
   invented; every value cites a take.

## Measurement (OFF → SHADOW → LIVE)

- **SHADOW:** D1 + D2 run for every chosen memo and for `voice preview
  --direct`; today's path is what's spoken. Log: validator verdict and reject
  reason, tag counts, emotions chosen, drift result. No real sends.
- **Ear test (Seth):** `voice preview --direct` renders today's path and the
  directed version of the same line on the new clone, side by side, for ten
  real replies. Proceed only if Seth prefers directed on ≥ 8/10.
- **Real-or-clone (W5), before any family member hears it again:** Seth's six
  real calibration takes mixed with six directed renders of comparable lines,
  shuffled, rated real/clone by someone who knows his voice and knows about
  the test. Pass: detection no better than chance (≤ 7/12 correct).
- **Family:** only after W5 passes. Mindy's memo is the recorded baseline:
  1 of 1 detected.

## Error handling

Every failure (provider error, timeout, empty, invalid tags, drift, gate) falls
back to today's path, logged with its reason. A memo is never sent empty, never
sent with tags as spoken words, and never sent both as audio and text.

## Testing

- D2 truth table: each reject class, each clamp, each budget, the
  all-valid case — pure unit tests, no network.
- D1 with a mock provider: a well-formed directed line is spoken as directed;
  invalid output falls back; a line whose words add a fact is not spoken;
  a line whose words trip moderation is not spoken.
- Daemon: direction runs only once a memo is chosen (lazy, like F1's rewrite).
- STT: a fake local endpoint turns a `.caf` into `[Audio transcription: …]`
  batch text; no endpoint → today's behavior.

## Open decisions for Seth

1. **Performance model.** Local GLM (private, weaker at following a tag
   palette) or Gemini 3.x on Vertex (stronger; the reply text and scene leave
   the Mac; `thinkingBudget` must be set). Default proposal: try local first,
   measure D2's reject rate in shadow, switch only if it is high.
2. **Who rates W5.** Someone who knows your voice well and knows it's a test.
3. **Calibration session.** ~20 minutes at the MV7: the clone script plus six
   natural memos.

## Order of work

1. Inbound STT (hear the answer).
2. MV7 session → W2 re-clone + calibration takes.
3. D2 validator + D3 split (pure C, test-first).
4. D1 prompt + voice DNA from the takes; `voice preview --direct`.
5. Shadow → ear test → W5 → family.
