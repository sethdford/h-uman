---
title: F1 — Voice Memos That Sound Spoken (Ferni Port, Async)
description: Rewrite voice replies for the ear with Ferni's mechanics in Seth's register, and replace transcript_prep's random humanization with Ferni's restraint budgets.
category: design
---

# F1 — Voice Memos That Sound Spoken

**Date:** 2026-09-27 · **Status:** approved direction (option B), spec for review · **Owner:** Seth
**Parent:** `docs/superpowers/specs/2026-09-26-voice-memo-realism-design.md` (F1 absorbs its W4)

## Goal

A voice memo h-uman sends should sound like Seth *saying* the reply, not reading a
text message aloud. Success is measured, not asserted: the blind A/B drip
(`scripts/blind_ab/voice_ab.py`) prefers F1 output over the current pipeline, and
the "real or clone?" test (W5) moves toward chance.

## What we found (evidence)

| Finding | Source |
|---|---|
| The reply is generated (`hu_agent_turn`, `src/daemon.c:7122`) before voice is decided (`hu_daemon_voice_reply`, `src/daemon.c:8687`, from features of the finished text). A memo speaks the **text-message** reply: typos, lowercase quirks, "haha/lol" fillers, raw "lmk/tmrw". | Codebase map 2026-09-26; read of daemon.c |
| `transcript_prep` adds humanization **at random**: `pick_nonverbal` fires on 10/20/25% of sentences and a third of those are `[laughter]` even when nothing is funny (`src/tts/transcript_prep.c:909-944`); "Hmm, /Well, /So, " openers at 40% (`:948-977`); "honestly/you know/I mean" at 30% (`:897-906`); a pause at every comma (`:827-876`); all enabled in `src/tts/voice_reply.c:35-38`. | Ferni research report 2026-09-27 |
| Ferni (voiceai) does the opposite. **Write for the ear**: the LLM is told its text goes straight to TTS; short sentences, fragments, self-corrections, reactions ("Oh!", "Wait—"), no stage directions or narrated thinking, no "Well…/So…" openers, no "good question" (`voiceai/src/personas/bundles/ferni/identity/voice-guidance.md`, `src/intelligence/context-builders/humanization/dynamic-speech-guidance.ts:36-190`). **Restraint**: fillers `probability 0.12, maxPerResponse 2` (`src/speech/config/speech-config.ts:106-121`); laughter only when contextually funny, `laughProbabilityBase 0.18`, `minTurnsBetweenLaughs 6` (`src/speech/adaptive-ssml/contextual-laughter.ts:235-242`); "Don't overuse SSML… SSML is for emphasis, not every sentence." | voiceai source |
| Ferni voice defaults: speed 0.95, volume 1.0; h-uman Seth persona: speed 0.85. | Ferni manifest; `~/.human/personas/seth.json` |
| No voice memo is live anywhere: iMessage `voice_enabled` is unset (67 decisions, 0 sent). | proactive_decisions query 2026-09-26 |

Ferni's *character* (a life coach: "That landed in my chest") is **not** ported —
the memos speak as Seth. Only the mechanics are.

## Design

### Pipeline (voice path only; text replies unchanged)

```
reply (as generated)
  └─► S1 rewrite for the ear  (LLM, HU_SPEECH_REWRITE)         src/tts/speech_rewrite.c
  └─► S2 deterministic cleanup (always)                         src/tts/speech_cleanup.c
  └─► S3 drift guard vs. the original reply                     src/tts/speech_rewrite.c
  └─► S4 W1 safety gates on the SPOKEN text                     hu_voice_reply_gates_clear
  └─► transcript_prep with Ferni restraint (S5)                 src/tts/transcript_prep.c
  └─► Cartesia
Any S1/S3/S4 failure → use the S2-only text (cleanup of the original). S4 failure on
the S2-only text too → decline voice (the reply goes out as text, per W1).
```

### S1 — Rewrite for the ear (LLM)

- One call through the agent's provider (`agent->provider.vtable->chat_with_system`,
  the pattern at `src/daemon.c:9505`) with a system prompt and the reply as the user
  message. `max_tokens` bounded (reply length × 2, cap 400); Gemini-family calls set an
  explicit thinking budget of 0 (CLAUDE.md gotcha).
- **System prompt** = the ported mechanics + Seth's register, assembled from:
  1. A fixed block (new data file `data/prompts/speak_it.txt`, embedded like the other
     prompt blobs) with the Ferni mechanics, rewritten for a voice memo from Seth to
     someone he knows: say the same thing, the way you'd say it out loud; short
     sentences and fragments; contractions; a reaction word when one is natural; no
     stage directions, brackets, emoji, asterisks or narrated thinking; never open with
     "Well", "So", "Hmm" or "Good question"; don't add facts, names, times, numbers,
     promises or questions that aren't in the text; keep it about as long.
  2. Seth's persona style fields already on `hu_persona_t` (`preferred_vocab`,
     `avoided_vocab`, `slang`, `anti_patterns`; `include/human/persona.h:382-429`) — so it
     sounds like him, not like Ferni.
- Output: plain words only. **No SSML from the LLM** — tags stay code-owned (S5).
- Gated `HU_SPEECH_REWRITE=off|shadow|live` (default off; LIVE > SHADOW > OFF).
  SHADOW runs S1–S4 and writes `{original, cleaned, rewritten, spoken, verdict}` to
  `~/.human/voice/shadow/<date>.jsonl` (via `hu_paths_state`), then speaks the S2-only
  text. Gate comment names the measurement (A/B drip preference for `rewrite` over
  `cleanup`, then W5).

### S2 — Deterministic cleanup (always on the voice path)

Pure function `hu_speech_cleanup(in, out)`:
- Start from the right text: typos and texting quirks cannot be reliably undone after
  the fact, so **the voice path reads a copy of the reply taken before
  `hu_daemon_shape_text_inplace` and typo injection** (`src/daemon.c` ~8576–8615; the
  copy is taken only when voice is possible for the channel).
- Strip `*stage directions*`, `(asides that narrate)`, brackets other than `[laughter]`,
  emoji, URLs (→ "I'll send you the link" only if the URL is the whole message; else
  dropped).
- Spoken forms: lmk→"let me know", tmrw→"tomorrow", idk→"I don't know", rn→"right now",
  bc→"because", ngl→"not gonna lie", tbh→"honestly", omw→"on my way", ur→"your"/"you're"
  (by next word), u→"you"; `lol`/`haha`/`lmao` → **Seth-owned rule** (see Open points).
- Word-boundary matching only (`~/.claude/rules/substring-classifier-pitfalls.md`).

### S3 — Drift guard (pure)

`hu_speech_rewrite_drift_ok(original, rewritten)` rejects the rewrite when it:
- introduces a digit sequence, time, date or currency amount absent from the original;
- introduces a capitalized name/proper noun absent from the original (case-folded
  token compare after cleanup);
- introduces a question mark when the original had none, or drops all of the
  original's question marks;
- is shorter than 0.5× or longer than 1.6× the cleaned original (words);
- contains any of the banned openers/stock phrases or bracket/asterisk syntax.
Rejected ⇒ S2-only text; the reason is logged (never the text).

### S4 — Safety on what is spoken

`hu_voice_reply_gates_clear` (W1) runs again on the final spoken text with the same
inbound message. The W1 call on the original reply stays where it is.

### S5 — Restraint in `transcript_prep`

- `[laughter]`: only when the **spoken text** itself laughs (a sentence containing
  "ha", "haha", "that's hilarious"-class cues after cleanup) or the inbound message is
  laughing; then at most once per memo, probability 0.18, never in two consecutive
  memos to the same contact (state: last-laughed timestamp per contact in memory, not
  persisted). Never for sympathetic/contemplative emotion.
- Thinking-sound openers ("Hmm, /Well, /So, "): **removed** (Ferni bans them; S1
  produces natural openings). `thinking_sounds` defaults false in
  `hu_voice_reply_build_request`.
- Discourse markers ("honestly, / you know, / I mean, "): **removed** from prep (S1 owns
  wording).
- Clause breaks: only before contrast words (but/though/although/however) and at em
  dashes — not at every comma. Sentence breaks keep emotion-change lengthening.
- SSML budget per memo: ≤ 2 emotion changes, ≤ 1 speed tag, ≤ 1 volume tag; the rest
  inherit the base. Enforced in the assembly loop.
- These change existing transcript_prep output. No voice memo is live (see evidence),
  so they land directly (not behind a flag); `human voice preview` shows the before/
  after for Seth. Existing transcript_prep tests that pin random laughter/openers are
  updated with a comment explaining the old assertion.

### Cartesia defaults

Speed 0.95 (Ferni) vs 0.85 (current) is **not** changed by default: it becomes a
measured question via the existing voice A/B `speed` axis. Same for default emotion.

## Units and interfaces

| Unit | Responsibility | Tested by |
|---|---|---|
| `src/tts/speech_cleanup.c` | S2, pure | table tests incl. "yeah lol that sounds good, lmk when ur free tmrw" |
| `src/tts/speech_rewrite.c` | S1 prompt assembly + provider call (injected), S3 drift guard, mode parse, shadow record | mock provider; drift-guard truth table; mutation checks |
| `src/tts/transcript_prep.c` | S5 restraint | updated + new tests: no `[laughter]` without a cue; no openers; clause-break placement; SSML budget |
| `src/daemon/daemon_voice_reply.c` | wiring: pre-shape capture → S1..S4 → prep; SHADOW/LIVE | daemon-level test via the deterministic fallback arm (as W1) |
| `data/prompts/speak_it.txt` | the ported mechanics | reviewed by Seth |

## Open points (decide during planning)

1. **lol / haha / lmao in speech** — Seth's call (a 5–10 line rule): drop it, turn it
   into a real laugh (feeds S5's cue), or say "haha". Default until decided: drop, and
   count it as a laughter cue.
2. Whether S1 sees the inbound message (better tone matching) — default yes, as context
   only, with "reply to this, don't answer it again" in the prompt.

## Out of scope

Realtime/duplex humanization — backchanneling, endpointing, early receipts, barge-in
recovery, echo guard — is **F2**, a separate spec. Re-cloning (W2) and the voice
choice (the running Ferni-vs-clone A/B) are independent.

## Risks

- **Rewrite changes meaning** → S3 drift guard + S4 gates + SHADOW first.
- **Over-casual or out-of-character wording** → persona style block; SHADOW samples
  reviewed by Seth before LIVE.
- **Extra LLM latency/cost** → one bounded call per memo; memos are rare (5–30% of
  eligible replies) and asynchronous.
- **Restraint makes memos flatter** → measured by the A/B drip; budgets are constants
  in one place.
