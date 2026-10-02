---
title: Reply-Length Policy (HU_LENGTH_POLICY)
---

# Reply-Length Policy (`HU_LENGTH_POLICY`)

`HU_LENGTH_POLICY=off|shadow|live` (default `off`) sets the 1:1 reply cap from
Seth's own per-contact reply distribution, not from a multiple of the inbound
message's length. Code: `src/agent/turn/length_policy.c`, contract:
`include/human/agent/length_policy.h`.

## Why

The 2026-10-01 humanness audit measured a median reply of 22 characters
against Seth's ~71. The 2026-09-22 specificity measurement had already found
that length explained 74% of the specificity gap. Several mechanisms forced
replies short at the same time:

| Site | Behaviour today (`off`) |
| --- | --- |
| `hu_conversation_max_response_chars_relational` | cap = inbound length x 2.0 (up to 3.25 for a deep relationship), 15-char floor, floored at the contact's measured `reply_chars_p90` |
| `prompt.c` RESPONSE LIMIT line | says "Keep it tight." for every cap of 80 or less. A contact whose p90 is 50 hears it on every turn |
| `hu_conversation_brief_char_cap` | brief mode caps at 50 (group) and 72–260 (1:1), floored at p90 |
| `hu_conversation_calibrate_length_for_contact` | a second "Target: ~N chars max" line computed from the same ratio formula |
| `hu_conversation_evaluate_quality` (quality retry, A/B pick) | asks for "Tighten up significantly" whenever a reply runs past 5x their last message, even inside the cap |
| iMessage `get_response_constraints` = 200 | the starting upper bound for the cap and the fragment-split threshold. It is not a truncation: nothing cuts a reply at 200 |

## What each state does

- **off**: today's cap and wording, byte for byte. `tests/test_length_policy.c`
  pins the cap (against the old daemon block, for 1:1, group and brief turns)
  and the RESPONSE LIMIT line.
- **shadow**: computes the new cap and logs one aggregate line per 1:1 turn,
  then applies today's cap:
  `[HU_LENGTH_POLICY shadow] old_cap=… new_cap=… tight_old=… tight_new=… stats=… p50=… p50_derived=… shape=… inbound_len=… brief=… stage=…`.
  The line has caps, flags, a byte length and enums, never text or a contact.
- **live**: for a 1:1 contact with measured stats:
  - cap = Seth's p90 to that contact;
  - a long (120+ bytes), story-shaped (80+ bytes, several sentences or lines)
    or question-shaped inbound may raise it to inbound x the relationship
    multiplier (2.0 / 2.55 / 3.0 / 3.25 by stage). A short statement never
    raises it;
  - brief mode may lower it, but never below Seth's p50 to that contact;
  - the hard bound is the channel constraint (iMessage 200), the configured
    `behavior.max_response_chars` (300), and 600 absolute. It wins over the
    p50 floor;
  - the RESPONSE LIMIT line says "keep it tight" only when the cap is below
    the p50; otherwise it uses the "stay within it, but sound like a real text
    thread" wording;
  - the calibration directive's "Target: ~N chars" carries the same number;
  - the quality scorer divides over-length by `max(their length, cap / 1.5)`,
    so a reply inside the cap keeps full brevity marks and never triggers the
    5x "tighten up" retry. The too-short checks are unchanged.

  Groups and contacts without stats keep today's cap and wording in every
  state.

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

1. **Refresh the stats**: `python3 scripts/measure_contact_reply_lengths.py --write`,
   then restart the daemon.
2. **SHADOW for 7 days**: `HU_LENGTH_POLICY=shadow` in the service-loop plist.
   Then run `python3 scripts/length_policy_caps.py`, which reads
   `~/.human/logs/service-loop-error.log`. It reports `turns_with_stats`,
   `old_cap_median` against `new_cap_median`, `new_gt_old`/`new_lt_old`, and
   `tight_old_rate` against `tight_new_rate`.
   - Licence to go LIVE: `turns_with_stats >= 50` and
     `tight_new_rate < tight_old_rate`.
   - Also check that `new_lt_old` stays a minority. A policy that mostly
     lowers caps is the wrong fix for replies that are too short.
3. **LIVE canary for 7 days**. Run
   `python3 scripts/reply_length_gauge.py --days 7`. It compares h-uman's
   replies with Seth's per contact and pooled: `median_ratio`, `brevity_rate`,
   `excess_rate`, plus a two-sample KS test (`ks_d`, `ks_p`) over UTF-8 bytes.
   - Keep LIVE if the pooled `median_ratio` rises toward 1.0 against the
     pre-canary run, and `ks_d` falls.
   - Roll back if `excess_rate` more than doubles.
4. **Specificity at matched length, and the human gate**. Rerun the
   specificity measurement on length-matched pairs, then the blind A/B human
   detection gate.
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
