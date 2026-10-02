---
title: Reply-Length Policy (HU_LENGTH_POLICY)
---

# Reply-Length Policy (`HU_LENGTH_POLICY`)

`HU_LENGTH_POLICY=off|shadow|live` (default `off`) makes 1:1 reply length
match Seth's own per-contact distribution. The cap is never shorter than
today's, and it opens up only for questions and stories. Code:
`src/agent/turn/length_policy.c`, contract:
`include/human/agent/length_policy.h`.

## Why

The goal is to match Seth, not to lengthen replies. On 2026-10-01, Seth's
own median reply per contact measured 18–37 bytes across the 6 contacts with
n >= 20 over 60 days.

The 2026-10-01 audit found these short-forcing sites. Today's cap is already
floored at the contact's measured `reply_chars_p90`, so the cap itself was
rarely the problem. The wording and the scorer were:

| Site | Behaviour today (`off`) |
| --- | --- |
| `prompt.c` RESPONSE LIMIT line | says "Keep it tight." for every cap of 80 or less, whatever arrived. A contact whose p90 is 50 hears it on every turn, including when they ask a question |
| `hu_conversation_evaluate_quality` (quality retry, A/B pick) | asks for "Tighten up significantly" whenever a reply runs past 5x their last message, even inside the cap |
| `hu_conversation_calibrate_length_for_contact` | states a second "Target: ~N chars max" that ignores the channel bound and brief mode, so it can disagree with RESPONSE LIMIT |
| `hu_conversation_brief_char_cap` | brief mode caps a question or story the same as small talk |
| `hu_conversation_max_response_chars_relational` | cap = inbound x a per-contact multiplier (2.0–3.25), 15-char floor, floored at p90. **Kept unchanged** as the starting point |
| iMessage `get_response_constraints` = 200 | the upper bound for the cap and the fragment-split threshold. It is not a truncation |

## What each state does

- **off**: today's cap, wording and scoring, byte for byte.
  `tests/test_length_policy.c` pins the cap against the old daemon block (1:1,
  group, brief) and pins the RESPONSE LIMIT line.
- **shadow**: logs one aggregate line per 1:1 turn and applies today's cap:
  `[HU_LENGTH_POLICY shadow] old_cap=… new_cap=… tight_old=… tight_new=… stats=… p50=… p50_derived=… shape=… inbound_len=… brief=… stage=…`.
  The line holds caps, flags, a byte length and enums. It never holds text or
  a contact.
- **live**: applies only to a 1:1 contact with measured stats.
  - **Cap.** It is `max(today's cap, Seth's p50)`. Today's cap keeps its
    per-contact multipliers, the 15-char floor and the p90 floor, so LIVE can
    never be shorter than OFF.
  - **Questions and stories.** A question- or story-shaped inbound also
    escapes the brief-mode cap. Story means 120+ bytes, or 80+ bytes with
    several sentences or lines.
  - **Hard bound.** The channel (200), `behavior.max_response_chars` (300)
    and 600.
  - **RESPONSE LIMIT wording.** "Keep it tight" only when the inbound is short
    and casual: no question, no story, and shorter than Seth's p50 to that
    contact. Otherwise the line states the bare limit.
  - **Calibration.** The calibration "Target: ~N chars" states the same number
    as RESPONSE LIMIT.
  - **Quality retry and A/B pick.** The over-length check divides by
    `max(their length, cap / 1.5)`, so a reply inside the cap keeps full
    brevity marks and never triggers the 5x "tighten up" retry. The too-short
    checks are unchanged.

Groups, contacts without stats, and voice-memo turns keep today's cap,
wording and scoring exactly in every state.

### Where p50 comes from

`reply_chars_p50` is an optional persona contact field.
`scripts/measure_contact_reply_lengths.py --write` now writes it next to
`reply_chars_p90`. When only p90 is present, p50 = p90 / 3.

That ratio was measured on 2026-10-01 with the script's `measure()` over 60
days, using owner-authored sends only, n >= 20, aggregate only. Six contacts
had a p50/p90 median of 0.37 (range 0.16–0.53). One third sits just under that
median, so a guessed p50 rarely exceeds the real one.

The same run measured six contacts while the live persona held p90 for only
four, and those four values were stale. Run the script with `--write` before
starting SHADOW.

## Promotion: SHADOW to LIVE

1. **Refresh the stats.** Run
   `python3 scripts/measure_contact_reply_lengths.py --write`, then restart
   the daemon.
2. **Run SHADOW for 7 days.** Set `HU_LENGTH_POLICY=shadow` in the
   service-loop plist. Then run `python3 scripts/length_policy_caps.py`, which
   reads `~/.human/logs/service-loop-error.log`.
   - **`new_lt_old` must be 0.** The construction makes a lower cap
     impossible, so the script exits 1 if it ever sees one. That is a bug, not
     a verdict.
   - **`new_gt_old` should come from question and story inbound.** Check
     that `new_gt_old_shaped` is most of it.
   - **The tight rate should fall:** `tight_new_rate < tight_old_rate`.
   - **Enough traffic:** `turns_with_stats >= 50`.
   - A missing log, or no turn with stats, exits 2 with no verdict.
3. **Run a LIVE canary for 7 days.** Run
   `python3 scripts/reply_length_gauge.py --days 7`. Per contact and pooled,
   it compares h-uman's replies with Seth's: `median_ratio`, `brevity_rate`,
   `excess_rate`, and a two-sample KS test (`ks_d`, `ks_p`) over UTF-8 bytes.
   - **Keep LIVE** if the pooled `median_ratio` moves toward 1.0 and `ks_d`
     falls against the pre-canary run.
   - **Roll back** if `excess_rate` more than doubles. The target is Seth's
     distribution, not longer replies.
4. **Check specificity at matched length, then the human gate.** Rerun the
   specificity measurement on length-matched pairs. Then run the blind A/B
   human detection gate.
   - The 2026-07-27 baseline is detection 0.225 PASS, CI [0.123, 0.350].
   - Keep LIVE only if detection does not rise past that CI's upper bound.

## Rollback

```bash
/usr/libexec/PlistBuddy -c "Set :EnvironmentVariables:HU_LENGTH_POLICY off" \
  ~/Library/LaunchAgents/ai.human.service-loop.plist
launchctl bootout gui/$(id -u)/ai.human.service-loop
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/ai.human.service-loop.plist
```

`off` restores today's behaviour exactly. No state is written, so there is
nothing else to undo.

## Not changed here (operator follow-ups)

- **The persona file itself.** It still asks for short replies in
  `style_rules[0]` ("5-20 words"), `channel_overlays.imessage.avg_length`
  ("5-15 words", "1-5 words"), `channel_overlays.unknown.avg_length`
  ("3-10 words", "MAX 15 words"), `channel_overlays.unknown.style_notes[2]`
  ("Be brief"), and one contact's `dynamic` ("3-8 words"). That is operator
  data. Reconcile it with the measured distribution by hand.
- **`HU_TERSENESS=live`** (set in the service-loop plist) appends "short,
  usually one line … Match the other person's brevity" to every reply prompt.
  Under LIVE that pulls against the new cap, so measure the two together.
- **Brevity-mirroring lines in the calibration and style blocks**: "Rapid-fire
  exchange — keep responses extra short" and "They send short rapid-fire
  messages. Keep yours very short too" (`src/context/conversation.c`).
