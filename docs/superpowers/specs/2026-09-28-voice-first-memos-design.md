---
title: Voice-first memos — decide before writing, compose a real memo, never a file
date: 2026-09-28
status: approved — Seth 2026-09-28 ("Get it all done")
---

# Voice-first memos

Approved by Seth 2026-09-28 ("Get it all done… it sounds a little cheesy, can
we make it so it doesn't freak people out"). Builds on
`2026-09-27-voice-direction-design.md`.

## Why

Measured on the live daemon, 2026-09-21 → 09-28:

| | |
|---|---|
| voice decisions logged | 70 |
| memos the daemon chose to send | **0** |
| declined: reply < 20 chars / inbound question / no keyword boost / roll / logistics | 31 / 21 / 16 / 1 / 1 |

Every memo so far was sent by hand. Three structural reasons:

1. **The decision reads the finished text.** `hu_voice_decision_classify_ex`
   (src/context/voice_decision.c) runs after the reply exists, keyed on the
   reply's length and keywords, then rolls 5% ("rare").
2. **A memo is a text read aloud.** iMessage caps the reply at 200 chars
   (imessage.c:3454) before generation and the texting shape rules say "one
   main point". Seth's real memos are 20–31 s, 47–58 words, several connected
   thoughts; the daemon's texts are 10–25 words.
3. **A voice memo from them changes nothing**, and "never voice when they ask
   a question" is backwards — a real question is when people talk.

And a fourth, found while designing: when native recording is blocked (Seth is
typing, mic busy), imessage.c falls back to sending the CAF **as a file
attachment**. Seth: "no no caf file".

### "Cheesy" measured

Median pitch, librosa pyin, same line:

| | Hz |
|---|---|
| Seth, six unscripted MV7 memos | 111–138 |
| clone source he read (MV7 clone, live) | 152 |
| live clone renders | 155–176 |
| clone from natural memos | 151 |
| clone from relaxed memos (teasing/consoling/story) | **144**, pitch sd 4.9 st (Seth 5.2–7.3; live clone 3.6) |

Cartesia renders ~2.3 st above its source, so a clone read in a "performing"
voice comes out brighter still — the bright, even, announcer tone. The emotion
tag does not move pitch (171 vs 173 Hz). The copy adds to it: "officially",
"I hope you have a wonderful day" are greeting-card lines no one says in a memo.

## What Seth chose

- Memo content: a **fuller reply to them**, **follow-ups on their life** from
  stored memory, **updates from his life** from verified sources only.
- Triggers: **they sent audio**, **heartfelt moments**, **questions worth
  talking through**. No base-rate roll.
- Approach A: decide first, then write the memo in the same turn.

## Design

### 1. Voice intent, decided before the turn

`hu_voice_intent_decide()` in `src/context/voice_decision.c` — a pure
predicate over facts only (security-predicate-extraction shape):

```
in:  inbound batch text, voice_messages config, has_voice_id,
     seconds since the last memo to this contact (-1 = never)
out: HU_VOICE_SEND_VOICE | HU_VOICE_SEND_TEXT, reason
```

| order | rule | result |
|---|---|---|
| 0 | group chat (a memo would go to the sender's handle, not the group) | TEXT `group` |
| 1 | no voice id / voice_messages disabled | TEXT `no_voice_id` / `disabled` |
| 2 | inbound carries `[Audio transcription: ` | VOICE `they_sent_audio` (spacing does not apply: answering audio with audio is reciprocity) |
| 3 | last memo to this contact < `HU_VOICE_MIN_GAP_SEC` (default 10800 = 3 h; invalid values fall back to it) | TEXT `spacing` |
| 4 | inbound heartfelt — phrases, not bare words: love you, miss you, I miss, proud of, so proud, so sorry, I'm sorry, sorry about, worried, scared, lonely, heartbroken, passed away, congrats, congratulations, crying, cried, upset, sad, grateful | VOICE `heartfelt` |
| 5 | a short (≤ 12 words) question about what time / where / when / address | TEXT `logistics` |
| 6 | inbound ends with `?` and is ≥ 8 words | VOICE `question_worth_talking` |
| 7 | otherwise | TEXT `no_trigger` |

Review 2026-09-28 changed the first draft: heartfelt used bare "love"/"sorry"
("sorry running late" became a memo) and logistics ran first, so "I miss you,
when are you home?" and "I cried when I heard" went as text.

Word-boundary matching throughout (`substring-classifier-pitfalls`:
"unloved"/"missed the bus" style overlaps are accepted risks of a keyword v1;
"missing" does not match "miss").

The spacing fact comes from `proactive_decisions` (trigger `voice_reply`,
`sent = 1`, newest per contact).

### 2. Composing the memo

`HU_VOICE_FIRST=off|shadow|live` (default **off**; gate comment names the
measurement below).

- **off** — today's behavior.
- **shadow** — decide, log `voice_first shadow: decision=… reason=…`, record a
  `proactive_decisions` row (trigger `voice_first`, decline/send, sent = 0).
  The turn and delivery are unchanged.
- **live** — only when the contact is on `HU_VOICE_DELIVERY_ONLY` (the family
  list). On VOICE:
  - a memo directive is prepended to the conversation context (the crisis
    directive's injection point, daemon.c ~6392);
  - `agent->max_response_chars` becomes 640: above the 600 ceiling where the
    texting shape rules ("one main point", "don't answer every sub-point")
    apply, which a memo must not follow;
  - the pre-decision rides on `hu_daemon_final_reply_t.voice_intent` into
    `hu_daemon_voice_reply`, which then skips the post-hoc classifier (it would
    reject the memo as `too_long`) but still runs every safety gate.

The memo directive (exact text in `src/daemon/daemon_voice_first.c`):

> VOICE MEMO: this reply goes out as a voice memo in your own voice, so say it
> the way you'd talk: about 45–80 words, a few connected thoughts, answering
> what they actually said. You can ask about something they told you before if
> it appears above. Share news from your own life only if it is stated above —
> never invent plans, events, places, people or numbers. Keep it low-key and
> understated: no greeting-card lines ("hope you have a wonderful day"), no
> announcements, no hype, at most one exclamation. This overrides the texting
> length rules.

The same anti-cheese rules go into the D1 performance prompt
(`speech_perform.c` k_cast) so directed memos don't add them back.

Things that would shrink a memo back to a text are off for memo turns
(review C2/I1/I2): the G5 length-anomaly guard (via `agent->voice_memo_turn`;
it rejected anything over 6x Seth's ~34-char texting average and logged the
memo as a DPO negative), the quality retry ("tighten up significantly"), the
Turing retry, best-of-N, and burst messaging. And a pre-decided memo is voiced
only if the turn produced one (12-110 words); a slim retry or canned fallback
goes back to the post-hoc classifier. A memo that ends up as text is logged
(`voice_first: memo went as text`).

A group reply is never voiced by the outbound bus, voice-first or not.

Duration check in the post-hoc classifier becomes word-based (2.6 words/s,
Seth's 155 wpm) instead of `chars / 5`.

### 3. Never a file

When native recording is blocked or fails before Send, the iMessage channel no
longer sends the audio as an attachment when `HU_VOICE_NO_ATTACHMENT=1`
(set in the service plist with this rollout). It returns `HU_ERR_IO_BUSY`;
`hu_daemon_voice_reply` reports not-sent and the text path delivers the reply
as text (split into bubbles as usual). Retrying the recording later is
deliberately out of scope: a memo that arrives 10 minutes late to a live
conversation is worse than a text now.

### 4. The voice itself

Switch `seth.json` `voice.voice_id` to the relaxed clone
(`c192c361-ef5e-443a-ac47-a48da03f7d26`) **after Seth listens** to the three
renders. Not code; backed up before the edit.

## Rollout

1. Ship with `HU_VOICE_FIRST=shadow`, `HU_VOICE_NO_ATTACHMENT=1`.
2. A few days of shadow: count decisions by reason; Seth reads the would-be
   voice triggers.
3. LIVE for Mindy only (`HU_VOICE_DELIVERY_ONLY` narrowed), then the rest of
   the family.
4. Measure per contact from chat.db: reply rate and latency after a memo vs
   after a text, and whether they answer in voice. LIVE as the default is
   gated on memos drawing replies at least as often as texts over ≥ 10 memos,
   plus Mindy's W5 real-or-clone rating.

## Testing

- `hu_voice_intent_decide`: one test per rule row, plus word-boundary negatives
  ("missing", "unloved"… "wherever" is not logistics).
- Daemon: LIVE + trigger → the prompt the turn sees contains the memo
  directive and max_response_chars 640; OFF/SHADOW → unchanged; pre-decided
  VOICE bypasses the classifier; contact off the family list → unchanged.
- iMessage: blocked native + `HU_VOICE_NO_ATTACHMENT=1` → no attachment,
  `HU_ERR_IO_BUSY` (the macOS-only path is covered through the pure route/decision
  helper, not a live recording).
- Word-based duration: a 70-word memo passes a 30 s cap; 120 words does not.

## Out of scope

LLM-judged heartfelt detection, a base-rate ramp, deferred memo retry, a
"would-have-said" shadow generation (costs a second turn per message).
